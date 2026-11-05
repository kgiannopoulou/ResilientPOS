#include "resilientpos/server/ProcessedTransactionStore.hpp"

#include "resilientpos/util/Sha256.hpp"
#include "storage/SqliteSupport.hpp"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
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

// A fixed set of connections, each lent to one caller at a time.
class ProcessedTransactionStore::ConnectionPool {
public:
    ConnectionPool(const std::string& path, std::size_t size, std::chrono::milliseconds busyTimeout)
    {
        // Every connection to ":memory:" would be its own empty database.
        const std::size_t count = path == ":memory:" ? 1 : std::max<std::size_t>(size, 1);
        idle_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            idle_.push_back(storage::detail::openDatabase(path, kSchema, busyTimeout));
        }
        upgradeSchema(idle_.front().get());
    }

    // Returns its connection to the pool when it goes out of scope.
    class Lease {
    public:
        Lease(ConnectionPool& pool, storage::detail::SqliteHandle connection)
            : pool_{pool}
            , connection_{std::move(connection)}
        {
        }
        ~Lease() { pool_.release(std::move(connection_)); }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        [[nodiscard]]
        sqlite3* get() const noexcept { return connection_.get(); }

    private:
        ConnectionPool& pool_;
        storage::detail::SqliteHandle connection_;
    };

    [[nodiscard]]
    Lease acquire()
    {
        std::unique_lock lock{mutex_};
        available_.wait(lock, [this] { return !idle_.empty(); });
        auto connection = std::move(idle_.back());
        idle_.pop_back();
        return Lease{*this, std::move(connection)};
    }

private:
    void release(storage::detail::SqliteHandle connection)
    {
        {
            const std::scoped_lock lock{mutex_};
            idle_.push_back(std::move(connection));
        }
        available_.notify_one();
    }

    std::mutex mutex_;
    std::condition_variable available_;
    std::vector<storage::detail::SqliteHandle> idle_;
};

ProcessedTransactionStore::ProcessedTransactionStore(
    const std::string& path,
    std::size_t connections,
    std::chrono::milliseconds busyTimeout)
    : pool_{std::make_unique<ConnectionPool>(path, connections, busyTimeout)}
{
}

ProcessedTransactionStore::~ProcessedTransactionStore() = default;

RecordOutcome ProcessedTransactionStore::record(const std::string& id, std::int64_t amountCents)
{
    const std::string payloadHash = transactionPayloadHash(amountCents);
    const auto connection = pool_->acquire();
    sqlite3* db = connection.get();

    // "Does it exist? If not, insert" as one atomic statement: there is no
    // window between the check and the write for a second request to slip in.
    Statement insert{db,
        "INSERT INTO processed_transactions (id, amount_cents, processed_at, payload_hash) "
        "VALUES (?1, ?2, ?3, ?4) ON CONFLICT(id) DO NOTHING"};
    insert.bind(1, id);
    insert.bind(2, amountCents);
    insert.bind(3, nowEpochMillis());
    insert.bind(4, payloadHash);
    insert.step();

    if (sqlite3_changes(db) == 1) {
        return RecordOutcome::Inserted;
    }

    // Lost the insert, so the row exists. Rows are never updated or deleted,
    // which makes reading it back separately safe: whoever won the race, this
    // is the payload the ID stands for, for good.
    Statement select{db, "SELECT payload_hash FROM processed_transactions WHERE id = ?1"};
    select.bind(1, id);
    if (!select.step()) {
        throw storage::StorageError{"Transaction '" + id + "' vanished after a duplicate insert"};
    }
    return select.columnText(0) == payloadHash ? RecordOutcome::AlreadyExists
                                               : RecordOutcome::Conflict;
}

std::optional<std::int64_t> ProcessedTransactionStore::amountFor(const std::string& id)
{
    const auto connection = pool_->acquire();
    Statement stmt{connection.get(), "SELECT amount_cents FROM processed_transactions WHERE id = ?1"};
    stmt.bind(1, id);
    if (!stmt.step()) {
        return std::nullopt;
    }
    return stmt.columnInt64(0);
}

std::size_t ProcessedTransactionStore::count()
{
    const auto connection = pool_->acquire();
    Statement stmt{connection.get(), "SELECT COUNT(*) FROM processed_transactions"};
    stmt.step();
    return static_cast<std::size_t>(stmt.columnInt64(0));
}

bool ProcessedTransactionStore::ping() noexcept
{
    try {
        const auto connection = pool_->acquire();
        Statement stmt{connection.get(), "SELECT 1 FROM processed_transactions LIMIT 1"};
        (void)stmt.step();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace resilientpos::server
