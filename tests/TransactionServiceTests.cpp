#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "TempDatabase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <string>

using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::SendResult;
using resilientpos::network::SendResultType;
using resilientpos::network::ServerClient;
using resilientpos::service::TransactionService;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::storage::TransactionRepository;
using resilientpos::test::TempDatabase;

namespace {

// Answers every send with a fixed result and records what the repository
// held at the moment of sending, to prove "persist first, send second".
class FakeServerClient final : public ServerClient {
public:
    FakeServerClient(TransactionRepository& repository, SendResultType answer)
        : repository_{repository}
        , answer_{answer}
    {
    }

    SendResult send(const Transaction& transaction) override
    {
        ++calls;
        storedWhenSent = repository_.findById(transaction.id);
        if (throwInstead) {
            throw std::runtime_error{"socket exploded"};
        }
        return {answer_, "fake"};
    }

    int calls{0};
    bool throwInstead{false};
    std::optional<Transaction> storedWhenSent;

private:
    TransactionRepository& repository_;
    SendResultType answer_;
};

} // namespace

TEST_CASE("Service saves the transaction before sending it", "[service]")
{
    SQLiteTransactionRepository repository{":memory:"};
    FakeServerClient server{repository, SendResultType::Accepted};
    TransactionService service{repository, server};

    const auto tx = service.createTransaction(Money{700});

    REQUIRE(server.calls == 1);
    REQUIRE(server.storedWhenSent.has_value());
    REQUIRE(server.storedWhenSent->id == tx.id);
    REQUIRE(server.storedWhenSent->status == TransactionStatus::PendingSync);
}

TEST_CASE("Service marks an accepted transaction SYNCED", "[service]")
{
    SQLiteTransactionRepository repository{":memory:"};
    FakeServerClient server{repository, SendResultType::Accepted};
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
    SQLiteTransactionRepository repository{":memory:"};
    FakeServerClient server{repository, SendResultType::AlreadyProcessed};
    TransactionService service{repository, server};

    const auto tx = service.createTransaction(Money{700});

    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::Synced);
}

TEST_CASE("Service keeps the sale as PENDING_SYNC when the server is unreachable", "[service][offline]")
{
    for (const auto failure : {SendResultType::NetworkError, SendResultType::ServerError}) {
        SQLiteTransactionRepository repository{":memory:"};
        FakeServerClient server{repository, failure};
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
    SQLiteTransactionRepository repository{":memory:"};
    FakeServerClient server{repository, SendResultType::Accepted};
    server.throwInstead = true;
    TransactionService service{repository, server};

    const auto tx = service.createTransaction(Money{700});

    REQUIRE(tx.status == TransactionStatus::PendingSync);
    REQUIRE(service.lastSendResult().type == SendResultType::NetworkError);
    REQUIRE(repository.findPending().size() == 1);
}

TEST_CASE("Offline sales survive a POS restart", "[service][offline]")
{
    TempDatabase db;
    {
        SQLiteTransactionRepository repository{db.path()};
        FakeServerClient server{repository, SendResultType::NetworkError};
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
