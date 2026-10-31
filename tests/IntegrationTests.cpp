// End to end over real sockets and real SQLite files:
//
//   TransactionService / SyncEngine -> HttpServerClient -> HttpTransactionServer
//          |                                                       |
//       POS SQLite                                           server SQLite
//
// These are the tests that prove the architecture, not just the classes.

#include "resilientpos/network/HttpServerClient.hpp"
#include "resilientpos/server/NetworkSimulationMode.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/sync/SyncEngine.hpp"
#include "Eventually.hpp"
#include "RunningServer.hpp"
#include "TempDatabase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::HttpServerClient;
using resilientpos::network::SendResultType;
using resilientpos::server::NetworkSimulationMode;
using resilientpos::server::ProcessedTransactionStore;
using resilientpos::service::TransactionService;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::sync::RetryPolicy;
using resilientpos::sync::SyncEngine;
using resilientpos::test::closedPort;
using resilientpos::test::eventually;
using resilientpos::test::RunningServer;
using resilientpos::test::TempDatabase;

// --- 1. Online -----------------------------------------------------------------

TEST_CASE("IntegrationTest: online sale is SYNCED on the POS and stored on the server", "[integration][http]")
{
    TempDatabase serverDb;
    ProcessedTransactionStore serverStore{serverDb.path()};
    RunningServer server{serverStore};

    TempDatabase posDb;
    SQLiteTransactionRepository posRepository{posDb.path()};
    HttpServerClient client{"127.0.0.1", server.port()};
    TransactionService service{posRepository, client};

    const auto a = service.createTransaction(Money{350} * 2);

    REQUIRE(service.lastSendResult().type == SendResultType::Accepted);
    REQUIRE(posRepository.findById(a.id)->status == TransactionStatus::Synced);
    REQUIRE(serverStore.amountFor(a.id) == 700);
}

// --- 2. Offline recovery ---------------------------------------------------------

TEST_CASE("IntegrationTest: sales made while the server is down sync once it starts", "[integration][http][offline]")
{
    const auto port = closedPort();  // server unavailable
    TempDatabase posDb;
    SQLiteTransactionRepository posRepository{posDb.path()};
    HttpServerClient client{"127.0.0.1", port, 1s};
    TransactionService service{posRepository, client};

    const auto a = service.createTransaction(Money{1000});
    const auto b = service.createTransaction(Money{2000});
    const auto c = service.createTransaction(Money{3000});
    REQUIRE(posRepository.findPending().size() == 3);

    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore, port};  // start server
    SyncEngine engine{posRepository, client};

    const auto report = engine.syncPendingTransactions();  // sync

    REQUIRE(report.synced == 3);
    REQUIRE(report.alreadyProcessed == 0);
    REQUIRE(posRepository.findPending().empty());
    REQUIRE(serverStore.amountFor(a.id) == 1000);
    REQUIRE(serverStore.amountFor(b.id) == 2000);
    REQUIRE(serverStore.amountFor(c.id) == 3000);
}

TEST_CASE("IntegrationTest: background worker syncs offline sales when the server returns", "[integration][http][offline][worker]")
{
    const auto port = closedPort();
    SQLiteTransactionRepository posRepository{":memory:"};
    HttpServerClient client{"127.0.0.1", port, 1s};
    SyncEngine engine{posRepository, client, RetryPolicy{50ms, 200ms}};
    TransactionService service{posRepository, client, {
        .persisted = {},
        .completed = [&](const Transaction&, const auto& result) {
            if (!result.successful()) {
                engine.notifyPendingTransaction();
            }
        },
    }};
    engine.start();

    std::vector<std::string> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(service.createTransaction(Money{100}).id);
    }
    REQUIRE(posRepository.findPending().size() == 3);

    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore, port};

    // No manual sync: the worker's next retry finds the server back.
    REQUIRE(eventually([&] { return posRepository.findPending().empty(); }));
    REQUIRE(serverStore.count() == 3);
}

// --- 3. Crash recovery ---------------------------------------------------------

TEST_CASE("IntegrationTest: pending sales survive destroying the POS", "[integration][durability]")
{
    TempDatabase posDb;
    std::vector<std::string> ids;
    const auto port = closedPort();
    {
        SQLiteTransactionRepository posRepository{posDb.path()};
        HttpServerClient client{"127.0.0.1", port, 1s};
        TransactionService service{posRepository, client};
        for (const auto cents : {1000, 2000, 3000}) {
            ids.push_back(service.createTransaction(Money{cents}).id);
        }
    } // POS application destroyed: repository closed, process "gone"

    SQLiteTransactionRepository restarted{posDb.path()};  // new POS instance
    const auto pending = restarted.findPending();

    REQUIRE(pending.size() == 3);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        REQUIRE(pending[i].id == ids[i]);
        REQUIRE(pending[i].status == TransactionStatus::PendingSync);
    }
}

TEST_CASE("IntegrationTest: a sale cut off mid-send is resent after restart", "[integration][durability]")
{
    TempDatabase posDb;
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    std::string id;
    {
        SQLiteTransactionRepository posRepository{posDb.path()};
        // Power cut after the server committed but before the POS heard back:
        // the row is still CREATED on disk.
        const auto tx = Transaction::create(Money{550});
        posRepository.save(tx);
        REQUIRE(serverStore.record(tx.id, 550) == resilientpos::server::RecordOutcome::Inserted);
        id = tx.id;
    }

    SQLiteTransactionRepository restarted{posDb.path()};
    HttpServerClient client{"127.0.0.1", server.port()};
    SyncEngine engine{restarted, client};  // startup recovery: CREATED -> PENDING_SYNC

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.alreadyProcessed == 1);
    REQUIRE(restarted.findById(id)->status == TransactionStatus::Synced);
    REQUIRE(serverStore.count() == 1);
}

