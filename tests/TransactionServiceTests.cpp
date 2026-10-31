#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/storage/StorageError.hpp"
#include "fakes/FakeServerClient.hpp"
#include "fakes/FakeTransactionRepository.hpp"
#include "TempDatabase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::SendResult;
using resilientpos::network::SendResultType;
using resilientpos::service::TransactionEvents;
using resilientpos::service::TransactionService;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::storage::StorageError;
using resilientpos::test::FakeServerClient;
using resilientpos::test::FakeTransactionRepository;
using resilientpos::test::TempDatabase;

TEST_CASE("Service persists the transaction before sending it", "[service]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    std::optional<Transaction> storedWhenSent;
    server.onSend = [&](const Transaction& tx) { storedWhenSent = repository.findById(tx.id); };
    TransactionService service{repository, server};

    const auto tx = service.createTransaction(Money{700});

    REQUIRE(server.sentTransactions.size() == 1);
    REQUIRE(storedWhenSent.has_value());
    REQUIRE(storedWhenSent->id == tx.id);
    // CREATED while in flight: the sync worker leaves it alone.
    REQUIRE(storedWhenSent->status == TransactionStatus::Created);
}

TEST_CASE("Service sends nothing if the sale cannot be stored", "[service]")
{
    FakeTransactionRepository repository;
    repository.failSaves = true;
    FakeServerClient server;
    TransactionService service{repository, server};

    REQUIRE_THROWS_AS(service.createTransaction(Money{700}), StorageError);
    REQUIRE(server.sentTransactions.empty());
}

TEST_CASE("Service marks a successful send SYNCED", "[service]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    TransactionService service{repository, server};

    const auto tx = service.createTransaction(Money{700});

    REQUIRE(tx.status == TransactionStatus::Synced);
    REQUIRE(tx.amount == Money{700});
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::Synced);
    REQUIRE(repository.findPending().empty());
    REQUIRE(service.lastSendResult().type == SendResultType::Accepted);
}

TEST_CASE("Service treats ALREADY_PROCESSED as synced", "[service][idempotency]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    server.resultToReturn = {SendResultType::AlreadyProcessed, "duplicate"};
    TransactionService service{repository, server};

    const auto tx = service.createTransaction(Money{700});

    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::Synced);
}

TEST_CASE("Service turns a network failure into PENDING_SYNC", "[service][offline]")
{
    for (const auto failure : {SendResultType::NetworkError, SendResultType::ServerError}) {
        FakeTransactionRepository repository;
        FakeServerClient server;
        server.resultToReturn = {failure, "down"};
        TransactionService service{repository, server};

        const auto tx = service.createTransaction(Money{550});

        REQUIRE(tx.status == TransactionStatus::PendingSync);
        const auto pending = repository.findPending();
        REQUIRE(pending.size() == 1);
        REQUIRE(pending.front().id == tx.id);
        REQUIRE(pending.front().amount == Money{550});
    }
}

TEST_CASE("Service keeps the sale if the client throws instead of reporting", "[service][offline]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    server.throwOnSend = true;
    TransactionService service{repository, server};

    const auto tx = service.createTransaction(Money{700});

    REQUIRE(tx.status == TransactionStatus::PendingSync);
    REQUIRE(service.lastSendResult().type == SendResultType::NetworkError);
    REQUIRE(repository.findPending().size() == 1);
}

TEST_CASE("Service reports progress through its events", "[service]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    server.resultToReturn = {SendResultType::NetworkError, "down"};

    std::vector<std::string> events;
    TransactionService service{repository, server, TransactionEvents{
        .persisted = [&](const Transaction& tx) {
            events.push_back("persisted " + std::string{toString(tx.status)});
        },
        .completed = [&](const Transaction& tx, const SendResult& result) {
            events.push_back("completed " + std::string{toString(tx.status)} + " "
                             + std::string{toString(result.type)});
        },
    }};

    (void)service.createTransaction(Money{700});

    REQUIRE(events == std::vector<std::string>{
        "persisted CREATED",
        "completed PENDING_SYNC NETWORK_ERROR",
    });
}

TEST_CASE("Offline sales survive a POS restart", "[service][offline][durability]")
{
    TempDatabase db;
    {
        SQLiteTransactionRepository repository{db.path()};
        FakeServerClient server;
        server.resultToReturn = {SendResultType::NetworkError, "down"};
        TransactionService service{repository, server};

        service.createTransaction(Money{550});
        service.createTransaction(Money{1240});
        service.createTransaction(Money{320});
    }

    SQLiteTransactionRepository reopened{db.path()};
    const auto pending = reopened.findPending();
    REQUIRE(pending.size() == 3);
    REQUIRE(pending[0].amount == Money{550});
    REQUIRE(pending[1].amount == Money{1240});
    REQUIRE(pending[2].amount == Money{320});
}
