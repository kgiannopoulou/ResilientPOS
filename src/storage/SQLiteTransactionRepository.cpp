#include "resilientpos/storage/SQLiteTransactionRepository.hpp"

#include "resilientpos/storage/StorageError.hpp"
#include "storage/SqliteSupport.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>

namespace resilientpos::storage {

namespace {

using core::TransactionStatus;
using detail::Statement;

constexpr std::string_view kSchema = R"sql(
CREATE TABLE IF NOT EXISTS transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    status TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    sync_attempts INTEGER NOT NULL DEFAULT 0,
    failed_attempts INTEGER NOT NULL DEFAULT 0,
    last_failure_code TEXT,
    last_failure_reason TEXT,
    last_failure_at INTEGER
);
)sql";

// Databases from before the dead letter queue get the new columns. A till may
// hold unsynced sales in that file, so it is upgraded in place, never replaced.
void upgradeSchema(sqlite3* db)
{
    if (detail::hasColumn(db, "transactions", "failed_attempts")) {
        return;
    }
    detail::exec(db, R"sql(
BEGIN IMMEDIATE;
ALTER TABLE transactions ADD COLUMN failed_attempts INTEGER NOT NULL DEFAULT 0;
ALTER TABLE transactions ADD COLUMN last_failure_code TEXT;
ALTER TABLE transactions ADD COLUMN last_failure_reason TEXT;
ALTER TABLE transactions ADD COLUMN last_failure_at INTEGER;
COMMIT;
)sql");
}

// Timestamps are stored as milliseconds since the Unix epoch.
std::int64_t toEpochMillis(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

std::chrono::system_clock::time_point fromEpochMillis(std::int64_t millis)
{
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds{millis})};
}

constexpr std::string_view kSelectColumns =
    "SELECT id, amount_cents, status, created_at, sync_attempts, failed_attempts, "
    "last_failure_code, last_failure_reason, last_failure_at FROM transactions ";

core::Transaction readTransaction(const Statement& stmt)
{
    const std::string statusText = stmt.columnText(2);
    const auto status = core::transactionStatusFromString(statusText);
    if (!status) {
        throw StorageError{"Unknown transaction status in database: " + statusText};
    }

    std::optional<core::SyncFailure> lastFailure;
    if (!stmt.columnIsNull(6)) {
        lastFailure = core::SyncFailure{
            .code = stmt.columnText(6),
            .reason = stmt.columnText(7),
            .at = fromEpochMillis(stmt.columnInt64(8)),
        };
    }

    return core::Transaction{
        .id = stmt.columnText(0),
        .amount = core::Money{stmt.columnInt64(1)},
        .status = *status,
        .createdAt = fromEpochMillis(stmt.columnInt64(3)),
        .syncAttempts = static_cast<unsigned int>(stmt.columnInt64(4)),
        .failedAttempts = static_cast<unsigned int>(stmt.columnInt64(5)),
        .lastFailure = std::move(lastFailure),
    };
}

std::vector<core::Transaction> readAll(Statement& stmt)
{
    std::vector<core::Transaction> rows;
    while (stmt.step()) {
        rows.push_back(readTransaction(stmt));
    }
    return rows;
}

} // namespace

void SQLiteTransactionRepository::Closer::operator()(sqlite3* db) const noexcept
{
    sqlite3_close(db);
}

SQLiteTransactionRepository::SQLiteTransactionRepository(const std::string& path)
    : db_{detail::openDatabase(path, kSchema).release()}
{
    upgradeSchema(db_.get());
}

void SQLiteTransactionRepository::save(const core::Transaction& transaction)
{
    const std::scoped_lock lock{mutex_};
    Statement stmt{db_.get(),
        "INSERT INTO transactions (id, amount_cents, status, created_at, sync_attempts) "
        "VALUES (?1, ?2, ?3, ?4, ?5)"};
    stmt.bind(1, transaction.id);
    stmt.bind(2, transaction.amount.cents());
    stmt.bind(3, core::toString(transaction.status));
    stmt.bind(4, toEpochMillis(transaction.createdAt));
    stmt.bind(5, static_cast<std::int64_t>(transaction.syncAttempts));
    stmt.step();
}

std::optional<core::Transaction>
SQLiteTransactionRepository::findById(const std::string& id)
{
    const std::scoped_lock lock{mutex_};
    return findByIdLocked(id);
}

std::vector<core::Transaction> SQLiteTransactionRepository::findAll()
{
    const std::scoped_lock lock{mutex_};
    Statement stmt{db_.get(), std::string{kSelectColumns} + "ORDER BY created_at, rowid"};
    return readAll(stmt);
}

std::vector<core::Transaction> SQLiteTransactionRepository::findPending()
{
    const std::scoped_lock lock{mutex_};
    Statement stmt{db_.get(),
        std::string{kSelectColumns} + "WHERE status IN (?1, ?2) ORDER BY created_at, rowid"};
    stmt.bind(1, core::toString(TransactionStatus::PendingSync));
    stmt.bind(2, core::toString(TransactionStatus::Syncing));
    return readAll(stmt);
}

