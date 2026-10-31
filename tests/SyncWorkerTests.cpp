// The SyncEngine's background worker (std::jthread + condition variable),
// and the locking that lets it share the repository with the UI thread.

#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/sync/SyncEngine.hpp"
#include "Eventually.hpp"
#include "fakes/FakeServerClient.hpp"
#include "fakes/FakeTransactionRepository.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::SendResult;
using resilientpos::network::SendResultType;
using resilientpos::network::ServerClient;
using resilientpos::service::TransactionService;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::storage::TransactionRepository;
using resilientpos::sync::RetryPolicy;
using resilientpos::sync::SyncEngine;
using resilientpos::test::eventually;
using resilientpos::test::FakeServerClient;
using resilientpos::test::FakeTransactionRepository;

namespace {

Transaction savePending(TransactionRepository& repository, Money amount)
{
    auto tx = Transaction::create(amount);
    tx.status = TransactionStatus::PendingSync;
    repository.save(tx);
    return tx;
}

bool isSynced(TransactionRepository& repository, const std::string& id)
{
    return repository.findById(id)->status == TransactionStatus::Synced;
}

// Short delays so backoff tests run in milliseconds, not seconds.
const RetryPolicy kFastRetry{20ms, 80ms};

} // namespace

TEST_CASE("Worker drains the queue as soon as it starts", "[worker]")
{
    FakeTransactionRepository repository;
    const auto a = savePending(repository, Money{100});
    const auto b = savePending(repository, Money{200});
    FakeServerClient server;
    SyncEngine engine{repository, server, kFastRetry};

    engine.start();

    REQUIRE(eventually([&] { return isSynced(repository, a.id) && isSynced(repository, b.id); }));
    REQUIRE(engine.workerStatus().running);
}

TEST_CASE("Worker wakes when notified of a new pending transaction", "[worker]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    SyncEngine engine{repository, server, kFastRetry};
    engine.start();
    REQUIRE(eventually([&] { return repository.pendingQueries >= 2; }));  // first pass done

    const auto tx = savePending(repository, Money{100});
    engine.notifyPendingTransaction();

    REQUIRE(eventually([&] { return isSynced(repository, tx.id); }));
}

TEST_CASE("Idle worker sleeps instead of polling the database", "[worker]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    SyncEngine engine{repository, server, kFastRetry};
    engine.start();
    REQUIRE(eventually([&] { return repository.pendingQueries >= 2; }));

    const int afterFirstPass = repository.pendingQueries;
    std::this_thread::sleep_for(300ms);

    REQUIRE(repository.pendingQueries == afterFirstPass);
}

TEST_CASE("Worker retries a failing server with exponential backoff", "[worker][retry]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{100});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::NetworkError, "down"};

    std::mutex timesMutex;
    std::vector<std::chrono::steady_clock::time_point> sendTimes;
    server.onSend = [&](const Transaction&) {
        const std::scoped_lock lock{timesMutex};
        sendTimes.push_back(std::chrono::steady_clock::now());
    };

    const RetryPolicy policy{30ms, 120ms};
    SyncEngine engine{repository, server, policy};
    engine.start();
    REQUIRE(eventually([&] { return server.sendCount() >= 4; }));

    const auto status = engine.workerStatus();
    REQUIRE(status.consecutiveFailures >= 3);
    REQUIRE(repository.findById(tx.id)->status != TransactionStatus::Synced);

    // Each gap is at least the backoff delay for that attempt: 30, 60, 120 ms.
    // (Lower bounds only, so a slow CI machine cannot make this flaky.)
    const std::scoped_lock lock{timesMutex};
    for (unsigned int i = 1; i < 4; ++i) {
        INFO("gap before send " << i);
        REQUIRE(sendTimes[i] - sendTimes[i - 1] >= policy.nextDelay(i - 1));
    }
}

TEST_CASE("Worker recovers once the server comes back", "[worker][offline]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{100});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::NetworkError, "down"};
    SyncEngine engine{repository, server, kFastRetry};
    engine.start();
    REQUIRE(eventually([&] { return engine.workerStatus().consecutiveFailures >= 2; }));

    server.setResult({SendResultType::Accepted, "back"});

    REQUIRE(eventually([&] { return isSynced(repository, tx.id); }));
    REQUIRE(eventually([&] { return engine.workerStatus().consecutiveFailures == 0; }));
}

TEST_CASE("New sales during a backoff do not trigger a retry storm", "[worker][retry]")
{
    FakeTransactionRepository repository;
    savePending(repository, Money{100});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::NetworkError, "down"};
    SyncEngine engine{repository, server, RetryPolicy{10s, 10s}};
    engine.start();
    REQUIRE(eventually([&] { return engine.workerStatus().nextRetryAt.has_value(); }));

    for (int i = 0; i < 20; ++i) {
        savePending(repository, Money{100});
        engine.notifyPendingTransaction();
    }
    std::this_thread::sleep_for(200ms);

    REQUIRE(server.sendCount() == 1);  // still waiting out the backoff
}

