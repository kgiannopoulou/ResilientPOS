#pragma once

#include "resilientpos/network/SendResult.hpp"
#include "resilientpos/network/ServerClient.hpp"

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string_view>

namespace resilientpos::network {

// What the till currently believes about its link to the server.
enum class ConnectivityState {
    Unknown,      // nothing sent yet
    Online,       // last send was Accepted / AlreadyProcessed
    ServerError,  // last send reached the server but it failed the request
    Offline       // last send never got a reply
};

// ConnectivityState::Online -> "ONLINE"
[[nodiscard]]
std::string_view toString(ConnectivityState state) noexcept;

struct ConnectivitySnapshot {
    ConnectivityState state{ConnectivityState::Unknown};
    std::optional<SendResult> lastResult;
    std::optional<std::chrono::system_clock::time_point> lastContactAt;
    std::size_t sends{0};
    std::size_t failures{0};
};

// Decorator that remembers the outcome of every send passing through it:
//
//   TransactionService ─┐
//                       ├─► MonitoredServerClient ─► HttpServerClient
//   SyncEngine ─────────┘          │
//                                  └─► snapshot() for the "Network: ONLINE" line
//
// The till learns about connectivity from real traffic instead of a separate
// health-check loop. Thread-safe (the UI and the sync worker both send).
class MonitoredServerClient final : public ServerClient {
public:
    explicit MonitoredServerClient(ServerClient& inner);

    SendResult send(const core::Transaction& transaction) override;

    [[nodiscard]]
    ConnectivitySnapshot snapshot() const;

private:
    ServerClient& inner_;
    mutable std::mutex mutex_;
    ConnectivitySnapshot snapshot_;
};

} // namespace resilientpos::network
