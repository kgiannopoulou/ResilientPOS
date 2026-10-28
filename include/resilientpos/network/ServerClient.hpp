#pragma once

#include "resilientpos/core/Transaction.hpp"
#include "resilientpos/network/SendResult.hpp"

namespace resilientpos::network {

// Transport boundary to the central server.
//
//   TransactionService
//          |
//          v
//     ServerClient        <- business logic only sees this
//          ^
//          |
//   HttpServerClient / FakeServerClient / ...
//
// Implementations must not throw for expected failures (timeouts, refused
// connections, 5xx); they report them as NetworkError / ServerError instead.
class ServerClient {
public:
    virtual ~ServerClient() = default;

    virtual SendResult
    send(const core::Transaction& transaction) = 0;
};

} // namespace resilientpos::network
