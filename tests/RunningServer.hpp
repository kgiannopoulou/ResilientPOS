#pragma once

#include "resilientpos/server/HttpTransactionServer.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"

#include <cstdint>
#include <thread>

namespace resilientpos::test {

// Runs a server on a free local port for the lifetime of the object.
class RunningServer {
public:
    explicit RunningServer(server::ProcessedTransactionStore& store)
        : server_{store, "127.0.0.1", 0}
        , thread_{[this] { server_.run(); }}
    {
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    ~RunningServer()
    {
        server_.stop();
        thread_.join();
    }

    [[nodiscard]]
    std::uint16_t port() const { return server_.port(); }

    void setMode(server::NetworkSimulationMode mode) { server_.setSimulationMode(mode); }

private:
    server::HttpTransactionServer server_;
    std::thread thread_;
};

// A port that nothing is listening on: bind one, then shut the server down.
inline std::uint16_t closedPort()
{
    server::ProcessedTransactionStore store{":memory:"};
    RunningServer server{store};
    return server.port();
}

} // namespace resilientpos::test
