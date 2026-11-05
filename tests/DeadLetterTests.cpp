// Giving up on a sale, and getting it back: failure classification, the retry
// limit, the DEAD_LETTER state and manual retry.

#include "resilientpos/network/HttpServerClient.hpp"
#include "resilientpos/server/NetworkSimulationMode.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/server/TransactionHandler.hpp"
#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/storage/StorageError.hpp"
#include "resilientpos/sync/SyncEngine.hpp"
#include "Eventually.hpp"
#include "RunningServer.hpp"
#include "TempDatabase.hpp"
#include "fakes/FakeServerClient.hpp"
#include "fakes/FakeTransactionRepository.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <sqlite3.h>

#include <chrono>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;
using resilientpos::core::Money;
using resilientpos::core::SyncFailure;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::classify;
using resilientpos::network::FailureClass;
using resilientpos::network::HttpServerClient;
using resilientpos::network::SendResult;
using resilientpos::network::SendResultType;
using resilientpos::network::toSyncFailure;
using resilientpos::server::NetworkSimulationMode;
using resilientpos::server::ProcessedTransactionStore;
using resilientpos::service::TransactionService;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::storage::StorageError;
using resilientpos::storage::TransactionRepository;
using resilientpos::sync::noJitter;
using resilientpos::sync::RetryPolicy;
using resilientpos::sync::SyncEngine;
using resilientpos::sync::SyncOptions;
using resilientpos::test::eventually;
using resilientpos::test::FakeServerClient;
using resilientpos::test::FakeTransactionRepository;
using resilientpos::test::RunningServer;
using resilientpos::test::TempDatabase;

namespace {

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

TransactionStatus statusOf(TransactionRepository& repository, const std::string& id)
{
    return repository.findById(id)->status;
}

const RetryPolicy kFastRetry{5ms, 20ms, noJitter()};

} // namespace

// --- Classification ------------------------------------------------------------

TEST_CASE("Failures are classified by what they say about the sale", "[dlq][classify]")
{
    REQUIRE(classify(SendResultType::Accepted) == FailureClass::None);
    REQUIRE(classify(SendResultType::AlreadyProcessed) == FailureClass::None);
    REQUIRE(classify(SendResultType::NetworkError) == FailureClass::Unreachable);
    REQUIRE(classify(SendResultType::ServerUnavailable) == FailureClass::Unreachable);
    REQUIRE(classify(SendResultType::ServerError) == FailureClass::Transient);
    REQUIRE(classify(SendResultType::Rejected) == FailureClass::Permanent);
    REQUIRE(classify(SendResultType::IdempotencyConflict) == FailureClass::Permanent);
}

TEST_CASE("Stored failure reasons are bounded", "[dlq]")
{
    const auto failure = toSyncFailure({SendResultType::ServerError, std::string(100'000, 'x')});

    REQUIRE(failure.code == "SERVER_ERROR");
    REQUIRE(failure.reason.size() == 500);
}

// --- Permanent failures --------------------------------------------------------

TEST_CASE("A permanent failure is dead-lettered on the first attempt", "[dlq]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::Rejected, "HTTP 422: invalid"};
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.deadLettered == 1);
    REQUIRE(report.failed == 0);
    REQUIRE(report.stillPending == 0);
    REQUIRE(report.outcomes.front().deadLettered);

    const auto stored = repository.findById(tx.id);
    REQUIRE(stored->status == TransactionStatus::DeadLetter);
    REQUIRE(stored->lastFailure->code == "REJECTED");
    REQUIRE(stored->lastFailure->reason == "HTTP 422: invalid");
}

TEST_CASE("An idempotency conflict is dead-lettered, not retried", "[dlq][idempotency]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::IdempotencyConflict, "HTTP 409"};
    SyncEngine engine{repository, server};

    engine.syncPendingTransactions();
    engine.syncPendingTransactions();

    REQUIRE(server.sendCount() == 1);
    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::DeadLetter);
}

TEST_CASE("A poison sale does not hold up the queue behind it", "[dlq]")
{
    FakeTransactionRepository repository;
    const auto a = savePending(repository, Money{100});
    const auto poison = savePending(repository, Money{200});
    const auto c = savePending(repository, Money{300});
    FakeServerClient server;
    server.resultFor[poison.id] = SendResultType::Rejected;
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.synced == 2);
    REQUIRE(report.deadLettered == 1);
    REQUIRE(statusOf(repository, a.id) == TransactionStatus::Synced);
    REQUIRE(statusOf(repository, poison.id) == TransactionStatus::DeadLetter);
    REQUIRE(statusOf(repository, c.id) == TransactionStatus::Synced);
}

