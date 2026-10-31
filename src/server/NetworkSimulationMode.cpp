#include "resilientpos/server/NetworkSimulationMode.hpp"

namespace resilientpos::server {

std::string_view toString(NetworkSimulationMode mode) noexcept
{
    switch (mode) {
    case NetworkSimulationMode::Normal:               return "normal";
    case NetworkSimulationMode::Latency:              return "latency";
    case NetworkSimulationMode::RandomFailure:        return "random";
    case NetworkSimulationMode::Timeout:              return "timeout";
    case NetworkSimulationMode::DropBeforeProcessing: return "drop-before";
    case NetworkSimulationMode::DropAfterProcessing:  return "drop-after";
    case NetworkSimulationMode::Reject:               return "reject";
    }
    return "unknown";
}

std::string_view describe(NetworkSimulationMode mode) noexcept
{
    switch (mode) {
    case NetworkSimulationMode::Normal:               return "Normal";
    case NetworkSimulationMode::Latency:              return "Latency (500 ms, then answer)";
    case NetworkSimulationMode::RandomFailure:        return "Random failure (some requests dropped before/after processing)";
    case NetworkSimulationMode::Timeout:              return "Timeout (never answers, not stored)";
    case NetworkSimulationMode::DropBeforeProcessing: return "Drop before processing (not stored, no answer)";
    case NetworkSimulationMode::DropAfterProcessing:  return "Drop after processing (stored, no answer)";
    case NetworkSimulationMode::Reject:               return "Reject (503, not stored)";
    }
    return "Unknown";
}

std::optional<NetworkSimulationMode> networkSimulationModeFromString(std::string_view text) noexcept
{
    for (const auto mode : kAllNetworkSimulationModes) {
        if (toString(mode) == text) {
            return mode;
        }
    }
    // Name used before drop-before existed.
    if (text == "drop") {
        return NetworkSimulationMode::DropAfterProcessing;
    }
    return std::nullopt;
}

} // namespace resilientpos::server
