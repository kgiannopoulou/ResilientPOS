#pragma once

#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/server/ServerMetrics.hpp"

#include <string>
#include <string_view>

namespace resilientpos::server {

struct HandlerResponse {
    unsigned int httpStatus;  // 200, 400, 409, 422, 500 or 503
    std::string body;         // JSON
};

// Handles the body of POST /transactions, independent of any socket code.
//
//   {"transaction_id": "550e8400-...", "amount_cents": 700}
//
//   new ID                     -> 200 {"status": "ACCEPTED"}
//   same ID, same payload      -> 200 {"status": "ALREADY_PROCESSED"}
//   same ID, different payload -> 409 {"status": "IDEMPOTENCY_CONFLICT", "error": "..."}
//   bad request                -> 400 {"status": "REJECTED", "error": "..."}
//   invalid business data      -> 422 {"status": "REJECTED", "error": "..."}
//   storage error              -> 500 {"status": "ERROR", "error": "..."}
//   database locked too long   -> 503 {"status": "ERROR", "error": "..."}
//
// Both 200 answers mean "the server has it", so a POS may resend the same
// transaction as often as it likes without it being counted twice. A 409 means
// the opposite: the server holds a DIFFERENT sale under that ID, and this one
// was not recorded.
//
// Safe to call concurrently: all shared state is in the store.
[[nodiscard]]
HandlerResponse handleTransactionRequest(
    std::string_view body,
    ProcessedTransactionStore& store,
    ServerMetrics* metrics = nullptr  // optional
);

} // namespace resilientpos::server