// --- The retry limit -----------------------------------------------------------

TEST_CASE("A server error is retried up to the limit, then dead-lettered", "[dlq]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::ServerError, "HTTP 500"};
    SyncEngine engine{repository, server, RetryPolicy{}, SyncOptions{.maxFailedAttempts = 3}};

    REQUIRE(engine.syncPendingTransactions().failed == 1);
    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::PendingSync);
    REQUIRE(engine.syncPendingTransactions().failed == 1);
    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::PendingSync);

    const auto third = engine.syncPendingTransactions();
    REQUIRE(third.failed == 0);
    REQUIRE(third.deadLettered == 1);
    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::DeadLetter);
    REQUIRE(repository.findById(tx.id)->failedAttempts == 3);

    REQUIRE(engine.syncPendingTransactions().attempted == 0);  // not sent a fourth time
    REQUIRE(server.sendCount() == 3);
}

TEST_CASE("A long outage never dead-letters a sale", "[dlq][offline]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    SyncEngine engine{repository, server, RetryPolicy{}, SyncOptions{.maxFailedAttempts = 3}};

    // A till can be cut off for days. However many passes that takes, being
    // offline says nothing about the sale.
    const auto type = GENERATE(SendResultType::NetworkError, SendResultType::ServerUnavailable);
    server.resultToReturn = {type, "down"};
    for (int pass = 0; pass < 100; ++pass) {
        REQUIRE(engine.syncPendingTransactions().deadLettered == 0);
    }

    const auto stored = repository.findById(tx.id);
    REQUIRE(stored->status == TransactionStatus::PendingSync);
    REQUIRE(stored->syncAttempts == 100);
    REQUIRE(stored->failedAttempts == 0);
    REQUIRE(stored->lastFailure->code == std::string{toString(type)});
}

TEST_CASE("Server unavailable ends the pass like a network error", "[dlq][offline]")
{
    FakeTransactionRepository repository;
    savePending(repository, Money{100});
    const auto b = savePending(repository, Money{200});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::ServerUnavailable, "HTTP 503"};
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(server.sendCount() == 1);
    REQUIRE(report.stillPending == 2);
    REQUIRE(repository.findById(b.id)->syncAttempts == 0);
}

TEST_CASE("Engine refuses a retry limit of zero", "[dlq]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;

    REQUIRE_THROWS_AS(
        SyncEngine(repository, server, RetryPolicy{}, SyncOptions{.maxFailedAttempts = 0}),
        std::invalid_argument);
}

// --- The worker ----------------------------------------------------------------

TEST_CASE("A dead letter does not put the worker into backoff", "[dlq][worker]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::Rejected, "HTTP 400"};
    SyncEngine engine{repository, server, RetryPolicy{60s, 60s, noJitter()}};

    engine.start();

    REQUIRE(eventually([&] { return statusOf(repository, tx.id) == TransactionStatus::DeadLetter; }));
    // The server is healthy: the next sale must go straight out, not wait 60 s.
    server.setResult({SendResultType::Accepted, "ok"});
    const auto next = savePending(repository, Money{500});
    engine.notifyPendingTransaction();
    REQUIRE(eventually([&] { return statusOf(repository, next.id) == TransactionStatus::Synced; }, 5s));
    REQUIRE_FALSE(engine.workerStatus().nextRetryAt.has_value());
}

// --- Manual inspection and retry -----------------------------------------------

