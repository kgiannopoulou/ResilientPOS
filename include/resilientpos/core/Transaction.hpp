#pragma once

#include "resilientpos/core/Money.hpp"
#include "resilientpos/core/TransactionStatus.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace resilientpos::core {

// Why the last attempt at getting a sale to the server failed.
struct SyncFailure {
    std::string code;    // what kind of failure, e.g. "REJECTED", "NETWORK_ERROR"
    std::string reason;  // the detail, for whoever inspects the dead letter queue
    std::chrono::system_clock::time_point at;
};

// A single sale recorded on the till. Created locally first, synced later.
struct Transaction {
    std::string id;
    Money amount;
    TransactionStatus status;
    std::chrono::system_clock::time_point createdAt;
    unsigned int syncAttempts{0};    // every send by the SyncEngine, whatever the outcome
    unsigned int failedAttempts{0};  // sends that count against the retry limit
    std::optional<SyncFailure> lastFailure;

    // New sale with a fresh UUID, status Created and the current time.
    [[nodiscard]]
    static Transaction create(Money amount);
};

} // namespace resilientpos::core
