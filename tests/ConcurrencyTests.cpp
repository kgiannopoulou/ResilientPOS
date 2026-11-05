// The multi-threaded server: many clients at once, racing on the same IDs.
//
// Every test here is also a ThreadSanitizer test: CI runs the whole suite under
// TSan, so a data race fails the build even if the assertions happen to pass.
//
// Catch2's assertion macros are not thread-safe, so worker threads only count
// what they saw and the test thread does the REQUIREs.

#include "resilientpos/network/HttpServerClient.hpp"
#include "resilientpos/server/HttpTransactionServer.hpp"
#include "resilientpos/server/NetworkSimulationMode.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/server/TransactionHandler.hpp"
#include "resilientpos/storage/StorageError.hpp"
#include "RunningServer.hpp"
#include "TempDatabase.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::network::HttpServerClient;
using resilientpos::network::SendResultType;
using resilientpos::server::handleTransactionRequest;
using resilientpos::server::HttpTransactionServer;
using resilientpos::server::NetworkSimulationMode;
using resilientpos::server::ProcessedTransactionStore;
using resilientpos::server::RecordOutcome;
using resilientpos::server::ServerOptions;
using resilientpos::test::RunningServer;
using resilientpos::test::TempDatabase;

namespace {

// Runs `body(i)` on `threads` threads, released together for maximum contention.
void inParallel(std::size_t threads, const std::function<void(std::size_t)>& body)
{
    std::latch startingLine{static_cast<std::ptrdiff_t>(threads)};
    std::vector<std::jthread> workers;
    workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers.emplace_back([&, i] {
            startingLine.arrive_and_wait();
            body(i);
        });
    }
}

struct Tally {
    std::atomic<int> inserted{0};
    std::atomic<int> alreadyExists{0};
    std::atomic<int> conflict{0};

    void add(RecordOutcome outcome)
    {
        switch (outcome) {
        case RecordOutcome::Inserted:      ++inserted; break;
        case RecordOutcome::AlreadyExists: ++alreadyExists; break;
        case RecordOutcome::Conflict:      ++conflict; break;
        }
    }
};

} // namespace

// --- The store -----------------------------------------------------------------

TEST_CASE("Racing threads insert one ID exactly once", "[concurrency][store][idempotency]")
{
    TempDatabase db;
    ProcessedTransactionStore store{db.path(), 8};
    Tally tally;

    inParallel(16, [&](std::size_t) { tally.add(store.record("same-id", 700)); });

    REQUIRE(tally.inserted == 1);
    REQUIRE(tally.alreadyExists == 15);
    REQUIRE(store.count() == 1);
}

TEST_CASE("The database alone keeps separate writers from double-inserting", "[concurrency][store][idempotency]")
{
    // No shared pool, no shared mutex, nothing in the process that could
    // serialize them: this is two server instances on one file. Only the
    // PRIMARY KEY and the atomic INSERT stand between them and a duplicate.
    TempDatabase db;
    constexpr std::size_t kInstances = 6;
    std::vector<std::unique_ptr<ProcessedTransactionStore>> instances;
    for (std::size_t i = 0; i < kInstances; ++i) {
        instances.push_back(std::make_unique<ProcessedTransactionStore>(db.path(), 1));
    }

    constexpr int kIds = 25;
    std::atomic<int> inserted{0};
    inParallel(kInstances, [&](std::size_t instance) {
        for (int id = 0; id < kIds; ++id) {
            if (instances[instance]->record("id-" + std::to_string(id), 100 + id)
                == RecordOutcome::Inserted) {
                ++inserted;
            }
        }
    });

    REQUIRE(inserted == kIds);
    REQUIRE(instances.front()->count() == kIds);
}

TEST_CASE("Racing different payloads for one ID: one wins, the rest conflict", "[concurrency][store][idempotency]")
{
    TempDatabase db;
    ProcessedTransactionStore store{db.path(), 8};
    Tally tally;

    // Sixteen threads, each with its own amount: no two payloads match.
    inParallel(16, [&](std::size_t i) {
        tally.add(store.record("contested", 1000 + static_cast<std::int64_t>(i)));
    });

    REQUIRE(tally.inserted == 1);
    REQUIRE(tally.alreadyExists == 0);
    REQUIRE(tally.conflict == 15);
    REQUIRE(store.count() == 1);
    const auto stored = store.amountFor("contested");
    REQUIRE(stored.has_value());
    REQUIRE(*stored >= 1000);
    REQUIRE(*stored < 1016);
}