TEST_CASE("Dead letters can be listed with their failure details", "[dlq]")
{
    FakeTransactionRepository repository;
    const auto a = savePending(repository, Money{100});
    const auto ok = savePending(repository, Money{200});
    const auto c = savePending(repository, Money{300});
    FakeServerClient server;
    server.resultFor[a.id] = SendResultType::Rejected;
    server.resultFor[c.id] = SendResultType::IdempotencyConflict;
    SyncEngine engine{repository, server};
    engine.syncPendingTransactions();

    const auto deadLetters = engine.deadLetters();

    REQUIRE(deadLetters.size() == 2);
    REQUIRE(deadLetters[0].id == a.id);  // oldest first
    REQUIRE(deadLetters[0].lastFailure->code == "REJECTED");
    REQUIRE(deadLetters[1].id == c.id);
    REQUIRE(deadLetters[1].lastFailure->code == "IDEMPOTENCY_CONFLICT");
    REQUIRE(statusOf(repository, ok.id) == TransactionStatus::Synced);
}

TEST_CASE("Manual retry requeues a dead letter and it can then sync", "[dlq]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::Rejected, "bad"};
    SyncEngine engine{repository, server};
    engine.syncPendingTransactions();
    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::DeadLetter);

    server.setResult({SendResultType::Accepted, "fixed"});
    REQUIRE(engine.retryDeadLetter(tx.id));
    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::PendingSync);

    REQUIRE(engine.syncPendingTransactions().synced == 1);
    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::Synced);
    REQUIRE(engine.deadLetters().empty());
}

TEST_CASE("Manual retry gives a sale a fresh retry budget", "[dlq]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::ServerError, "HTTP 500"};
    SyncEngine engine{repository, server, RetryPolicy{}, SyncOptions{.maxFailedAttempts = 2}};
    engine.syncPendingTransactions();
    engine.syncPendingTransactions();
    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::DeadLetter);

    REQUIRE(engine.retryDeadLetter(tx.id));

    REQUIRE(repository.findById(tx.id)->failedAttempts == 0);
    REQUIRE(repository.findById(tx.id)->lastFailure.has_value());  // history is kept
    engine.syncPendingTransactions();
    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::PendingSync);  // 1 of 2 again
}

TEST_CASE("Manual retry wakes the background worker", "[dlq][worker]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{1000});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::Rejected, "bad"};
    SyncEngine engine{repository, server, kFastRetry};
    engine.start();
    REQUIRE(eventually([&] { return statusOf(repository, tx.id) == TransactionStatus::DeadLetter; }));

    server.setResult({SendResultType::Accepted, "fixed"});
    REQUIRE(engine.retryDeadLetter(tx.id));

    REQUIRE(eventually([&] { return statusOf(repository, tx.id) == TransactionStatus::Synced; }));
}

TEST_CASE("Retry all requeues every dead letter", "[dlq]")
{
    FakeTransactionRepository repository;
    savePending(repository, Money{100});
    savePending(repository, Money{200});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::Rejected, "bad"};
    SyncEngine engine{repository, server};
    engine.syncPendingTransactions();

    REQUIRE(engine.retryAllDeadLetters() == 2);
    REQUIRE(engine.deadLetters().empty());
    REQUIRE(repository.findPending().size() == 2);
    REQUIRE(engine.retryAllDeadLetters() == 0);
}

TEST_CASE("Only a dead letter can be manually retried", "[dlq]")
{
    FakeTransactionRepository repository;
    const auto tx = savePending(repository, Money{100});
    FakeServerClient server;
    SyncEngine engine{repository, server};

    REQUIRE_FALSE(engine.retryDeadLetter(tx.id));
    REQUIRE_THROWS_AS(engine.retryDeadLetter("no-such-id"), StorageError);
}

// --- First send (TransactionService) -------------------------------------------

TEST_CASE("Service dead-letters a sale the server refuses outright", "[dlq][service]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    server.resultToReturn = {SendResultType::Rejected, "HTTP 422: invalid"};
    TransactionService service{repository, server};

    const auto tx = service.createTransaction(Money{1000});

    REQUIRE(tx.status == TransactionStatus::DeadLetter);
    REQUIRE(tx.lastFailure->code == "REJECTED");
    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::DeadLetter);
    REQUIRE(repository.findPending().empty());
}

TEST_CASE("Service counts a server error against the retry limit", "[dlq][service]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    server.resultToReturn = {SendResultType::ServerError, "HTTP 500"};
    TransactionService service{repository, server};

    const auto tx = service.createTransaction(Money{1000});

    REQUIRE(tx.status == TransactionStatus::PendingSync);
    REQUIRE(repository.findById(tx.id)->failedAttempts == 1);
    REQUIRE(repository.findById(tx.id)->lastFailure->code == "SERVER_ERROR");
}

