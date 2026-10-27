#pragma once

#include <iosfwd>
#include <string_view>

namespace resilientpos::core {

// Lifecycle of a sale on the till:
//
//   Created -> PendingSync -> Syncing -> Synced
//                   ^            |
//                   |            v
//                   +-------- Failed   (retried later)
enum class TransactionStatus {
    Created,
    PendingSync,
    Syncing,
    Synced,
    Failed
};

// TransactionStatus::PendingSync -> "PENDING_SYNC"
[[nodiscard]]
std::string_view toString(TransactionStatus status) noexcept;

std::ostream& operator<<(std::ostream& os, TransactionStatus status);

} // namespace resilientpos::core
