#pragma once

#include "resilientpos/server/ProcessedTransactionStore.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace resilientpos::server {

// Minimal HTTP/1.1 front end for the central server (Boost.Beast on Asio).
//
//   POST /transactions   -> handleTransactionRequest()
//   anything else        -> 404 / 405
//
// Runs on one thread: requests are handled one at a time, so the store never
// sees concurrent writes. Each connection carries one request and is closed.
class HttpTransactionServer {
public:
    using Logger = std::function<void(std::string_view)>;

    // Binds immediately; throws if the address/port is unavailable.
    // Port 0 picks a free port (see port()), which tests rely on.
    HttpTransactionServer(
        ProcessedTransactionStore& store,
        const std::string& address,
        std::uint16_t port,
        Logger logger = {}
    );
    ~HttpTransactionServer();

    HttpTransactionServer(const HttpTransactionServer&) = delete;
    HttpTransactionServer& operator=(const HttpTransactionServer&) = delete;

    // The port actually bound.
    [[nodiscard]]
    std::uint16_t port() const noexcept;

    // Serves requests until stop() is called (or SIGINT/SIGTERM if
    // handleSignals was requested). Blocks the calling thread.
    void run(bool handleSignals = false);

    // Thread-safe; makes run() return.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace resilientpos::server