TEST_CASE("Service does not count being offline against the retry limit", "[dlq][service][offline]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    server.resultToReturn = {SendResultType::NetworkError, "refused"};
    TransactionService service{repository, server};

    const auto tx = service.createTransaction(Money{1000});

    REQUIRE(tx.status == TransactionStatus::PendingSync);
    REQUIRE(repository.findById(tx.id)->failedAttempts == 0);
}

// --- SQLite --------------------------------------------------------------------

TEST_CASE("Failure details and dead letters survive a restart", "[dlq][durability][repository]")
{
    TempDatabase db;
    const auto failedAt = std::chrono::system_clock::now();
    std::string id;
    {
        SQLiteTransactionRepository repository{db.path()};
        id = savePending(repository, Money{1000}).id;
        REQUIRE(repository.tryUpdateStatus(id, TransactionStatus::PendingSync, TransactionStatus::Syncing));
        repository.recordFailure(id, TransactionStatus::DeadLetter,
                                 SyncFailure{"REJECTED", "HTTP 422: invalid", failedAt}, false);
    }

    SQLiteTransactionRepository reopened{db.path()};
    const auto deadLetters = reopened.findDeadLetters();

    REQUIRE(deadLetters.size() == 1);
    REQUIRE(deadLetters[0].id == id);
    REQUIRE(deadLetters[0].status == TransactionStatus::DeadLetter);
    REQUIRE(deadLetters[0].lastFailure->code == "REJECTED");
    REQUIRE(deadLetters[0].lastFailure->reason == "HTTP 422: invalid");
    REQUIRE(std::chrono::floor<std::chrono::milliseconds>(deadLetters[0].lastFailure->at)
            == std::chrono::floor<std::chrono::milliseconds>(failedAt));
    REQUIRE(reopened.findPending().empty());
}

TEST_CASE("Recording a failure counts it only when asked to", "[dlq][repository]")
{
    SQLiteTransactionRepository repository{":memory:"};
    const auto id = savePending(repository, Money{1000}).id;
    const SyncFailure failure{"SERVER_ERROR", "HTTP 500", std::chrono::system_clock::now()};

    REQUIRE(repository.tryUpdateStatus(id, TransactionStatus::PendingSync, TransactionStatus::Syncing));
    repository.recordFailure(id, TransactionStatus::PendingSync, failure, true);
    REQUIRE(repository.tryUpdateStatus(id, TransactionStatus::PendingSync, TransactionStatus::Syncing));
    repository.recordFailure(id, TransactionStatus::PendingSync, failure, false);

    REQUIRE(repository.findById(id)->failedAttempts == 1);
    REQUIRE(repository.findById(id)->syncAttempts == 2);
}

TEST_CASE("Recording a failure respects the state machine", "[dlq][repository]")
{
    SQLiteTransactionRepository repository{":memory:"};
    const auto id = savePending(repository, Money{1000}).id;
    const SyncFailure failure{"REJECTED", "bad", std::chrono::system_clock::now()};

    // PENDING_SYNC -> DEAD_LETTER skips the claim.
    REQUIRE_THROWS_AS(repository.recordFailure(id, TransactionStatus::DeadLetter, failure, false),
                      resilientpos::core::InvalidTransitionError);
    REQUIRE_THROWS_AS(repository.recordFailure("missing", TransactionStatus::DeadLetter, failure, false),
                      StorageError);
}

TEST_CASE("Crash recovery leaves dead letters alone", "[dlq][durability][repository]")
{
    SQLiteTransactionRepository repository{":memory:"};
    const auto id = savePending(repository, Money{1000}).id;
    REQUIRE(repository.tryUpdateStatus(id, TransactionStatus::PendingSync, TransactionStatus::Syncing));
    repository.recordFailure(id, TransactionStatus::DeadLetter,
                             SyncFailure{"REJECTED", "bad", std::chrono::system_clock::now()}, false);

    REQUIRE(repository.recoverInterrupted() == 0);
    REQUIRE(statusOf(repository, id) == TransactionStatus::DeadLetter);
}