std::vector<core::Transaction> SQLiteTransactionRepository::findDeadLetters()
{
    const std::scoped_lock lock{mutex_};
    Statement stmt{db_.get(),
        std::string{kSelectColumns} + "WHERE status = ?1 ORDER BY created_at, rowid"};
    stmt.bind(1, core::toString(TransactionStatus::DeadLetter));
    return readAll(stmt);
}

void SQLiteTransactionRepository::updateStatus(const std::string& id, TransactionStatus status)
{
    const std::scoped_lock lock{mutex_};
    const auto current = findByIdLocked(id);
    if (!current) {
        throw StorageError{"No transaction with id '" + id + "'"};
    }
    if (!core::canTransition(current->status, status)) {
        throw core::InvalidTransitionError{current->status, status};
    }
    writeStatusLocked(id, status);
}

bool SQLiteTransactionRepository::tryUpdateStatus(
    const std::string& id,
    TransactionStatus expected,
    TransactionStatus to)
{
    if (!core::canTransition(expected, to)) {
        throw core::InvalidTransitionError{expected, to};
    }

    const std::scoped_lock lock{mutex_};
    const auto current = findByIdLocked(id);
    if (!current) {
        throw StorageError{"No transaction with id '" + id + "'"};
    }
    if (current->status != expected) {
        return false;
    }
    writeStatusLocked(id, to);
    return true;
}

void SQLiteTransactionRepository::recordFailure(
    const std::string& id,
    TransactionStatus status,
    const core::SyncFailure& failure,
    bool countsAgainstLimit)
{
    const std::scoped_lock lock{mutex_};
    const auto current = findByIdLocked(id);
    if (!current) {
        throw StorageError{"No transaction with id '" + id + "'"};
    }
    if (!core::canTransition(current->status, status)) {
        throw core::InvalidTransitionError{current->status, status};
    }

    Statement stmt{db_.get(),
        "UPDATE transactions SET status = ?1, failed_attempts = failed_attempts + ?2, "
        "last_failure_code = ?3, last_failure_reason = ?4, last_failure_at = ?5 WHERE id = ?6"};
    stmt.bind(1, core::toString(status));
    stmt.bind(2, std::int64_t{countsAgainstLimit ? 1 : 0});
    stmt.bind(3, failure.code);
    stmt.bind(4, failure.reason);
    stmt.bind(5, toEpochMillis(failure.at));
    stmt.bind(6, id);
    stmt.step();
}

bool SQLiteTransactionRepository::requeueDeadLetter(const std::string& id)
{
    const std::scoped_lock lock{mutex_};
    const auto current = findByIdLocked(id);
    if (!current) {
        throw StorageError{"No transaction with id '" + id + "'"};
    }
    if (current->status != TransactionStatus::DeadLetter) {
        return false;
    }

    Statement stmt{db_.get(),
        "UPDATE transactions SET status = ?1, failed_attempts = 0 WHERE id = ?2"};
    stmt.bind(1, core::toString(TransactionStatus::PendingSync));
    stmt.bind(2, id);
    stmt.step();
    return true;
}

std::size_t SQLiteTransactionRepository::recoverInterrupted()
{
    const std::scoped_lock lock{mutex_};
    // CREATED -> PENDING_SYNC and SYNCING -> PENDING_SYNC are both legal moves.
    Statement stmt{db_.get(), "UPDATE transactions SET status = ?1 WHERE status IN (?2, ?3)"};
    stmt.bind(1, core::toString(TransactionStatus::PendingSync));
    stmt.bind(2, core::toString(TransactionStatus::Created));
    stmt.bind(3, core::toString(TransactionStatus::Syncing));
    stmt.step();
    return static_cast<std::size_t>(sqlite3_changes(db_.get()));
}

std::optional<core::Transaction>
SQLiteTransactionRepository::findByIdLocked(const std::string& id)
{
    Statement stmt{db_.get(), std::string{kSelectColumns} + "WHERE id = ?1"};
    stmt.bind(1, id);
    if (!stmt.step()) {
        return std::nullopt;
    }
    return readTransaction(stmt);
}

void SQLiteTransactionRepository::writeStatusLocked(const std::string& id, TransactionStatus to)
{
    // Every move into SYNCING is one more attempt at getting the sale across.
    Statement stmt{db_.get(),
        "UPDATE transactions SET status = ?1, "
        "sync_attempts = sync_attempts + CASE WHEN ?1 = ?3 THEN 1 ELSE 0 END "
        "WHERE id = ?2"};
    stmt.bind(1, core::toString(to));
    stmt.bind(2, id);
    stmt.bind(3, core::toString(TransactionStatus::Syncing));
    stmt.step();
}

} // namespace resilientpos::storage
