// A whole shop in one process: several terminals, one central server and a
// scripted outage. All the logic is in simulation::runFleetSimulation(); this
// file parses arguments and prints. Exits non-zero if verification fails.

#include "resilientpos/simulation/FleetSimulation.hpp"
#include "resilientpos/util/Logging.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <charconv>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

using resilientpos::simulation::FleetOptions;

namespace {

std::size_t parseCount(std::string_view flag, std::string_view text, std::size_t min, std::size_t max)
{
    std::size_t value{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || value < min || value > max) {
        throw std::invalid_argument{std::string{flag} + " must be between " + std::to_string(min)
                                    + " and " + std::to_string(max)};
    }
    return value;
}

FleetOptions parseArguments(int argc, char* argv[])
{
    FleetOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag{argv[i]};
        if (i + 1 >= argc) {
            throw std::invalid_argument{std::string{flag} + " needs a value"};
        }
        const std::string_view value{argv[++i]};

        if (flag == "--terminals") {
            options.terminals = parseCount(flag, value, 2, 100);
        } else if (flag == "--sales") {
            options.salesPerPhase = parseCount(flag, value, 1, 1000);
        } else if (flag == "--threads") {
            options.serverThreads = static_cast<unsigned int>(parseCount(flag, value, 1, 256));
        } else if (flag == "--dir") {
            options.dataDirectory = std::filesystem::path{value};
        } else {
            throw std::invalid_argument{"Unknown option " + std::string{flag}};
        }
    }
    return options;
}

} // namespace

// Usage: resilient-sim [--terminals N] [--sales N] [--threads N] [--dir path]
int main(int argc, char* argv[])
{
    try {
        auto options = parseArguments(argc, argv);
        options.print = [](std::string_view line) { std::cout << line << '\n' << std::flush; };

        // The narrative goes to the terminal, diagnostics to a file beside the
        // databases, the same split the till makes.
        std::filesystem::create_directories(options.dataDirectory);
        resilientpos::util::installDefaultLogger(spdlog::basic_logger_mt(
            "simulation", (options.dataDirectory / "simulation.log").string(), /*truncate=*/true));

        const auto report = resilientpos::simulation::runFleetSimulation(options);
        return report.passed() ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 2;
    }
}
