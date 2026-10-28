#pragma once

#include <string>
#include <string_view>

namespace resilientpos::network {

// Outcome of handing one transaction to the server.
enum class SendResultType {
    Accepted,          // server stored it for the first time
    AlreadyProcessed,  // server had it already (e.g. a retry after a lost ACK)
    NetworkError,      // never reached the server or no reply: retry later
    ServerError        // server reached but refused/failed: retry later
};

struct SendResult {
    SendResultType type;
    std::string message;

    // True for Accepted and AlreadyProcessed. Either way the server now holds
    // the transaction, so the till can mark it SYNCED and stop resending it.
    // Treating AlreadyProcessed as success is what makes retries idempotent.
    [[nodiscard]]
    bool successful() const noexcept;
};

// SendResultType::AlreadyProcessed -> "ALREADY_PROCESSED"
[[nodiscard]]
std::string_view toString(SendResultType type) noexcept;

} // namespace resilientpos::network
