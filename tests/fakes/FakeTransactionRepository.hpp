#pragma once

#include "resilientpos/storage/StorageError.hpp"
#include "resilientpos/storage/TransactionRepository.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace resilientpos::test {

// In-memory TransactionRepository with the same contract as the SQLite one
// (ordering, state-machine validation, sync-attempt counting), minus the disk.
// Lets service and sync tests run without touching SQLite, and lets a test
// make storage fail on demand.
class FakeTransactionRepository final : public storage::TransactionRepository {
public:
    void save(const core::Transaction& transaction) override
    {
        const std::scoped_lock lock{mutex_};
        if (failSaves) {
            throw storage::StorageError{"disk full (fake)"};
        }
        if (findLocked(transaction.id) != nullptr) {
            throw storage::StorageError{"duplicate id " + transaction.id};
        }
        rows_.push_back(transaction);
    }

    std::optional<core::Transaction> findById(const std::string& id) override
    {
        const std::scoped_lock lock{mutex_};
        const auto* row = findLocked(id);
        return row != nullptr ? std::optional{*row} : std::nullopt;
    }

    std::vector<core::Transaction> findAll() override
    {
        const std::scoped_lock lock{mutex_};
        return sortedLocked();
    }

    std::vector<core::Transaction> findPending() override
    {
        ++pendingQueries;
        const std::scoped_lock lock{mutex_};
        auto rows = sortedLocked();
        std::erase_if(rows, [](const core::Transaction& tx) {
            return tx.status != core::TransactionStatus::PendingSync
                && tx.status != core::TransactionStatus::Syncing;
        });
        return rows;
    }

    void updateStatus(const std::string& id, core::TransactionStatus status) override
    {
        const std::scoped_lock lock{mutex_};
        auto& row = existingLocked(id);
        if (!core::canTransition(row.status, status)) {
            throw core::InvalidTransitionError{row.status, status};
        }
        writeLocked(row, status);
    }

    bool tryUpdateStatus(const std::string& id, core::TransactionStatus expected,
                         core::TransactionStatus to) override
    {
        if (!core::canTransition(expected, to)) {
            throw core::InvalidTransitionError{expected, to};
        }
        const std::scoped_lock lock{mutex_};
        auto& row = existingLocked(id);
        if (row.status != expected) {
            return false;
        }
        writeLocked(row, to);
        return true;
    }

    std::size_t recoverInterrupted() override
    {
        const std::scoped_lock lock{mutex_};
        std::size_t moved = 0;
        for (auto& row : rows_) {
            if (row.status == core::TransactionStatus::Created
                || row.status == core::TransactionStatus::Syncing) {
                row.status = core::TransactionStatus::PendingSync;
                ++moved;
            }
        }
        return moved;
    }

    bool failSaves{false};
    // How often findPending() ran: proves an idle sync worker is not polling.
    std::atomic<int> pendingQueries{0};

private:
    core::Transaction* findLocked(const std::string& id)
    {
        const auto it = std::ranges::find(rows_, id, &core::Transaction::id);
        return it != rows_.end() ? &*it : nullptr;
    }

    core::Transaction& existingLocked(const std::string& id)
    {
        auto* row = findLocked(id);
        if (row == nullptr) {
            throw storage::StorageError{"No transaction with id '" + id + "'"};
        }
        return *row;
    }

    static void writeLocked(core::Transaction& row, core::TransactionStatus to)
    {
        if (to == core::TransactionStatus::Syncing) {
            ++row.syncAttempts;
        }
        row.status = to;
    }

    std::vector<core::Transaction> sortedLocked() const
    {
        auto rows = rows_;
        std::ranges::stable_sort(rows, {}, &core::Transaction::createdAt);
        return rows;
    }

    std::mutex mutex_;
    std::vector<core::Transaction> rows_;
};

} // namespace resilientpos::test
