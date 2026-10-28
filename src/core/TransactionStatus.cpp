#include "resilientpos/core/TransactionStatus.hpp"

#include <array>
#include <ostream>

namespace resilientpos::core {

std::string_view toString(TransactionStatus status) noexcept
{
    switch (status) {
    case TransactionStatus::Created:     return "CREATED";
    case TransactionStatus::PendingSync: return "PENDING_SYNC";
    case TransactionStatus::Syncing:     return "SYNCING";
    case TransactionStatus::Synced:      return "SYNCED";
    case TransactionStatus::Failed:      return "FAILED";
    }
    // Only reachable if someone casts an out-of-range integer to the enum.
    return "UNKNOWN";
}

std::optional<TransactionStatus> transactionStatusFromString(std::string_view text) noexcept
{
    constexpr std::array all{
        TransactionStatus::Created,
        TransactionStatus::PendingSync,
        TransactionStatus::Syncing,
        TransactionStatus::Synced,
        TransactionStatus::Failed,
    };
    for (const auto status : all) {
        if (toString(status) == text) {
            return status;
        }
    }
    return std::nullopt;
}

std::ostream& operator<<(std::ostream& os, TransactionStatus status)
{
    return os << toString(status);
}

} // namespace resilientpos::core
