#include "resilientpos/server/HttpTransactionServer.hpp"
#include "resilientpos/server/NetworkSimulationMode.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"

#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using resilientpos::server::HttpTransactionServer;
using resilientpos::server::NetworkSimulationMode;
using resilientpos::server::ProcessedTransactionStore;
using resilientpos::server::networkSimulationModeFromString;

namespace {

// The server keeps its own database; it never touches the till's data/pos.db.
constexpr std::string_view kDefaultDatabase = "data/server.db";
constexpr std::uint16_t kDefaultPort = 8080;

// Server log lines and menu output come from two threads.
std::mutex outputMutex;

void print(std::string_view text)
{
    const std::lock_guard lock{outputMutex};
    std::cout << text << std::flush;
}

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

constexpr std::string_view kMenu =
    "Network Simulation\n"
    "  1. Normal\n"
    "  2. 500 ms latency\n"
    "  3. Reject (503, not stored)\n"
    "  4. Timeout (never answers, not stored)\n"
    "  5. Drop response after processing (stored, no answer)\n"
    "  q. Stop server\n";

std::string describe(NetworkSimulationMode mode)
{
    return "[SIM] Mode: " + std::string{toString(mode)} + '\n';
}

// Reads menu choices from stdin. Runs detached: when stdin is closed (server
// started in the background) it just stops reading and the server keeps going.
void runMenu(HttpTransactionServer& server)
{
    std::string line;
    while (std::getline(std::cin, line)) {
        const std::string_view choice = line.empty() ? std::string_view{} : std::string_view{line}.substr(0, 1);
        if (choice == "q") {
            server.stop();
            return;
        }

        constexpr NetworkSimulationMode kByChoice[] = {
            NetworkSimulationMode::Normal,
            NetworkSimulationMode::Latency,
            NetworkSimulationMode::Reject,
            NetworkSimulationMode::Timeout,
            NetworkSimulationMode::DropResponseAfterProcessing,
        };
        if (choice.size() == 1 && choice[0] >= '1' && choice[0] <= '5') {
            const auto mode = kByChoice[choice[0] - '1'];
            server.setSimulationMode(mode);
            print(describe(mode));
        } else {
            print(kMenu);
        }
    }
}

} // namespace

// Usage: resilient-server [--mode normal|latency|reject|timeout|drop] [port] [database]
int main(int argc, char* argv[])
{
    std::cout << "======= ResilientPOS Server ========\n";

    try {
        NetworkSimulationMode mode = NetworkSimulationMode::Normal;
        std::vector<std::string_view> positional;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{argv[i]};
            if (arg == "--mode") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument{"--mode needs a value"};
                }
                const auto parsed = networkSimulationModeFromString(argv[++i]);
                if (!parsed) {
                    throw std::invalid_argument{
                        "Unknown mode '" + std::string{argv[i]}
                        + "' (normal, latency, reject, timeout, drop)"};
                }
                mode = *parsed;
            } else {
                positional.push_back(arg);
            }
        }

        const std::uint16_t port = positional.size() > 0 ? parsePort(positional[0]) : kDefaultPort;
        const std::filesystem::path databasePath =
            positional.size() > 1 ? std::filesystem::path{positional[1]}
                                  : std::filesystem::path{kDefaultDatabase};

        if (databasePath.has_parent_path()) {
            std::filesystem::create_directories(databasePath.parent_path());
        }
        ProcessedTransactionStore store{databasePath.string()};

        HttpTransactionServer server{store, "127.0.0.1", port, [](std::string_view line) {
            print("[SERVER] " + std::string{line} + '\n');
        }};
        server.setSimulationMode(mode);

        print("Database: " + databasePath.string() + " (" + std::to_string(store.count())
              + " processed)\n"
              + "Listening on http://127.0.0.1:" + std::to_string(server.port()) + "/transactions\n"
              + "Press Ctrl+C to stop.\n\n"
              + std::string{kMenu} + describe(mode) + '\n');

        std::thread{runMenu, std::ref(server)}.detach();
        server.run(/*handleSignals=*/true);
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 1;
    }

    print("\nServer stopped.\n");
    return 0;
}