TEST_CASE("A till database from before dead letters upgrades in place", "[dlq][durability][repository]")
{
    TempDatabase db;
    {
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(db.path().c_str(), &raw) == SQLITE_OK);
        REQUIRE(sqlite3_exec(raw,
            "CREATE TABLE transactions ("
            "id TEXT PRIMARY KEY, amount_cents INTEGER NOT NULL, status TEXT NOT NULL, "
            "created_at INTEGER NOT NULL, sync_attempts INTEGER NOT NULL DEFAULT 0);"
            "INSERT INTO transactions VALUES ('queued', 700, 'PENDING_SYNC', 1, 4);",
            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }

    SQLiteTransactionRepository repository{db.path()};

    const auto pending = repository.findPending();
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].id == "queued");
    REQUIRE(pending[0].syncAttempts == 4);
    REQUIRE(pending[0].failedAttempts == 0);
    REQUIRE_FALSE(pending[0].lastFailure.has_value());
}

// --- End to end ----------------------------------------------------------------

TEST_CASE("Server refuses non-positive amounts as invalid business data", "[dlq][server][handler]")
{
    ProcessedTransactionStore store{":memory:"};

    for (const char* body : {R"({"transaction_id":"a","amount_cents":0})",
                             R"({"transaction_id":"b","amount_cents":-700})"}) {
        INFO("body: " << body);
        REQUIRE(resilientpos::server::handleTransactionRequest(body, store).httpStatus == 422);
    }
    REQUIRE(store.count() == 0);
}

TEST_CASE("IntegrationTest: invalid sale is dead-lettered and the queue keeps moving", "[integration][http][dlq]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    TempDatabase posDb;
    SQLiteTransactionRepository repository{posDb.path()};
    const auto invalid = savePending(repository, Money{-500});
    const auto valid = savePending(repository, Money{700});
    HttpServerClient client{"127.0.0.1", server.port()};
    SyncEngine engine{repository, client};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.synced == 1);
    REQUIRE(report.deadLettered == 1);
    REQUIRE(statusOf(repository, invalid.id) == TransactionStatus::DeadLetter);
    REQUIRE(repository.findById(invalid.id)->lastFailure->code == "REJECTED");
    REQUIRE(statusOf(repository, valid.id) == TransactionStatus::Synced);
    REQUIRE(serverStore.count() == 1);
}

TEST_CASE("IntegrationTest: poison mode, then the operator fixes it and retries", "[integration][http][dlq][simulation]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    server.setMode(NetworkSimulationMode::Poison);
    TempDatabase posDb;
    SQLiteTransactionRepository repository{posDb.path()};
    HttpServerClient client{"127.0.0.1", server.port()};
    SyncEngine engine{repository, client};
    TransactionService service{repository, client};

    const auto tx = service.createTransaction(Money{700});
    REQUIRE(tx.status == TransactionStatus::DeadLetter);
    REQUIRE(serverStore.count() == 0);

    server.setMode(NetworkSimulationMode::Normal);
    REQUIRE(engine.retryDeadLetter(tx.id));
    REQUIRE(engine.syncPendingTransactions().synced == 1);

    REQUIRE(statusOf(repository, tx.id) == TransactionStatus::Synced);
    REQUIRE(serverStore.amountFor(tx.id) == 700);
}

TEST_CASE("IntegrationTest: a conflicting ID ends in the dead letter queue, original intact", "[integration][http][dlq][idempotency]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};
    HttpServerClient client{"127.0.0.1", server.port()};

    // Two tills end up with one ID for two different sales.
    auto tx = Transaction::create(Money{1000});
    REQUIRE(client.send(tx).type == SendResultType::Accepted);

    TempDatabase otherDb;
    SQLiteTransactionRepository otherTill{otherDb.path()};
    tx.status = TransactionStatus::PendingSync;
    tx.amount = Money{10000};
    otherTill.save(tx);
    SyncEngine engine{otherTill, client};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.deadLettered == 1);
    REQUIRE(report.outcomes.front().result == SendResultType::IdempotencyConflict);
    REQUIRE(statusOf(otherTill, tx.id) == TransactionStatus::DeadLetter);
    REQUIRE(serverStore.amountFor(tx.id) == 1000);
    REQUIRE(serverStore.count() == 1);
}
