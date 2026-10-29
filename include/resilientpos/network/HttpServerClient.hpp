#pragma once

#include "resilientpos/network/ServerClient.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace resilientpos::network {

// ServerClient that POSTs each transaction as JSON to the central server:
//
//   POST /transactions
//   {"transaction_id": "550e8400-...", "amount_cents": 700}
//
// One request per call, no retries (the sync layer decides when to resend).
// Never throws for transport problems; they come back as:
//
//   refused / unreachable / timed out / dropped  -> NetworkError
//   HTTP error or unexpected reply              -> ServerError
//   {"status": "ACCEPTED"}                       -> Accepted
//   {"status": "ALREADY_PROCESSED"}              -> AlreadyProcessed
class HttpServerClient final : public ServerClient {
public:
    HttpServerClient(
        std::string host,
        std::uint16_t port,
        std::chrono::milliseconds timeout = std::chrono::seconds{3}
    );

    SendResult send(const core::Transaction& transaction) override;

private:
    std::string host_;
    std::uint16_t port_;
    std::chrono::milliseconds timeout_;
};

} // namespace resilientpos::network
