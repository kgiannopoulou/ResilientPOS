#include "resilientpos/core/TransactionStatus.hpp"

#include <array>
#include <ostream>
#include <string>

namespace resilientpos::core {

bool canTransition(TransactionStatus from, TransactionStatus to) noexcept
{
    using enum TransactionStatus;
    switch (from) {
    case Created:     return to == PendingSync || to == Synced;
    case PendingSync: return to == Syncing;
    case Syncing:     return to == Synced || to == PendingSync;
    case Synced:      return false;
    }
    return false;
}

InvalidTransitionError::InvalidTransitionError(TransactionStatus from, TransactionStatus to)
    : std::logic_error{"Invalid transaction status transition " + std::string{toString(from)}
                       + " -> " + std::string{toString(to)}}
    , from_{from}
    , to_{to}
{
}

std::string_view toString(TransactionStatus status) noexcept
{
    switch (status) {
    case TransactionStatus::Created:     return "CREATED";
    case TransactionStatus::PendingSync: return "PENDING_SYNC";
    case TransactionStatus::Syncing:     return "SYNCING";
    case TransactionStatus::Synced:      return "SYNCED";
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