// --- 4. Ambiguous network failure ------------------------------------------------

TEST_CASE("IntegrationTest: LostResponseDoesNotCauseDuplicateTransaction", "[integration][http][idempotency]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    SQLiteTransactionRepository posRepository{":memory:"};
    HttpServerClient client{"127.0.0.1", server.port()};
    TransactionService service{posRepository, client};
    SyncEngine engine{posRepository, client};

    // Server receives A, stores A, connection drops before the response.
    server.setMode(NetworkSimulationMode::DropAfterProcessing);
    const auto a = service.createTransaction(Money{1040});

    // POS thinks: A = pending. Server already has it.
    REQUIRE(service.lastSendResult().type == SendResultType::NetworkError);
    REQUIRE(posRepository.findById(a.id)->status == TransactionStatus::PendingSync);
    REQUIRE(serverStore.amountFor(a.id) == 1040);

    // Retry A: the server sees a duplicate and answers ALREADY_PROCESSED.
    server.setMode(NetworkSimulationMode::Normal);
    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.alreadyProcessed == 1);
    REQUIRE(posRepository.findById(a.id)->status == TransactionStatus::Synced);
    REQUIRE(serverStore.count() == 1);  // only ONE copy of A exists
}

TEST_CASE("IntegrationTest: drop before processing needs the retry to deliver", "[integration][http][simulation]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    SQLiteTransactionRepository posRepository{":memory:"};
    HttpServerClient client{"127.0.0.1", server.port()};
    TransactionService service{posRepository, client};
    SyncEngine engine{posRepository, client};

    server.setMode(NetworkSimulationMode::DropBeforeProcessing);
    const auto a = service.createTransaction(Money{1040});

    // Same symptom on the POS as a drop after processing...
    REQUIRE(service.lastSendResult().type == SendResultType::NetworkError);
    REQUIRE(posRepository.findById(a.id)->status == TransactionStatus::PendingSync);
    // ...but this time the server does not have it.
    REQUIRE_FALSE(serverStore.amountFor(a.id).has_value());

    server.setMode(NetworkSimulationMode::Normal);
    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.synced == 1);
    REQUIRE(report.alreadyProcessed == 0);  // a genuine first delivery
    REQUIRE(serverStore.amountFor(a.id) == 1040);
}

TEST_CASE("IntegrationTest: random failures never lose or duplicate a sale", "[integration][http][simulation][idempotency]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    server.setMode(NetworkSimulationMode::RandomFailure);
    SQLiteTransactionRepository posRepository{":memory:"};
    HttpServerClient client{"127.0.0.1", server.port()};
    TransactionService service{posRepository, client};
    SyncEngine engine{posRepository, client};

    constexpr int kSales = 30;
    for (int i = 0; i < kSales; ++i) {
        service.createTransaction(Money{100 + i});
    }
    for (int pass = 0; pass < 200 && !posRepository.findPending().empty(); ++pass) {
        engine.syncPendingTransactions();
    }

    REQUIRE(posRepository.findPending().empty());
    REQUIRE(serverStore.count() == kSales);
    for (const auto& tx : posRepository.findAll()) {
        REQUIRE(tx.status == TransactionStatus::Synced);
        REQUIRE(serverStore.amountFor(tx.id) == tx.amount.cents());
    }
}

// --- Transport behaviour ---------------------------------------------------------

TEST_CASE("Client gets ACCEPTED, then ALREADY_PROCESSED for the same transaction", "[http][idempotency]")
{
    ProcessedTransactionStore store{":memory:"};
    RunningServer server{store};
    HttpServerClient client{"127.0.0.1", server.port()};
    const auto tx = Transaction::create(Money{700});

    const auto first = client.send(tx);
    INFO(first.message);
    REQUIRE(first.type == SendResultType::Accepted);

    const auto second = client.send(tx);
    INFO(second.message);
    REQUIRE(second.type == SendResultType::AlreadyProcessed);
    REQUIRE(store.count() == 1);
}

TEST_CASE("Client reports a stopped server as NetworkError", "[http][offline]")
{
    HttpServerClient client{"127.0.0.1", closedPort(), 2s};

    const auto result = client.send(Transaction::create(Money{700}));

    REQUIRE(result.type == SendResultType::NetworkError);
}

TEST_CASE("Reject mode answers 503 and stores nothing", "[http][simulation]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    server.setMode(NetworkSimulationMode::Reject);
    HttpServerClient client{"127.0.0.1", server.port()};

    const auto result = client.send(Transaction::create(Money{700}));

    REQUIRE(result.type == SendResultType::ServerError);
    REQUIRE(serverStore.count() == 0);
}

TEST_CASE("Timeout mode never answers and stores nothing", "[http][simulation]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    server.setMode(NetworkSimulationMode::Timeout);
    HttpServerClient client{"127.0.0.1", server.port(), 300ms};

    const auto result = client.send(Transaction::create(Money{700}));

    REQUIRE(result.type == SendResultType::NetworkError);
    REQUIRE(serverStore.count() == 0);
}

TEST_CASE("Latency mode answers late but still accepts", "[http][simulation]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    server.setMode(NetworkSimulationMode::Latency);
    HttpServerClient client{"127.0.0.1", server.port()};

    const auto started = std::chrono::steady_clock::now();
    const auto result = client.send(Transaction::create(Money{700}));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(result.type == SendResultType::Accepted);
    REQUIRE(elapsed >= resilientpos::server::kSimulatedLatency);
    REQUIRE(serverStore.count() == 1);
}
