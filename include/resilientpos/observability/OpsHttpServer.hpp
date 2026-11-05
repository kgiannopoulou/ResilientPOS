#pragma once

#include "resilientpos/observability/OpsEndpoints.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace resilientpos::observability {

// Serves OpsEndpoints on a background thread of its own, for a process that
// has no HTTP server to add them to: the till. Scrapes are rare and cheap, so
// one thread is plenty, and the cashier's screen never waits on them.
//
// Stops and joins in its destructor, so declare it after everything the
// endpoints read.
class OpsHttpServer {
public:
    // Binds immediately; throws if the address/port is unavailable. Port 0
    // picks a free port (see port()).
    OpsHttpServer(OpsEndpoints endpoints, const std::string& address, std::uint16_t port);
    ~OpsHttpServer();

    OpsHttpServer(const OpsHttpServer&) = delete;
    OpsHttpServer& operator=(const OpsHttpServer&) = delete;

    [[nodiscard]]
    std::uint16_t port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace resilientpos::observability
