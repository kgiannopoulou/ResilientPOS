#include "resilientpos/storage/SQLiteTransactionRepository.hpp"

#include "resilientpos/storage/StorageError.hpp"
#include "storage/SqliteSupport.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace resilientpos::storage {

namespace {

using detail::Statement;

constexpr std::string_view kSchema = R"sql(
CREATE TABLE IF NOT EXISTS transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    status TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    sync_attempts INTEGER NOT NULL DEFAULT 0
);
)sql";

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
    "SELECT id, amount_cents, status, created_at, sync_attempts FROM transactions ";

core::Transaction readTransaction(const Statement& stmt)
{
    const std::string statusText = stmt.columnText(2);
    const auto status = core::transactionStatusFromString(statusText);
    if (!status) {
        throw StorageError{"Unknown transaction status in database: " + statusText};
    }

    return core::Transaction{
        .id = stmt.columnText(0),
        .amount = core::Money{stmt.columnInt64(1)},
        .status = *status,
        .createdAt = fromEpochMillis(stmt.columnInt64(3)),
        .syncAttempts = static_cast<unsigned int>(stmt.columnInt64(4)),
    };
}

} // namespace

void SQLiteTransactionRepository::Closer::operator()(sqlite3* db) const noexcept
{
    sqlite3_close(db);
}

SQLiteTransactionRepository::SQLiteTransactionRepository(const std::string& path)
    : db_{detail::openDatabase(path, kSchema).release()}
{
}

void SQLiteTransactionRepository::save(const core::Transaction& transaction)
{
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
    Statement stmt{db_.get(), std::string{kSelectColumns} + "WHERE id = ?1"};
    stmt.bind(1, id);
    if (!stmt.step()) {
        return std::nullopt;
    }
    return readTransaction(stmt);
}

std::vector<core::Transaction> SQLiteTransactionRepository::findPending()
{
    Statement stmt{db_.get(),
        std::string{kSelectColumns} + "WHERE status IN (?1, ?2) ORDER BY created_at, rowid"};
    stmt.bind(1, core::toString(core::TransactionStatus::PendingSync));
    // SYNCING only survives in the database if the till stopped mid-sync;
    // whether the server got it is unknown, so it must be sent again.
    stmt.bind(2, core::toString(core::TransactionStatus::Syncing));

    std::vector<core::Transaction> pending;
    while (stmt.step()) {
        pending.push_back(readTransaction(stmt));
    }
    return pending;
}

void SQLiteTransactionRepository::updateStatus(
    const std::string& id,
    core::TransactionStatus status)
{
    Statement stmt{db_.get(), "UPDATE transactions SET status = ?1 WHERE id = ?2"};
    stmt.bind(1, core::toString(status));
    stmt.bind(2, id);
    stmt.step();

    if (sqlite3_changes(db_.get()) == 0) {
        throw StorageError{"No transaction with id '" + id + "'"};
    }
}

} // namespace resilientpos::storage
