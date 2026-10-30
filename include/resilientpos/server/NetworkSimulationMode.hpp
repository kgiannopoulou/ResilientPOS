#pragma once

#include <chrono>
#include <optional>
#include <string_view>

namespace resilientpos::server {

// Failures the server can inject, so the POS can be tested against the
// network misbehaving in each way that matters:
//
//   Normal                       process, answer
//   Latency                      process, answer after kSimulatedLatency
//   Reject                       don't process, answer 503
//   Timeout                      don't process, never answer (hold the connection)
//   DropResponseAfterProcessing  process and COMMIT, then close without answering
//
// DropResponseAfterProcessing is the ambiguous failure: the POS sees a
// network error and keeps the sale PENDING_SYNC, yet the server has it.
// Only idempotent resends (ALREADY_PROCESSED) make that safe.
enum class NetworkSimulationMode {
    Normal,
    Latency,
    Reject,
    Timeout,
    DropResponseAfterProcessing
};

inline constexpr std::chrono::milliseconds kSimulatedLatency{500};

// NetworkSimulationMode::DropResponseAfterProcessing -> "drop"
// (the short names the server CLI accepts with --mode).
[[nodiscard]]
std::string_view toString(NetworkSimulationMode mode) noexcept;

// "drop" -> DropResponseAfterProcessing; unknown text -> nullopt.
[[nodiscard]]
std::optional<NetworkSimulationMode> networkSimulationModeFromString(std::string_view text) noexcept;

} // namespace resilientpos::server
