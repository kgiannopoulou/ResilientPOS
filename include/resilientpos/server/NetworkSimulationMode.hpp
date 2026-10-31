#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string_view>

namespace resilientpos::server {

// Failures the server can inject, so the POS can be tested against the
// network misbehaving in each way that matters:
//
//   mode                  stored?  answered?
//   Normal                yes      yes
//   Latency               yes      yes, after kSimulatedLatency
//   RandomFailure         maybe    maybe (per request: normal, drop-before or drop-after)
//   Timeout               no       never (connection held open)
//   DropBeforeProcessing  no       no (request read, connection closed)
//   DropAfterProcessing   yes      no (COMMITTED, then connection closed)
//   Reject                no       yes, 503
//
// The two drops look identical to the POS (a NetworkError) but are opposite
// situations on the server:
//
//   DropBeforeProcessing           DropAfterProcessing
//   POS ───X──► server             POS ──────► server ── COMMIT
//                                  POS ◄──X─── response
//   server doesn't have it:        server already has it:
//   the retry is REQUIRED          the retry must be IDEMPOTENT
//
// The POS cannot tell which happened, so it always retries the same
// transaction ID and relies on the server answering ALREADY_PROCESSED.
enum class NetworkSimulationMode {
    Normal,
    Latency,
    RandomFailure,
    Timeout,
    DropBeforeProcessing,
    DropAfterProcessing,
    Reject
};

inline constexpr std::array kAllNetworkSimulationModes{
    NetworkSimulationMode::Normal,
    NetworkSimulationMode::Latency,
    NetworkSimulationMode::RandomFailure,
    NetworkSimulationMode::Timeout,
    NetworkSimulationMode::DropBeforeProcessing,
    NetworkSimulationMode::DropAfterProcessing,
    NetworkSimulationMode::Reject,
};

inline constexpr std::chrono::milliseconds kSimulatedLatency{500};

// NetworkSimulationMode::DropAfterProcessing -> "drop-after"
// (the short names the server CLI accepts with --mode).
[[nodiscard]]
std::string_view toString(NetworkSimulationMode mode) noexcept;

// Human-readable menu text, e.g. "Drop after processing (stored, no answer)".
[[nodiscard]]
std::string_view describe(NetworkSimulationMode mode) noexcept;

// "drop-after" -> DropAfterProcessing; unknown text -> nullopt.
[[nodiscard]]
std::optional<NetworkSimulationMode> networkSimulationModeFromString(std::string_view text) noexcept;

} // namespace resilientpos::server
