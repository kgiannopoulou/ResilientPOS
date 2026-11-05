#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace resilientpos::server {

enum class RecordOutcome {
    Inserted,       // first time the server has seen this transaction ID
    AlreadyExists,  // same ID, same payload: a retry, nothing was written
    Conflict        // same ID, DIFFERENT payload: refused, nothing was written
};

// Fingerprint of everything in a transaction except its ID: SHA-256 over a
// versioned canonical form. Comparing fingerprints rather than individual
// fields means line items, tax or payment method only have to be added to the
// canonical form, and the idempotency check keeps working unchanged.
[[nodiscard]]
std::string transactionPayloadHash(std::int64_t amountCents);

// The central server's record of transactions it has accepted.
//
// Lives in its own database file (data/server.db), never the till's. The
// transaction ID is the primary key, so recording the same sale twice is a
// no-op: that is what makes POS retries safe.
//
// An idempotency key is only safe if it stands for one specific request, so
// each row keeps a hash of the payload it was first recorded with:
//
//   same ID + same hash       -> AlreadyExists (a retry; answer success)
//   same ID + different hash  -> Conflict      (never accepted, never overwritten)
//
// Threading: safe to call from any number of threads. Each call borrows a
// connection from a small pool for its whole duration, so a connection is never
// shared (sqlite3_changes(), which decides Inserted vs not, is per connection).
// Waiting for a free connection blocks; it does not fail.
//
// Nothing here serializes writers, on purpose. Two threads can race to insert
// one ID, and so can two server processes on one file, where no in-process lock
// could help. What makes that safe is the PRIMARY KEY and the single atomic
// INSERT ... ON CONFLICT DO NOTHING: exactly one writer wins, in the database.
class ProcessedTransactionStore {
public:
    static constexpr std::size_t kDefaultConnections = 4;

    // Opens (or creates) the database at `path`, with `connections` pooled
    // connections. ":memory:" for tests: a private database per connection,
    // so that always uses exactly one. `busyTimeout` is how long a write waits
    // for the database lock before throwing storage::StorageBusyError.
    explicit ProcessedTransactionStore(
        const std::string& path,
        std::size_t connections = kDefaultConnections,
        std::chrono::milliseconds busyTimeout = std::chrono::seconds{5}
    );
    ~ProcessedTransactionStore();

    ProcessedTransactionStore(const ProcessedTransactionStore&) = delete;
    ProcessedTransactionStore& operator=(const ProcessedTransactionStore&) = delete;

    // Records the transaction if its ID is new. A known ID leaves the stored
    // row untouched; the outcome says whether the payload matched.
    RecordOutcome record(const std::string& id, std::int64_t amountCents);

    // Stored amount for `id`, or nullopt if the server never accepted it.
    [[nodiscard]]
    std::optional<std::int64_t> amountFor(const std::string& id);

    [[nodiscard]]
    std::size_t count();

private:
    class ConnectionPool;

    std::unique_ptr<ConnectionPool> pool_;
};

} // namespace resilientpos::server
