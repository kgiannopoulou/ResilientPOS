#include "resilientpos/server/ProcessedTransactionStore.hpp"

#include "storage/SqliteSupport.hpp"

#include <chrono>
#include <string_view>

namespace resilientpos::server {

namespace {

using storage::detail::Statement;

constexpr std::string_view kSchema = R"sql(
CREATE TABLE IF NOT EXISTS processed_transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    processed_at INTEGER NOT NULL
);
)sql";

std::int64_t nowEpochMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

void ProcessedTransactionStore::Closer::operator()(sqlite3* db) const noexcept
{
    sqlite3_close(db);
}

ProcessedTransactionStore::ProcessedTransactionStore(const std::string& path)
    : db_{storage::detail::openDatabase(path, kSchema).release()}
{
}

RecordOutcome ProcessedTransactionStore::record(const std::string& id, std::int64_t amountCents)
{
    // "Does it exist? If not, insert" as one atomic statement: there is no
    // window between the check and the write for a second request to slip in.
    Statement stmt{db_.get(),
        "INSERT INTO processed_transactions (id, amount_cents, processed_at) "
        "VALUES (?1, ?2, ?3) ON CONFLICT(id) DO NOTHING"};
    stmt.bind(1, id);
    stmt.bind(2, amountCents);
    stmt.bind(3, nowEpochMillis());
    stmt.step();

    return sqlite3_changes(db_.get()) == 1 ? RecordOutcome::Inserted
                                           : RecordOutcome::AlreadyExists;
}

std::optional<std::int64_t> ProcessedTransactionStore::amountFor(const std::string& id)
{
    Statement stmt{db_.get(), "SELECT amount_cents FROM processed_transactions WHERE id = ?1"};
    stmt.bind(1, id);
    if (!stmt.step()) {
        return std::nullopt;
    }
    return stmt.columnInt64(0);
}

std::size_t ProcessedTransactionStore::count()
{
    Statement stmt{db_.get(), "SELECT COUNT(*) FROM processed_transactions"};
    stmt.step();
    return static_cast<std::size_t>(stmt.columnInt64(0));
}

} // namespace resilientpos::server
