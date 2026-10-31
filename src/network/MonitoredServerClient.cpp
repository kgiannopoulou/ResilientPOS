#include "resilientpos/network/MonitoredServerClient.hpp"

namespace resilientpos::network {

std::string_view toString(ConnectivityState state) noexcept
{
    switch (state) {
    case ConnectivityState::Unknown:     return "UNKNOWN";
    case ConnectivityState::Online:      return "ONLINE";
    case ConnectivityState::ServerError: return "SERVER ERROR";
    case ConnectivityState::Offline:     return "OFFLINE";
    }
    return "UNKNOWN";
}

MonitoredServerClient::MonitoredServerClient(ServerClient& inner)
    : inner_{inner}
{
}

SendResult MonitoredServerClient::send(const core::Transaction& transaction)
{
    // Not under the lock: a send can take seconds and both threads may send.
    SendResult result = inner_.send(transaction);

    const std::scoped_lock lock{mutex_};
    ++snapshot_.sends;
    snapshot_.lastResult = result;
    switch (result.type) {
    case SendResultType::Accepted:
    case SendResultType::AlreadyProcessed:
        snapshot_.state = ConnectivityState::Online;
        snapshot_.lastContactAt = std::chrono::system_clock::now();
        break;
    case SendResultType::ServerError:
        snapshot_.state = ConnectivityState::ServerError;
        snapshot_.lastContactAt = std::chrono::system_clock::now();
        ++snapshot_.failures;
        break;
    case SendResultType::NetworkError:
        snapshot_.state = ConnectivityState::Offline;
        ++snapshot_.failures;
        break;
    }
    return result;
}

ConnectivitySnapshot MonitoredServerClient::snapshot() const
{
    const std::scoped_lock lock{mutex_};
    return snapshot_;
}

} // namespace resilientpos::network
