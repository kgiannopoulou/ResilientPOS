#include "resilientpos/storage/SQLiteTransactionRepository.hpp"

#include "resilientpos/storage/StorageError.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <string_view>
#include <utility>

namespace resilientpos::storage {

namespace {

constexpr std::string_view kSchema = R"sql(
CREATE TABLE IF NOT EXISTS transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    status TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    sync_attempts INTEGER NOT NULL DEFAULT 0
);
)sql";

[[noreturn]]
void fail(sqlite3* db, std::string_view what)
{
    std::string message{what};
    message += ": ";
    message += db != nullptr ? sqlite3_errmsg(db) : "out of memory";
    throw StorageError{message};
}

void exec(sqlite3* db, std::string_view sql)
{
    // sqlite3_exec needs a NUL-terminated string.
    const std::string text{sql};
    char* error = nullptr;
    if (sqlite3_exec(db, text.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = "SQLite exec failed: ";
        message += error != nullptr ? error : "unknown error";
        sqlite3_free(error);
        throw StorageError{message};
    }
}

// RAII wrapper around a prepared statement: finalized automatically,
// including when a bind or step throws halfway through.
class Statement {
public:
    Statement(sqlite3* db, std::string_view sql)
        : db_{db}
    {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw, nullptr)
            != SQLITE_OK) {
            fail(db, "SQLite prepare failed");
        }
        stmt_.reset(raw);
    }

    void bind(int index, std::string_view text)
    {
        check(sqlite3_bind_text(stmt_.get(), index, text.data(),
                                static_cast<int>(text.size()), SQLITE_TRANSIENT));
    }

    void bind(int index, std::int64_t value)
    {
        check(sqlite3_bind_int64(stmt_.get(), index, value));
    }

    // Advances the statement; true while it yields rows, false when done.
    bool step()
    {
        const int rc = sqlite3_step(stmt_.get());
        if (rc == SQLITE_ROW) {
            return true;
        }
        if (rc == SQLITE_DONE) {
            return false;
        }
        fail(db_, "SQLite step failed");
    }

    [[nodiscard]]
    std::int64_t columnInt64(int column) const
    {
        return sqlite3_column_int64(stmt_.get(), column);
    }

    [[nodiscard]]
    std::string columnText(int column) const
    {
        const auto* text = sqlite3_column_text(stmt_.get(), column);
        const int size = sqlite3_column_bytes(stmt_.get(), column);
        if (text == nullptr) {
            return {};
        }
        return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(size)};
    }

private:
    struct Finalizer {
        void operator()(sqlite3_stmt* stmt) const noexcept { sqlite3_finalize(stmt); }
    };

    void check(int rc)
    {
        if (rc != SQLITE_OK) {
            fail(db_, "SQLite bind failed");
        }
    }

    sqlite3* db_;
    std::unique_ptr<sqlite3_stmt, Finalizer> stmt_;
};

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
{
    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &raw,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    // sqlite3_open_v2 may hand back a handle even on failure; own it either way.
    db_.reset(raw);
    if (rc != SQLITE_OK) {
        fail(db_.get(), "Cannot open database '" + path + "'");
    }

    // Wait briefly instead of failing if another process holds the lock.
    sqlite3_busy_timeout(db_.get(), 5000);

    // WAL survives crashes and lets readers run alongside the writer;
    // synchronous=FULL fsyncs every commit so an acknowledged sale is on disk.
    exec(db_.get(), "PRAGMA journal_mode=WAL;");
    exec(db_.get(), "PRAGMA synchronous=FULL;");
    exec(db_.get(), kSchema);
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
        std::string{kSelectColumns} + "WHERE status = ?1 ORDER BY created_at, rowid"};
    stmt.bind(1, core::toString(core::TransactionStatus::PendingSync));

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
