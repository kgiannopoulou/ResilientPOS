#include "resilientpos/server/HttpTransactionServer.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

using resilientpos::server::HttpTransactionServer;
using resilientpos::server::ProcessedTransactionStore;

namespace {

// The server keeps its own database; it never touches the till's data/pos.db.
constexpr std::string_view kDefaultDatabase = "data/server.db";
constexpr std::uint16_t kDefaultPort = 8080;

std::uint16_t parsePort(std::string_view text)
{
    std::uint16_t port{};
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, port);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument{"Invalid port: " + std::string{text}};
    }
    return port;
}

} // namespace

// Usage: resilient-server [port] [database]
int main(int argc, char* argv[])
{
    std::cout << "======= ResilientPOS Server ========\n";

    try {
        const std::uint16_t port = argc > 1 ? parsePort(argv[1]) : kDefaultPort;
        const std::filesystem::path databasePath =
            argc > 2 ? std::filesystem::path{argv[2]} : std::filesystem::path{kDefaultDatabase};

        if (databasePath.has_parent_path()) {
            std::filesystem::create_directories(databasePath.parent_path());
        }
        ProcessedTransactionStore store{databasePath.string()};

        HttpTransactionServer server{store, "127.0.0.1", port, [](std::string_view line) {
            std::cout << "[SERVER] " << line << std::endl;
        }};

        std::cout << "Database: " << databasePath.string()
                  << " (" << store.count() << " processed)\n"
                  << "Listening on http://127.0.0.1:" << server.port() << "/transactions\n"
                  << "Press Ctrl+C to stop.\n\n"
                  << std::flush;

        server.run(/*handleSignals=*/true);
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 1;
    }

    std::cout << "\nServer stopped.\n";
    return 0;
}