TEST_CASE("Server reachable ends a backoff early", "[worker][retry]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{100});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::NetworkError, "down"};
    SyncEngine engine{repository, server, RetryPolicy{60s, 60s}};
    engine.start();
    REQUIRE(eventually([&] { return engine.workerStatus().nextRetryAt.has_value(); }));

    server.setResult({SendResultType::Accepted, "back"});
    engine.notifyServerReachable();  // e.g. a live sale just went through

    REQUIRE(eventually([&] { return isSynced(repository, tx.id); }, 5s));
}

TEST_CASE("Stop wakes a backing-off worker and joins it promptly", "[worker][shutdown]")
{
    FakeTransactionRepository repository;
    savePending(repository, Money{100});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::NetworkError, "down"};
    SyncEngine engine{repository, server, RetryPolicy{60s, 60s}};
    engine.start();
    REQUIRE(eventually([&] { return engine.workerStatus().nextRetryAt.has_value(); }));

    const auto started = std::chrono::steady_clock::now();
    engine.stop();
    const auto took = std::chrono::steady_clock::now() - started;

    REQUIRE(took < 5s);  // not the 60 s backoff
    REQUIRE_FALSE(engine.workerStatus().running);
    engine.stop();  // idempotent
}

TEST_CASE("Destroying the engine stops the worker before the repository goes away", "[worker][shutdown]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    {
        SyncEngine engine{repository, server, kFastRetry};
        engine.start();
        REQUIRE(eventually([&] { return repository.pendingQueries >= 2; }));
    } // ~SyncEngine -> request_stop -> join

    const int afterShutdown = repository.pendingQueries;
    savePending(repository, Money{100});
    std::this_thread::sleep_for(100ms);

    REQUIRE(repository.pendingQueries == afterShutdown);
    REQUIRE(server.sendCount() == 0);
}

TEST_CASE("Concurrent claims hand each pending row to exactly one thread", "[worker][concurrency]")
{
    SQLiteTransactionRepository repository{":memory:"};
    std::vector<std::string> ids;
    for (int i = 0; i < 100; ++i) {
        ids.push_back(savePending(repository, Money{100}).id);
    }

    std::atomic<int> claims{0};
    std::vector<std::jthread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (const auto& id : ids) {
                if (repository.tryUpdateStatus(id, TransactionStatus::PendingSync,
                                               TransactionStatus::Syncing)) {
                    ++claims;
                }
            }
        });
    }
    threads.clear();  // join

    REQUIRE(claims == 100);
    for (const auto& id : ids) {
        REQUIRE(repository.findById(id)->syncAttempts == 1);
    }
}

namespace {

// Behaves like the real server (idempotent by ID) but drops every other
// first attempt, and checks nobody sends the same sale twice at once.
class FlakyIdempotentServer final : public ServerClient {
public:
    SendResult send(const Transaction& tx) override
    {
        {
            const std::scoped_lock lock{mutex_};
            if (!inFlight_.insert(tx.id).second) {
                concurrentDuplicateSend = true;
            }
        }
        std::this_thread::sleep_for(1ms);  // widen the race window

        const std::scoped_lock lock{mutex_};
        inFlight_.erase(tx.id);
        if (stored.contains(tx.id)) {
            return {SendResultType::AlreadyProcessed, "duplicate"};
        }
        if (++calls_ % 2 == 0) {
            return {SendResultType::NetworkError, "dropped"};
        }
        stored.insert(tx.id);
        return {SendResultType::Accepted, "stored"};
    }

    std::set<std::string> storedIds()
    {
        const std::scoped_lock lock{mutex_};
        return stored;
    }

    std::atomic<bool> concurrentDuplicateSend{false};

private:
    std::mutex mutex_;
    std::set<std::string> inFlight_;
    std::set<std::string> stored;
    int calls_{0};
};

} // namespace

TEST_CASE("Selling while the worker syncs loses nothing and duplicates nothing", "[worker][concurrency]")
{
    SQLiteTransactionRepository repository{":memory:"};
    FlakyIdempotentServer server;
    SyncEngine engine{repository, server, RetryPolicy{5ms, 20ms}};
    TransactionService service{repository, server, {
        .persisted = {},
        .completed = [&](const Transaction&, const SendResult& result) {
            if (!result.successful()) {
                engine.notifyPendingTransaction();
            }
        },
    }};
    engine.start();

    std::set<std::string> sold;
    for (int i = 0; i < 40; ++i) {
        sold.insert(service.createTransaction(Money{100 + i}).id);
    }

    REQUIRE(eventually([&] { return repository.findPending().empty(); }));
    engine.stop();

    REQUIRE_FALSE(server.concurrentDuplicateSend);
    REQUIRE(server.storedIds() == sold);  // every sale exactly once
    for (const auto& id : sold) {
        REQUIRE(isSynced(repository, id));
    }
}
