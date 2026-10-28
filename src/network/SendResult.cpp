#include "resilientpos/network/SendResult.hpp"

namespace resilientpos::network {

bool SendResult::successful() const noexcept
{
    return type == SendResultType::Accepted
        || type == SendResultType::AlreadyProcessed;
}

std::string_view toString(SendResultType type) noexcept
{
    switch (type) {
    case SendResultType::Accepted:         return "ACCEPTED";
    case SendResultType::AlreadyProcessed: return "ALREADY_PROCESSED";
    case SendResultType::NetworkError:     return "NETWORK_ERROR";
    case SendResultType::ServerError:      return "SERVER_ERROR";
    }
    return "UNKNOWN";
}

} // namespace resilientpos::network
