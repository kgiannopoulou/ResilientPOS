#include "resilientpos/server/HttpTransactionServer.hpp"
#include "resilientpos/server/NetworkSimulationMode.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/util/Logging.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <charconv>
#include <cstddef>
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
#include <utility>
#include <vector>

using resilientpos::server::HttpTransactionServer;
using resilientpos::server::kAllNetworkSimulationModes;
using resilientpos::server::NetworkSimulationMode;
using resilientpos::server::ProcessedTransactionStore;
using resilientpos::server::networkSimulationModeFromString;

namespace {

// The server keeps its own database; it never touches the till's data/pos.db.
constexpr std::string_view kDefaultDatabase = "data/server.db";
constexpr std::uint16_t kDefaultPort = 8080;

// Menu output comes from the stdin thread while the server thread logs.
std::mutex outputMutex;

void print(std::string_view text)
{
    const std::lock_guard lock{outputMutex};
    std::cout << text << std::flush;
}

std::uint16_t parsePort(std::string_view text)
{
    std::uint16_t port{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, port);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument{"Invalid port: " + std::string{text}};
    }
    return port;
}

std::string menu()
{
    std::string text = "Network Simulation\n";
    for (std::size_t i = 0; i < kAllNetworkSimulationModes.size(); ++i) {
        text += "  " + std::to_string(i + 1) + ". "
              + std::string{describe(kAllNetworkSimulationModes[i])} + '\n';
    }
    text += "  q. Stop server\n";
    return text;
}

std::string describeMode(NetworkSimulationMode mode)
{
    return "[SIM] Mode: " + std::string{toString(mode)} + " - " + std::string{describe(mode)} + '\n';
}

// Reads menu choices from stdin. Runs detached: when stdin is closed (server
// started in the background) it just stops reading and the server keeps going.
void runMenu(HttpTransactionServer& server)
{
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line[0] == 'q') {
            server.stop();
            return;
        }

        const int index = line.empty() ? -1 : line[0] - '1';
        if (index >= 0 && index < static_cast<int>(kAllNetworkSimulationModes.size())) {
            const auto mode = kAllNetworkSimulationModes[static_cast<std::size_t>(index)];
            server.setSimulationMode(mode);
            print(describeMode(mode));
        } else {
            print(menu());
        }
    }
}

void setUpLogging()
{
    resilientpos::util::installDefaultLogger(spdlog::stdout_color_mt("server"));
}

} // namespace

// Usage: resilient-server [--mode normal|latency|random|timeout|drop-before|drop-after|reject]
//                         [port] [database]
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
                        + "' (normal, latency, random, timeout, drop-before, drop-after, reject)"};
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
        setUpLogging();
        ProcessedTransactionStore store{databasePath.string()};

        HttpTransactionServer server{store, "127.0.0.1", port};

        print("Database: " + databasePath.string() + " (" + std::to_string(store.count())
              + " processed)\n"
              + "Listening on http://127.0.0.1:" + std::to_string(server.port()) + "/transactions\n"
              + "Press Ctrl+C to stop.\n\n"
              + menu() + describeMode(mode) + '\n');
        server.setSimulationMode(mode);

        std::thread{runMenu, std::ref(server)}.detach();
        server.run(/*handleSignals=*/true);
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 1;
    }

    print("\nServer stopped.\n");
    return 0;
}
