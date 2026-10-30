#include "resilientpos/server/NetworkSimulationMode.hpp"

#include <array>

namespace resilientpos::server {

namespace {

constexpr std::array kAllModes{
    NetworkSimulationMode::Normal,
    NetworkSimulationMode::Latency,
    NetworkSimulationMode::Reject,
    NetworkSimulationMode::Timeout,
    NetworkSimulationMode::DropResponseAfterProcessing,
};

} // namespace

std::string_view toString(NetworkSimulationMode mode) noexcept
{
    switch (mode) {
    case NetworkSimulationMode::Normal:                      return "normal";
    case NetworkSimulationMode::Latency:                     return "latency";
    case NetworkSimulationMode::Reject:                      return "reject";
    case NetworkSimulationMode::Timeout:                     return "timeout";
    case NetworkSimulationMode::DropResponseAfterProcessing: return "drop";
    }
    return "unknown";
}

std::optional<NetworkSimulationMode> networkSimulationModeFromString(std::string_view text) noexcept
{
    for (const auto mode : kAllModes) {
        if (toString(mode) == text) {
            return mode;
        }
    }
    return std::nullopt;
}

} // namespace resilientpos::server
