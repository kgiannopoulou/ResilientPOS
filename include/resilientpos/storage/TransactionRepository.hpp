#pragma once

#include "resilientpos/core/Transaction.hpp"
#include "resilientpos/core/TransactionStatus.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace resilientpos::storage {

// Persistence boundary for transactions.
//
// Business logic depends on this interface, never on a concrete database, so
// services can be unit-tested against a fake and SQLite stays an implementation
// detail behind it.
//
// Implementations must be safe to call from several threads at once: the till's
// UI thread rings up sales while the sync worker drains the queue.
class TransactionRepository {
public:
    virtual ~TransactionRepository() = default;

    // Stores a new transaction. Throws if a transaction with the same ID exists.
    virtual void save(const core::Transaction& transaction) = 0;

    [[nodiscard]]
    virtual std::optional<core::Transaction>
    findById(const std::string& id) = 0;

    // Every transaction, oldest first.
    [[nodiscard]]
    virtual std::vector<core::Transaction>
    findAll() = 0;

    // Transactions waiting to be sent to the server, oldest first so they sync
    // in the order they were rung up: PENDING_SYNC, plus any SYNCING right now.
    [[nodiscard]]
    virtual std::vector<core::Transaction>
    findPending() = 0;

    // Moves a transaction to `status`. Throws StorageError if the ID is unknown
    // and core::InvalidTransitionError if the state machine forbids the move.
    virtual void updateStatus(
        const std::string& id,
        core::TransactionStatus status
    ) = 0;

    // Atomic compare-and-set: moves the transaction to `to` only if it is
    // currently `expected`, and reports whether it did. This is how the sync
    // worker claims a PENDING_SYNC row without racing anyone else for it.
    // Throws like updateStatus() for an unknown ID or a forbidden transition.
    // Moving to SYNCING also counts one sync attempt.
    virtual bool tryUpdateStatus(
        const std::string& id,
        core::TransactionStatus expected,
        core::TransactionStatus to
    ) = 0;

    // Crash recovery, called once at startup: a CREATED or SYNCING row on disk
    // was in flight when the previous run died, so whether the server got it
    // is unknown. Moves every such row back to PENDING_SYNC so it is resent
    // (the server's idempotency makes that safe). Returns how many it moved.
    virtual std::size_t recoverInterrupted() = 0;
};

} // namespace resilientpos::storage
