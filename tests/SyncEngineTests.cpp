// SyncEngine, one synchronous pass at a time, against in-memory fakes:
// no SQLite, no sockets, no threads.

#include "resilientpos/sync/SyncEngine.hpp"
#include "fakes/FakeServerClient.hpp"
#include "fakes/FakeTransactionRepository.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::SendResultType;
using resilientpos::storage::TransactionRepository;
using resilientpos::sync::SyncEngine;
using resilientpos::test::FakeServerClient;
using resilientpos::test::FakeTransactionRepository;

namespace {

// Saved with strictly increasing timestamps so "oldest first" is unambiguous.
Transaction savePending(TransactionRepository& repository, Money amount)
{
    static auto clock = std::chrono::system_clock::now();
    clock += std::chrono::seconds{1};

    auto tx = Transaction::create(amount);
    tx.status = TransactionStatus::PendingSync;
    tx.createdAt = clock;
    repository.save(tx);
    return tx;
}

} // namespace

TEST_CASE("Sync with an empty queue sends nothing", "[sync]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(server.sentTransactions.empty());
    REQUIRE(report.attempted == 0);
    REQUIRE(report.synced == 0);
    REQUIRE(report.stillPending == 0);
}

TEST_CASE("Sync loads pending transactions and sends them oldest first", "[sync]")
{
    FakeTransactionRepository repository;
    const auto a = savePending(repository, Money{1000});
    const auto b = savePending(repository, Money{2000});
    const auto c = savePending(repository, Money{3000});
    FakeServerClient server;
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(server.sentIds() == std::vector<std::string>{a.id, b.id, c.id});
    REQUIRE(server.sentTransactions[1].amount == Money{2000});
    REQUIRE(report.attempted == 3);
    REQUIRE(report.outcomes.size() == 3);
}

TEST_CASE("Sync success becomes SYNCED", "[sync]")
{
    FakeTransactionRepository repository;
    const auto a = savePending(repository, Money{1000});
    const auto b = savePending(repository, Money{2000});
    FakeServerClient server;
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.synced == 2);
    REQUIRE(report.failed == 0);
    REQUIRE(report.stillPending == 0);
    REQUIRE(repository.findById(a.id)->status == TransactionStatus::Synced);
    REQUIRE(repository.findById(b.id)->status == TransactionStatus::Synced);
}

TEST_CASE("Sync marks a transaction SYNCING while it is being sent", "[sync]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    std::vector<TransactionStatus> statusWhileSending;
    server.onSend = [&](const Transaction& sent) {
        statusWhileSending.push_back(repository.findById(sent.id)->status);
    };
    SyncEngine engine{repository, server};

    engine.syncPendingTransactions();

    REQUIRE(statusWhileSending == std::vector<TransactionStatus>{TransactionStatus::Syncing});
    REQUIRE(repository.findById(tx.id)->syncAttempts == 1);
}

TEST_CASE("Sync network failure remains PENDING_SYNC", "[sync][offline]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::NetworkError, "connection refused"};
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.failed == 1);
    REQUIRE(report.stillPending == 1);
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::PendingSync);
    REQUIRE(report.outcomes.front().result == SendResultType::NetworkError);
    REQUIRE(report.outcomes.front().message == "connection refused");
}

TEST_CASE("Sync stops the pass at the first network error", "[sync][offline]")
{
    FakeTransactionRepository repository;
    const auto a = savePending(repository, Money{1000});
    const auto b = savePending(repository, Money{2000});
    const auto c = savePending(repository, Money{3000});
    FakeServerClient server;
    server.resultFor[b.id] = SendResultType::NetworkError;
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    // Server unreachable: no point waiting for c to time out too.
    REQUIRE(server.sentIds() == std::vector<std::string>{a.id, b.id});
    REQUIRE(report.synced == 1);
    REQUIRE(report.failed == 1);
    REQUIRE(report.stillPending == 2);
    REQUIRE(repository.findById(c.id)->status == TransactionStatus::PendingSync);
    REQUIRE(repository.findById(c.id)->syncAttempts == 0);
}

TEST_CASE("Sync carries on past a server error", "[sync]")
{
    FakeTransactionRepository repository;
    const auto a = savePending(repository, Money{1000});
    const auto b = savePending(repository, Money{2000});
    const auto c = savePending(repository, Money{3000});
    FakeServerClient server;
    server.resultFor[b.id] = SendResultType::ServerError;
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(server.sendCount() == 3);
    REQUIRE(report.synced == 2);
    REQUIRE(report.failed == 1);
    REQUIRE(repository.findById(a.id)->status == TransactionStatus::Synced);
    REQUIRE(repository.findById(b.id)->status == TransactionStatus::PendingSync);
    REQUIRE(repository.findById(c.id)->status == TransactionStatus::Synced);
}

TEST_CASE("Sync AlreadyProcessed becomes SYNCED", "[sync][idempotency]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1040});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::AlreadyProcessed, "duplicate"};
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.synced == 1);
    REQUIRE(report.alreadyProcessed == 1);
    REQUIRE(report.stillPending == 0);
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::Synced);
}

TEST_CASE("Sync keeps a transaction pending if the client throws", "[sync][offline]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    server.throwOnSend = true;
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.failed == 1);
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::PendingSync);
}

TEST_CASE("Synced transactions are not sent again", "[sync]")
{
    FakeTransactionRepository repository;
    savePending(repository, Money{1000});
    FakeServerClient server;
    SyncEngine engine{repository, server};

    engine.syncPendingTransactions();
    const auto second = engine.syncPendingTransactions();

    REQUIRE(server.sendCount() == 1);
    REQUIRE(second.attempted == 0);
}

TEST_CASE("Sync leaves sales the service is still sending alone", "[sync]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    SyncEngine engine{repository, server};
    // Saved after the engine started: a live sale mid-send, not a crash leftover.
    repository.save(Transaction::create(Money{1000}));  // CREATED

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.attempted == 0);
    REQUIRE(server.sentTransactions.empty());
}

TEST_CASE("Engine startup requeues sales a crash left in flight", "[sync][durability]")
{
    FakeTransactionRepository repository;
    const auto created = Transaction::create(Money{1000});  // died during the first send
    repository.save(created);
    const auto syncing = savePending(repository, Money{2000});
    repository.updateStatus(syncing.id, TransactionStatus::Syncing);  // died during a resend

    FakeServerClient server;
    server.resultToReturn = {SendResultType::AlreadyProcessed, "duplicate"};
    SyncEngine engine{repository, server};  // recovery happens here

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.synced == 2);
    REQUIRE(repository.findById(created.id)->status == TransactionStatus::Synced);
    REQUIRE(repository.findById(syncing.id)->status == TransactionStatus::Synced);
}
