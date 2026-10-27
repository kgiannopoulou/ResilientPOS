#pragma once

#include "resilientpos/core/Money.hpp"
#include "resilientpos/core/TransactionStatus.hpp"

#include <chrono>
#include <string>

namespace resilientpos::core {

// A single sale recorded on the till. Created locally first, synced later.
struct Transaction {
    std::string id;
    Money amount;
    TransactionStatus status;
    std::chrono::system_clock::time_point createdAt;
    unsigned int syncAttempts{0};

    // New sale with a fresh UUID, status Created and the current time.
    [[nodiscard]]
    static Transaction create(Money amount);
};

} // namespace resilientpos::core
