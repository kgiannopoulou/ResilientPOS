#include "resilientpos/server/ProcessedTransactionStore.hpp"

#include "resilientpos/util/Sha256.hpp"
#include "storage/SqliteSupport.hpp"

#include <chrono>
#include <string_view>
#include <utility>
#include <vector>

namespace resilientpos::server {

namespace {

using storage::detail::Statement;

constexpr std::string_view kSchema = R"sql(
CREATE TABLE IF NOT EXISTS processed_transactions (
    id TEXT PRIMARY KEY,
    amount_cents INTEGER NOT NULL,
    processed_at INTEGER NOT NULL,
    payload_hash TEXT NOT NULL
);
)sql";

// Databases written before payload hashes existed get the column added and
// every row fingerprinted from the amount it already holds. BEGIN IMMEDIATE
// takes the write lock first, so two servers starting together cannot both
// run the ALTER.
void upgradeSchema(sqlite3* db)
{
    using storage::detail::exec;
    using storage::detail::hasColumn;

    if (hasColumn(db, "processed_transactions", "payload_hash")) {
        return;
    }

    exec(db, "BEGIN IMMEDIATE");
    try {
        if (!hasColumn(db, "processed_transactions", "payload_hash")) {
            exec(db, "ALTER TABLE processed_transactions ADD COLUMN payload_hash TEXT");

            std::vector<std::pair<std::string, std::int64_t>> rows;
            Statement select{db, "SELECT id, amount_cents FROM processed_transactions"};
            while (select.step()) {
                rows.emplace_back(select.columnText(0), select.columnInt64(1));
            }
            for (const auto& [id, amountCents] : rows) {
                Statement update{db,
                    "UPDATE processed_transactions SET payload_hash = ?1 WHERE id = ?2"};
                update.bind(1, transactionPayloadHash(amountCents));
                update.bind(2, id);
                update.step();
            }
        }
        exec(db, "COMMIT");
    } catch (...) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::int64_t nowEpochMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::string transactionPayloadHash(std::int64_t amountCents)
{
    // Changing this text invalidates stored hashes, so a new field means a new
    // version prefix, not an edit to v1.
    return util::sha256Hex("resilientpos.transaction.v1\namount_cents="
                           + std::to_string(amountCents) + '\n');
}

void ProcessedTransactionStore::Closer::operator()(sqlite3* db) const noexcept
{
    sqlite3_close(db);
}

ProcessedTransactionStore::ProcessedTransactionStore(const std::string& path)
    : db_{storage::detail::openDatabase(path, kSchema).release()}
{
    upgradeSchema(db_.get());
}

RecordOutcome ProcessedTransactionStore::record(const std::string& id, std::int64_t amountCents)
{
    const std::string payloadHash = transactionPayloadHash(amountCents);

    // "Does it exist? If not, insert" as one atomic statement: there is no
    // window between the check and the write for a second request to slip in.
    Statement insert{db_.get(),
        "INSERT INTO processed_transactions (id, amount_cents, processed_at, payload_hash) "
        "VALUES (?1, ?2, ?3, ?4) ON CONFLICT(id) DO NOTHING"};
    insert.bind(1, id);
    insert.bind(2, amountCents);
    insert.bind(3, nowEpochMillis());
    insert.bind(4, payloadHash);
    insert.step();

    if (sqlite3_changes(db_.get()) == 1) {
        return RecordOutcome::Inserted;
    }

    // Lost the insert, so the row exists. Rows are never updated or deleted,
    // which makes reading it back separately safe: whoever won the race, this
    // is the payload the ID stands for, for good.
    Statement select{db_.get(), "SELECT payload_hash FROM processed_transactions WHERE id = ?1"};
    select.bind(1, id);
    if (!select.step()) {
        throw storage::StorageError{"Transaction '" + id + "' vanished after a duplicate insert"};
    }
    return select.columnText(0) == payloadHash ? RecordOutcome::AlreadyExists
                                               : RecordOutcome::Conflict;
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
