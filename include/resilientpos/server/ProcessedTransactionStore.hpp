#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct sqlite3;

namespace resilientpos::server {

enum class RecordOutcome {
    Inserted,      // first time the server has seen this transaction ID
    AlreadyExists  // seen before: nothing was written
};

// The central server's record of transactions it has accepted.
//
// Lives in its own database file (data/server.db), never the till's. The
// transaction ID is the primary key, so recording the same sale twice is a
// no-op: that is what makes POS retries safe.
class ProcessedTransactionStore {
public:
    // Opens (or creates) the database at `path`. ":memory:" for tests.
    explicit ProcessedTransactionStore(const std::string& path);

    // Records the transaction if its ID is new. A duplicate ID leaves the
    // stored row untouched, even if the amount differs.
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
