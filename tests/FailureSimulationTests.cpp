// Offline recovery and the server's failure modes, end to end over real
// sockets: TransactionService / SyncEngine -> HttpServerClient -> HttpTransactionServer.

#include "resilientpos/network/HttpServerClient.hpp"
#include "resilientpos/server/NetworkSimulationMode.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/sync/SyncEngine.hpp"
#include "RunningServer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;
using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::HttpServerClient;
using resilientpos::network::SendResultType;
using resilientpos::server::NetworkSimulationMode;
using resilientpos::server::networkSimulationModeFromString;
using resilientpos::server::ProcessedTransactionStore;
using resilientpos::service::TransactionService;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::sync::SyncEngine;
using resilientpos::test::closedPort;
using resilientpos::test::RunningServer;

TEST_CASE("Simulation modes round-trip through their CLI names", "[simulation]")
{
    for (const auto mode : {NetworkSimulationMode::Normal, NetworkSimulationMode::Latency,
                            NetworkSimulationMode::Reject, NetworkSimulationMode::Timeout,
                            NetworkSimulationMode::DropResponseAfterProcessing}) {
        REQUIRE(networkSimulationModeFromString(toString(mode)) == mode);
    }
    REQUIRE(toString(NetworkSimulationMode::DropResponseAfterProcessing) == "drop");
    REQUIRE_FALSE(networkSimulationModeFromString("offline").has_value());
}

TEST_CASE("Sales made while the server is off sync once it is back", "[sync][http][offline]")
{
    SQLiteTransactionRepository posRepository{":memory:"};

    // Server off: three sales queue up locally.
    {
        HttpServerClient offline{"127.0.0.1", closedPort(), 500ms};
        TransactionService service{posRepository, offline};
        service.createTransaction(Money{1000});
        service.createTransaction(Money{2000});
        service.createTransaction(Money{3000});
    }
    REQUIRE(posRepository.findPending().size() == 3);

    // Server started: one sync pass drains the queue.
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    HttpServerClient online{"127.0.0.1", server.port()};
    SyncEngine engine{posRepository, online};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.synced == 3);
    REQUIRE(report.alreadyProcessed == 0);
    REQUIRE(report.stillPending == 0);
    REQUIRE(posRepository.findPending().empty());
    REQUIRE(serverStore.count() == 3);
}

TEST_CASE("Dropped response: server commits, POS keeps the sale PENDING_SYNC", "[simulation][http]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    server.setMode(NetworkSimulationMode::DropResponseAfterProcessing);

    SQLiteTransactionRepository posRepository{":memory:"};
    HttpServerClient client{"127.0.0.1", server.port()};
    TransactionService service{posRepository, client};

    const auto tx = service.createTransaction(Money{1040});

    // The ambiguous failure: both sides disagree about the sale.
    REQUIRE(service.lastSendResult().type == SendResultType::NetworkError);
    REQUIRE(tx.status == TransactionStatus::PendingSync);
    REQUIRE(posRepository.findById(tx.id)->status == TransactionStatus::PendingSync);
    REQUIRE(serverStore.amountFor(tx.id) == 1040);
}

TEST_CASE("Resending after a dropped response is ALREADY_PROCESSED and ends SYNCED", "[simulation][sync][idempotency]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    SQLiteTransactionRepository posRepository{":memory:"};
    HttpServerClient client{"127.0.0.1", server.port()};
    TransactionService service{posRepository, client};
    SyncEngine engine{posRepository, client};

    server.setMode(NetworkSimulationMode::DropResponseAfterProcessing);
    const auto tx = service.createTransaction(Money{1040});
    REQUIRE(tx.status == TransactionStatus::PendingSync);

    // Even if the connection stays flaky the resend is harmless...
    engine.syncPendingTransactions();
    REQUIRE(serverStore.count() == 1);

    // ...and once replies get through, the server's answer settles it.
    server.setMode(NetworkSimulationMode::Normal);
    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.synced == 1);
    REQUIRE(report.alreadyProcessed == 1);
    REQUIRE(posRepository.findById(tx.id)->status == TransactionStatus::Synced);
    REQUIRE(serverStore.count() == 1);  // stored exactly once
    REQUIRE(serverStore.amountFor(tx.id) == 1040);
}

TEST_CASE("Reject mode answers 503 and stores nothing", "[simulation][http]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    server.setMode(NetworkSimulationMode::Reject);
    HttpServerClient client{"127.0.0.1", server.port()};

    const auto result = client.send(Transaction::create(Money{700}));

    REQUIRE(result.type == SendResultType::ServerError);
    REQUIRE(serverStore.count() == 0);
}

TEST_CASE("Timeout mode never answers and stores nothing", "[simulation][http]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    server.setMode(NetworkSimulationMode::Timeout);
    HttpServerClient client{"127.0.0.1", server.port(), 300ms};

    const auto result = client.send(Transaction::create(Money{700}));

    REQUIRE(result.type == SendResultType::NetworkError);
    REQUIRE(serverStore.count() == 0);
}

TEST_CASE("Latency mode answers late but still accepts", "[simulation][http]")
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
