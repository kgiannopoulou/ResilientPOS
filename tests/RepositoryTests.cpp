#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/storage/StorageError.hpp"
#include "TempDatabase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

using resilientpos::core::InvalidTransitionError;
using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::storage::StorageError;
using resilientpos::test::TempDatabase;

namespace {

// SQLite keeps millisecond precision, so compare timestamps at that resolution.
std::chrono::milliseconds millis(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
}

void requireSameTransaction(const Transaction& actual, const Transaction& expected)
{
    REQUIRE(actual.id == expected.id);
    REQUIRE(actual.amount == expected.amount);
    REQUIRE(actual.status == expected.status);
    REQUIRE(millis(actual.createdAt) == millis(expected.createdAt));
    REQUIRE(actual.syncAttempts == expected.syncAttempts);
}

} // namespace

TEST_CASE("Repository saves a transaction", "[repository]")
{
    TempDatabase db;
    SQLiteTransactionRepository repository{db.path()};

    const auto tx = Transaction::create(Money{700});

    REQUIRE_NOTHROW(repository.save(tx));
}

TEST_CASE("Repository retrieves a saved transaction", "[repository]")
{
    TempDatabase db;
    SQLiteTransactionRepository repository{db.path()};

    auto tx = Transaction::create(Money{1299});
    tx.syncAttempts = 3;
    repository.save(tx);

    const auto found = repository.findById(tx.id);

    REQUIRE(found.has_value());
    requireSameTransaction(*found, tx);
}

TEST_CASE("Repository returns nullopt for unknown ID", "[repository]")
{
    TempDatabase db;
    SQLiteTransactionRepository repository{db.path()};
    repository.save(Transaction::create(Money{350}));

    REQUIRE_FALSE(repository.findById("does-not-exist").has_value());
}

TEST_CASE("Repository rejects a duplicate ID", "[repository]")
{
    TempDatabase db;
    SQLiteTransactionRepository repository{db.path()};

    const auto tx = Transaction::create(Money{350});
    repository.save(tx);

    REQUIRE_THROWS_AS(repository.save(tx), StorageError);
}

TEST_CASE("Repository updates status", "[repository]")
{
    TempDatabase db;
    SQLiteTransactionRepository repository{db.path()};

    const auto tx = Transaction::create(Money{350});
    repository.save(tx);

    repository.updateStatus(tx.id, TransactionStatus::PendingSync);
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::PendingSync);

    repository.updateStatus(tx.id, TransactionStatus::Syncing);
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::Syncing);

    repository.updateStatus(tx.id, TransactionStatus::Synced);
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::Synced);
}

TEST_CASE("Repository rejects transitions the state machine forbids", "[repository][state-machine]")
{
    SQLiteTransactionRepository repository{":memory:"};
    const auto tx = Transaction::create(Money{350});
    repository.save(tx);
    repository.updateStatus(tx.id, TransactionStatus::Synced);

    REQUIRE_THROWS_AS(repository.updateStatus(tx.id, TransactionStatus::Created),
                      InvalidTransitionError);
    REQUIRE_THROWS_AS(repository.updateStatus(tx.id, TransactionStatus::PendingSync),
                      InvalidTransitionError);
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::Synced);
}

TEST_CASE("Repository compare-and-set only moves a row in the expected state", "[repository]")
{
    SQLiteTransactionRepository repository{":memory:"};
    auto tx = Transaction::create(Money{350});
    tx.status = TransactionStatus::PendingSync;
    repository.save(tx);

    // First claim wins, a second claim on the same row loses.
    REQUIRE(repository.tryUpdateStatus(tx.id, TransactionStatus::PendingSync, TransactionStatus::Syncing));
    REQUIRE_FALSE(repository.tryUpdateStatus(tx.id, TransactionStatus::PendingSync, TransactionStatus::Syncing));
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::Syncing);

    REQUIRE_THROWS_AS(
        repository.tryUpdateStatus(tx.id, TransactionStatus::Synced, TransactionStatus::Created),
        InvalidTransitionError);
    REQUIRE_THROWS_AS(
        repository.tryUpdateStatus("missing", TransactionStatus::PendingSync, TransactionStatus::Syncing),
        StorageError);
}

TEST_CASE("Repository counts every move into SYNCING as a sync attempt", "[repository]")
{
    SQLiteTransactionRepository repository{":memory:"};
    auto tx = Transaction::create(Money{350});
    tx.status = TransactionStatus::PendingSync;
    repository.save(tx);

    for (int attempt = 0; attempt < 3; ++attempt) {
        repository.updateStatus(tx.id, TransactionStatus::Syncing);
        repository.updateStatus(tx.id, TransactionStatus::PendingSync);
    }

    REQUIRE(repository.findById(tx.id)->syncAttempts == 3);
}

