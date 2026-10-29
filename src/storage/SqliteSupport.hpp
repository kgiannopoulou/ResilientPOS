#pragma once

// Internal SQLite helpers shared by the POS repository and the server store.
// Not part of the public include/ tree: nothing outside src/ sees sqlite3.h.

#include "resilientpos/storage/StorageError.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace resilientpos::storage::detail {

[[noreturn]]
inline void fail(sqlite3* db, std::string_view what)
{
    std::string message{what};
    message += ": ";
    message += db != nullptr ? sqlite3_errmsg(db) : "out of memory";
    throw StorageError{message};
}

inline void exec(sqlite3* db, std::string_view sql)
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

struct SqliteCloser {
    void operator()(sqlite3* db) const noexcept { sqlite3_close(db); }
};

using SqliteHandle = std::unique_ptr<sqlite3, SqliteCloser>;

// Opens (or creates) the database at `path` with the durability settings both
// sides rely on, then runs `schema`.
inline SqliteHandle openDatabase(const std::string& path, std::string_view schema)
{
    sqlite3* raw = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &raw,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    // sqlite3_open_v2 may hand back a handle even on failure; own it either way.
    SqliteHandle db{raw};
    if (rc != SQLITE_OK) {
        fail(db.get(), "Cannot open database '" + path + "'");
    }

    // Wait briefly instead of failing if another process holds the lock.
    sqlite3_busy_timeout(db.get(), 5000);

    // WAL survives crashes and lets readers run alongside the writer;
    // synchronous=FULL fsyncs every commit so an acknowledged write is on disk.
    exec(db.get(), "PRAGMA journal_mode=WAL;");
    exec(db.get(), "PRAGMA synchronous=FULL;");
    exec(db.get(), schema);
    return db;
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

} // namespace resilientpos::storage::detail
