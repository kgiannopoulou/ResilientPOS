#include "resilientpos/network/SendResult.hpp"

#include <chrono>
#include <cstddef>

namespace resilientpos::network {

namespace {

constexpr std::size_t kMaxReasonLength = 500;

} // namespace

bool SendResult::successful() const noexcept
{
    return type == SendResultType::Accepted
        || type == SendResultType::AlreadyProcessed;
}

FailureClass classify(SendResultType type) noexcept
{
    switch (type) {
    case SendResultType::Accepted:
    case SendResultType::AlreadyProcessed:
        return FailureClass::None;
    case SendResultType::NetworkError:
    case SendResultType::ServerUnavailable:
        return FailureClass::Unreachable;
    case SendResultType::ServerError:
        return FailureClass::Transient;
    case SendResultType::Rejected:
    case SendResultType::IdempotencyConflict:
        return FailureClass::Permanent;
    }
    return FailureClass::Transient;
}

std::string_view toString(SendResultType type) noexcept
{
    switch (type) {
    case SendResultType::Accepted:            return "ACCEPTED";
    case SendResultType::AlreadyProcessed:    return "ALREADY_PROCESSED";
    case SendResultType::NetworkError:        return "NETWORK_ERROR";
    case SendResultType::ServerUnavailable:   return "SERVER_UNAVAILABLE";
    case SendResultType::ServerError:         return "SERVER_ERROR";
    case SendResultType::Rejected:            return "REJECTED";
    case SendResultType::IdempotencyConflict: return "IDEMPOTENCY_CONFLICT";
    }
    return "UNKNOWN";
}

core::SyncFailure toSyncFailure(const SendResult& result)
{
    return core::SyncFailure{
        .code = std::string{toString(result.type)},
        .reason = result.message.substr(0, kMaxReasonLength),
        .at = std::chrono::system_clock::now(),
    };
}

} // namespace resilientpos::network