TEST_CASE("More threads than connections wait for one instead of failing", "[concurrency][store]")
{
    TempDatabase db;
    ProcessedTransactionStore store{db.path(), 2};
    std::atomic<int> wrong{0};

    inParallel(12, [&](std::size_t thread) {
        for (int i = 0; i < 5; ++i) {
            const auto id = std::to_string(thread) + "-" + std::to_string(i);
            if (store.record(id, 100) != RecordOutcome::Inserted || store.amountFor(id) != 100) {
                ++wrong;
            }
        }
    });

    REQUIRE(wrong == 0);
    REQUIRE(store.count() == 60);
}

TEST_CASE("An in-memory store is safe to share between threads", "[concurrency][store]")
{
    ProcessedTransactionStore store{":memory:", 8};  // always one connection

    inParallel(8, [&](std::size_t thread) {
        for (int i = 0; i < 20; ++i) {
            (void)store.record(std::to_string(thread) + "-" + std::to_string(i), 100);
        }
    });

    REQUIRE(store.count() == 160);
}

TEST_CASE("A locked database reports busy, and the server answers 503", "[concurrency][store]")
{
    TempDatabase db;
    ProcessedTransactionStore store{db.path(), 1, 20ms};

    sqlite3* blocker = nullptr;
    REQUIRE(sqlite3_open(db.path().c_str(), &blocker) == SQLITE_OK);
    REQUIRE(sqlite3_exec(blocker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);

    REQUIRE_THROWS_AS(store.record("abc", 700), resilientpos::storage::StorageBusyError);
    const auto response = handleTransactionRequest(
        R"({"transaction_id":"abc","amount_cents":700})", store);
    REQUIRE(response.httpStatus == 503);

    REQUIRE(sqlite3_exec(blocker, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(blocker);
    REQUIRE(store.record("abc", 700) == RecordOutcome::Inserted);
}

// --- The HTTP server -----------------------------------------------------------

TEST_CASE("Requests really are served in parallel", "[concurrency][http]")
{
    // Each request waits inside the server until kParallel of them are in
    // there together, which can only ever happen on kParallel different
    // threads. With a single I/O thread the first request would sit out the
    // whole timeout alone and `peak` would stay at 1.
    constexpr int kParallel = 3;
    std::mutex mutex;
    std::condition_variable allArrived;
    int inside = 0;
    int peak = 0;

    ServerOptions options;
    options.ioThreads = kParallel;
    options.beforeProcess = [&] {
        std::unique_lock lock{mutex};
        peak = std::max(peak, ++inside);
        allArrived.notify_all();
        allArrived.wait_for(lock, 5s, [&] { return peak >= kParallel; });
        --inside;
    };

    TempDatabase db;
    ProcessedTransactionStore store{db.path(), kParallel};
    RunningServer server{store, 0, options};

    std::atomic<int> accepted{0};
    inParallel(kParallel, [&](std::size_t) {
        HttpServerClient client{"127.0.0.1", server.port(), 20s};
        if (client.send(Transaction::create(Money{700})).type == SendResultType::Accepted) {
            ++accepted;
        }
    });

    REQUIRE(accepted == kParallel);
    REQUIRE(peak == kParallel);
    REQUIRE(store.count() == kParallel);
}

TEST_CASE("Concurrent duplicate requests: one ACCEPTED, the rest ALREADY_PROCESSED", "[concurrency][http][idempotency]")
{
    TempDatabase db;
    ProcessedTransactionStore store{db.path()};
    RunningServer server{store};
    const auto tx = Transaction::create(Money{700});

    std::atomic<int> accepted{0};
    std::atomic<int> duplicates{0};
    std::atomic<int> other{0};
    inParallel(16, [&](std::size_t) {
        HttpServerClient client{"127.0.0.1", server.port()};
        switch (client.send(tx).type) {
        case SendResultType::Accepted:         ++accepted; break;
        case SendResultType::AlreadyProcessed: ++duplicates; break;
        default:                               ++other; break;
        }
    });

    REQUIRE(accepted == 1);
    REQUIRE(duplicates == 15);
    REQUIRE(other == 0);
    REQUIRE(store.count() == 1);
}

TEST_CASE("Concurrent conflicting requests: one wins, the rest get 409", "[concurrency][http][idempotency]")
{
    TempDatabase db;
    ProcessedTransactionStore store{db.path()};
    RunningServer server{store};
    const auto base = Transaction::create(Money{700});

    std::atomic<int> accepted{0};
    std::atomic<int> conflicts{0};
    std::atomic<int> other{0};
    inParallel(12, [&](std::size_t i) {
        auto tx = base;
        tx.amount = Money{1000 + static_cast<std::int64_t>(i)};
        HttpServerClient client{"127.0.0.1", server.port()};
        switch (client.send(tx).type) {
        case SendResultType::Accepted:            ++accepted; break;
        case SendResultType::IdempotencyConflict: ++conflicts; break;
        default:                                  ++other; break;
        }
    });

    REQUIRE(accepted == 1);
    REQUIRE(conflicts == 11);
    REQUIRE(other == 0);
    REQUIRE(store.count() == 1);
}

TEST_CASE("Stress: many clients, each sale sent twice, none lost or doubled", "[concurrency][http][stress]")
{
    constexpr std::size_t kClients = 16;
    constexpr std::size_t kSalesPerClient = 20;

    TempDatabase db;
    ProcessedTransactionStore store{db.path()};
    ServerOptions options;
    options.ioThreads = 8;
    RunningServer server{store, 0, options};

    std::atomic<int> accepted{0};
    std::atomic<int> duplicates{0};
    std::atomic<int> failures{0};
    std::mutex salesMutex;
    std::vector<Transaction> sales;

    inParallel(kClients, [&](std::size_t) {
        HttpServerClient client{"127.0.0.1", server.port()};
        for (std::size_t i = 0; i < kSalesPerClient; ++i) {
            const auto tx = Transaction::create(Money{static_cast<std::int64_t>(100 + i)});
            for (int send = 0; send < 2; ++send) {
                switch (client.send(tx).type) {
                case SendResultType::Accepted:         ++accepted; break;
                case SendResultType::AlreadyProcessed: ++duplicates; break;
                default:                               ++failures; break;
                }
            }
            const std::scoped_lock lock{salesMutex};
            sales.push_back(tx);
        }
    });

    constexpr auto kTotal = static_cast<int>(kClients * kSalesPerClient);
    REQUIRE(failures == 0);
    REQUIRE(accepted == kTotal);
    REQUIRE(duplicates == kTotal);
    REQUIRE(store.count() == static_cast<std::size_t>(kTotal));
    for (const auto& tx : sales) {
        REQUIRE(store.amountFor(tx.id) == tx.amount.cents());
    }
}

TEST_CASE("Random failures under concurrency still lose and double nothing", "[concurrency][http][simulation]")
{
    TempDatabase db;
    ProcessedTransactionStore store{db.path()};
    RunningServer server{store};
    server.setMode(NetworkSimulationMode::RandomFailure);

    constexpr std::size_t kClients = 8;
    constexpr std::size_t kSalesPerClient = 10;
    std::atomic<int> gaveUp{0};

    inParallel(kClients, [&](std::size_t) {
        HttpServerClient client{"127.0.0.1", server.port()};
        for (std::size_t i = 0; i < kSalesPerClient; ++i) {
            const auto tx = Transaction::create(Money{500});
            int attempts = 0;
            while (!client.send(tx).successful()) {  // what the sync worker does
                if (++attempts == 200) {
                    ++gaveUp;
                    break;
                }
            }
        }
    });

    REQUIRE(gaveUp == 0);
    REQUIRE(store.count() == kClients * kSalesPerClient);
}

TEST_CASE("A single I/O thread is still a valid configuration", "[concurrency][http]")
{
    ProcessedTransactionStore store{":memory:"};
    ServerOptions options;
    options.ioThreads = 1;
    RunningServer server{store, 0, options};
    HttpServerClient client{"127.0.0.1", server.port()};

    REQUIRE(client.send(Transaction::create(Money{700})).type == SendResultType::Accepted);
}

TEST_CASE("Zero I/O threads is refused", "[concurrency][http]")
{
    ProcessedTransactionStore store{":memory:"};
    ServerOptions options;
    options.ioThreads = 0;

    REQUIRE_THROWS_AS(HttpTransactionServer(store, "127.0.0.1", 0, options), std::invalid_argument);
}

TEST_CASE("Stopping a busy server joins every I/O thread", "[concurrency][http][shutdown]")
{
    ProcessedTransactionStore store{":memory:"};
    std::atomic<bool> keepSending{true};
    std::uint16_t port = 0;
    {
        RunningServer server{store};
        port = server.port();
        std::jthread client{[&] {
            HttpServerClient http{"127.0.0.1", port, 500ms};
            while (keepSending) {
                (void)http.send(Transaction::create(Money{100}));
            }
        }};
        std::this_thread::sleep_for(50ms);
        keepSending = false;
    }  // the client thread joins, then ~RunningServer stops and joins the pool

    REQUIRE(port != 0);
}
