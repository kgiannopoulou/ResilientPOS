#pragma once

#include "resilientpos/core/Transaction.hpp"
#include "resilientpos/core/TransactionStatus.hpp"

#include <optional>
#include <string>
#include <vector>

namespace resilientpos::storage {

// Persistence boundary for transactions.
//
// Business logic depends on this interface, never on a concrete database, so
// services can be unit-tested against a fake and SQLite stays an implementation
// detail behind it.
class TransactionRepository {
public:
    virtual ~TransactionRepository() = default;

    // Stores a new transaction. Throws if a transaction with the same ID exists.
    virtual void save(const core::Transaction& transaction) = 0;

    [[nodiscard]]
    virtual std::optional<core::Transaction>
    findById(const std::string& id) = 0;

    // Transactions waiting to be sent to the server (PENDING_SYNC),
    // oldest first so they sync in the order they were rung up.
    [[nodiscard]]
    virtual std::vector<core::Transaction>
    findPending() = 0;

    // Throws if no transaction with this ID exists.
    virtual void updateStatus(
        const std::string& id,
        core::TransactionStatus status
    ) = 0;
};

} // namespace resilientpos::storage
