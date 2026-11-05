#pragma once

#include "resilientpos/core/Transaction.hpp"

#include <string>
#include <string_view>

namespace resilientpos::network {

// Outcome of handing one transaction to the server.
enum class SendResultType {
    Accepted,            // server stored it for the first time
    AlreadyProcessed,    // server had it already (e.g. a retry after a lost ACK)
    NetworkError,        // never reached the server or no reply: retry later
    ServerUnavailable,   // server up but shedding load (502/503/504/408/429): retry later
    ServerError,         // server failed on this request (5xx): retry a limited number of times
    Rejected,            // server says this sale is invalid (400/413/422): retrying cannot help
    IdempotencyConflict  // server holds a different sale under this ID: NOT stored
};

// What a failed send means for the sale. The distinction that matters most is
// between the first two: a till must be able to stay offline for days without
// any sale being given up on, so only failures the server actually attributes
// to a request may use up its retry budget.
//
//   class        results                          retried?   counts toward limit?
//   None         Accepted, AlreadyProcessed       no need    -
//   Unreachable  NetworkError, ServerUnavailable  forever    no (says nothing about the sale)
//   Transient    ServerError                      limited    yes, then DEAD_LETTER
//   Permanent    Rejected, IdempotencyConflict    never      straight to DEAD_LETTER
enum class FailureClass {
    None,
    Unreachable,
    Transient,
    Permanent
};

[[nodiscard]]
FailureClass classify(SendResultType type) noexcept;

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

// The record of a failed send, stamped with the current time. The reason is
// truncated: it may be a server's response body and is stored on every sale.
[[nodiscard]]
core::SyncFailure toSyncFailure(const SendResult& result);

} // namespace resilientpos::network
