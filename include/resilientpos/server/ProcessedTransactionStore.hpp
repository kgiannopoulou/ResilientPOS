#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct sqlite3;

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
class ProcessedTransactionStore {
public:
    // Opens (or creates) the database at `path`. ":memory:" for tests.
    explicit ProcessedTransactionStore(const std::string& path);

    // Records the transaction if its ID is new. A known ID leaves the stored
    // row untouched; the outcome says whether the payload matched.
    RecordOutcome record(const std::string& id, std::int64_t amountCents);

    // Stored amount for `id`, or nullopt if the server never accepted it.
    [[nodiscard]]
    std::optional<std::int64_t> amountFor(const std::string& id);

    [[nodiscard]]
    std::size_t count();

private:
    struct Closer {
        void operator()(sqlite3* db) const noexcept;
    };

    std::unique_ptr<sqlite3, Closer> db_;
};

} // namespace resilientpos::server
