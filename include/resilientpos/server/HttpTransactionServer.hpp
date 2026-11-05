#pragma once

#include "resilientpos/observability/Metrics.hpp"
#include "resilientpos/server/NetworkSimulationMode.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace resilientpos::server {

struct ServerOptions {
    // Threads running the io_context; run() uses the caller's plus ioThreads - 1.
    unsigned int ioThreads{4};
    // Where to report. Leave null for a private registry; pass one to keep the
    // numbers across a server restart within a process. Must outlive the server.
    observability::MetricsRegistry* metrics{nullptr};
    // Called on the I/O thread just before a request reaches the store. A seam
    // for tests to prove requests are really in flight at the same time.
    std::function<void()> beforeProcess;
};

// Minimal HTTP/1.1 front end for the central server (Boost.Beast on Asio).
//
//   POST /transactions          -> handleTransactionRequest()
//   GET  /health /ready /metrics -> observability::handleOpsRequest()
//   anything else               -> 404 / 405
//
// The operational endpoints ignore the simulation mode: a monitoring system has
// to be able to see an outage, including a simulated one.
//
// Concurrency model:
//
//                 POS 1   POS 2   POS 3
//                    \      |      /
//                  acceptor (its own strand)
//                           |
//         one strand per connection, on a shared io_context
//                  /        |        \
//              I/O 1      I/O 2     I/O 3     ServerOptions::ioThreads
//                  \        |        /
//              ProcessedTransactionStore      connection pool
//                           |
//              SQLite (PRIMARY KEY + atomic INSERT)
//
// A strand means one connection's handlers never run in parallel, so a Session
// needs no locks; different connections run on different threads at once. The
// store call blocks its I/O thread for a commit (an fsync), which is exactly
// what extra threads are for: others keep reading and answering meanwhile.
// State shared across connections is the store (thread-safe) and the simulation
// mode (atomic). Each connection carries one request and is closed. Diagnostics
// go to the spdlog default logger.
class HttpTransactionServer {
public:
    // Binds immediately; throws if the address/port is unavailable.
    // Port 0 picks a free port (see port()), which tests rely on.
    // Throws std::invalid_argument if options.ioThreads is 0.
    HttpTransactionServer(
        ProcessedTransactionStore& store,
        const std::string& address,
        std::uint16_t port,
        ServerOptions options = {}
    );
    ~HttpTransactionServer();

    HttpTransactionServer(const HttpTransactionServer&) = delete;
    HttpTransactionServer& operator=(const HttpTransactionServer&) = delete;

    // The port actually bound.
    [[nodiscard]]
    std::uint16_t port() const noexcept;

    // Failure injection (see NetworkSimulationMode). Thread-safe; applies to
    // requests that arrive after the call. Starts as Normal.
    void setSimulationMode(NetworkSimulationMode mode) noexcept;

    [[nodiscard]]
    NetworkSimulationMode simulationMode() const noexcept;

    // What GET /metrics answers.
    [[nodiscard]]
    std::string renderMetrics() const;

    // Serves requests until stop() is called (or SIGINT/SIGTERM if
    // handleSignals was requested). Blocks the calling thread, which becomes
    // one of the I/O threads; the others are joined before it returns.
    void run(bool handleSignals = false);

    // Thread-safe; makes run() return.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace resilientpos::server