TEST_CASE("Repository requeues sales that were in flight when the till died", "[repository][durability]")
{
    TempDatabase db;
    const auto inFlightFirstSend = Transaction::create(Money{100});  // CREATED
    auto inFlightResend = Transaction::create(Money{200});
    inFlightResend.status = TransactionStatus::Syncing;
    auto synced = Transaction::create(Money{300});
    synced.status = TransactionStatus::Synced;
    {
        SQLiteTransactionRepository repository{db.path()};
        repository.save(inFlightFirstSend);
        repository.save(inFlightResend);
        repository.save(synced);
    } // crash

    SQLiteTransactionRepository reopened{db.path()};
    REQUIRE(reopened.recoverInterrupted() == 2);

    REQUIRE(reopened.findById(inFlightFirstSend.id)->status == TransactionStatus::PendingSync);
    REQUIRE(reopened.findById(inFlightResend.id)->status == TransactionStatus::PendingSync);
    REQUIRE(reopened.findById(synced.id)->status == TransactionStatus::Synced);
    REQUIRE(reopened.recoverInterrupted() == 0);
}

TEST_CASE("Repository lists every transaction oldest first", "[repository]")
{
    SQLiteTransactionRepository repository{":memory:"};
    const auto now = std::chrono::system_clock::now();
    auto older = Transaction::create(Money{100});
    older.createdAt = now - std::chrono::minutes{1};
    auto newer = Transaction::create(Money{200});
    newer.createdAt = now;
    newer.status = TransactionStatus::Synced;
    repository.save(newer);
    repository.save(older);

    const auto all = repository.findAll();

    REQUIRE(all.size() == 2);
    REQUIRE(all[0].id == older.id);
    REQUIRE(all[1].id == newer.id);
}

TEST_CASE("Repository throws when updating an unknown ID", "[repository]")
{
    TempDatabase db;
    SQLiteTransactionRepository repository{db.path()};

    REQUIRE_THROWS_AS(
        repository.updateStatus("does-not-exist", TransactionStatus::Synced),
        StorageError);
}

TEST_CASE("Repository retrieves pending transactions", "[repository]")
{
    TempDatabase db;
    SQLiteTransactionRepository repository{db.path()};

    const auto now = std::chrono::system_clock::now();

    auto older = Transaction::create(Money{100});
    older.createdAt = now - std::chrono::minutes{5};
    auto newer = Transaction::create(Money{200});
    newer.createdAt = now;
    const auto created = Transaction::create(Money{300});
    const auto synced = Transaction::create(Money{400});

    // Save out of order to prove findPending() sorts by creation time.
    repository.save(newer);
    repository.save(older);
    repository.save(created);
    repository.save(synced);

    repository.updateStatus(newer.id, TransactionStatus::PendingSync);
    repository.updateStatus(older.id, TransactionStatus::PendingSync);
    repository.updateStatus(synced.id, TransactionStatus::Synced);

    const auto pending = repository.findPending();

    REQUIRE(pending.size() == 2);
    REQUIRE(pending[0].id == older.id);
    REQUIRE(pending[1].id == newer.id);
    for (const auto& tx : pending) {
        REQUIRE(tx.status == TransactionStatus::PendingSync);
    }
}

TEST_CASE("Repository returns a transaction left SYNCING by an interrupted sync", "[repository][durability]")
{
    TempDatabase db;
    const auto tx = Transaction::create(Money{700});

    {
        SQLiteTransactionRepository repository{db.path()};
        repository.save(tx);
        repository.updateStatus(tx.id, TransactionStatus::PendingSync);
        repository.updateStatus(tx.id, TransactionStatus::Syncing);
    } // till dies before the server's answer arrives

    SQLiteTransactionRepository reopened{db.path()};
    const auto pending = reopened.findPending();
    REQUIRE(pending.size() == 1);
    REQUIRE(pending.front().id == tx.id);
    REQUIRE(pending.front().status == TransactionStatus::Syncing);
}

TEST_CASE("Repository returns no pending transactions for an empty database", "[repository]")
{
    TempDatabase db;
    SQLiteTransactionRepository repository{db.path()};

    REQUIRE(repository.findPending().empty());
}

TEST_CASE("Data survives closing and reopening the database", "[repository][durability]")
{
    TempDatabase db;
    auto tx = Transaction::create(Money{700});

    {
        SQLiteTransactionRepository repository{db.path()};
        repository.save(tx);
        repository.updateStatus(tx.id, TransactionStatus::PendingSync);
    } // repository destroyed -> database closed, as if the POS exited

    SQLiteTransactionRepository reopened{db.path()};

    tx.status = TransactionStatus::PendingSync;
    const auto found = reopened.findById(tx.id);
    REQUIRE(found.has_value());
    requireSameTransaction(*found, tx);

    const auto pending = reopened.findPending();
    REQUIRE(pending.size() == 1);
    REQUIRE(pending.front().id == tx.id);
}

TEST_CASE("Repository fails clearly when the database cannot be opened", "[repository]")
{
    const auto missingDir = std::filesystem::temp_directory_path() /
                            ("resilientpos-missing-" + resilientpos::util::UuidGenerator::generate());
    const auto path = (missingDir / "pos.db").string();

    REQUIRE_THROWS_AS(SQLiteTransactionRepository{path}, StorageError);
}
