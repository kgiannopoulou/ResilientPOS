// The whole system at once: several terminals against one central server,
// through a link failure, a real server outage and lost responses.
//
// A run takes seconds and ctest gives every TEST_CASE its own process, so one
// case asserts everything about a run instead of repeating it per assertion.

#include "resilientpos/simulation/FleetSimulation.hpp"
#include "resilientpos/util/UuidGenerator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace std::chrono_literals;
using resilientpos::simulation::FleetOptions;
using resilientpos::simulation::FleetReport;
using resilientpos::simulation::runFleetSimulation;

namespace {

// A scratch directory for one run's databases.
class TempDirectory {
public:
    TempDirectory()
        : path_{std::filesystem::temp_directory_path()
                / ("resilientpos-fleet-" + resilientpos::util::UuidGenerator::generate())}
    {
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]]
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

FleetOptions fastOptions(const std::filesystem::path& directory)
{
    FleetOptions options;
    options.dataDirectory = directory;
    options.salesPerPhase = 3;
    options.outage = 300ms;
    options.retryBase = 20ms;
    options.retryCap = 200ms;
    return options;
}

} // namespace

TEST_CASE("Fleet: every sale reaches the server exactly once, through every failure", "[fleet][integration]")
{
    TempDirectory directory;
    auto options = fastOptions(directory.path());
    std::vector<std::string> narrative;
    options.print = [&](std::string_view line) { narrative.emplace_back(line); };

    const auto report = runFleetSimulation(options);
    INFO("server metrics:\n" << report.serverMetrics);

    // Nothing lost, nothing doubled.
    REQUIRE(report.queuesDrained);
    REQUIRE(report.salesRungUp == 27);  // 3 terminals x 3 phases x 3 sales
    REQUIRE(report.uniqueIds == 27);    // IDs generated offline never collide
    REQUIRE(report.serverStored == 27);
    REQUIRE(report.missingOnServer == 0);
    REQUIRE(report.wrongAmountOnServer == 0);
    REQUIRE(report.tillTotalCents == report.serverTotalCents);

    // Fault isolation: POS-1 losing its link did not touch the others.
    REQUIRE(report.isolationHeld);

    // The replies lost after commit came back as harmless duplicates.
    REQUIRE(report.duplicatesAbsorbed > 0);

    // The tampered replay changed nothing and is parked on the terminal that sent it.
    REQUIRE(report.conflictsRefused == 1);
    REQUIRE(report.tamperedAmountStored == 0);
    REQUIRE(report.terminals.back().deadLetters == 1);
    for (std::size_t i = 0; i + 1 < report.terminals.size(); ++i) {
        REQUIRE(report.terminals[i].deadLetters == 0);
    }

    // Each terminal retried on a schedule of its own: sharing one is exactly
    // the failure jitter exists to prevent.
    for (const auto& terminal : report.terminals) {
        REQUIRE_FALSE(terminal.retryDelays.empty());
        REQUIRE(terminal.retryAttempts > 0);
    }
    REQUIRE(report.terminals[0].retryDelays != report.terminals[1].retryDelays);
    REQUIRE(report.terminals[1].retryDelays != report.terminals[2].retryDelays);

    REQUIRE(report.passed());
    REQUIRE(narrative.back() == "PASS: every sale is on the server exactly once.");
}

TEST_CASE("Fleet: a larger fleet holds up too", "[fleet][integration][stress]")
{
    TempDirectory directory;
    auto options = fastOptions(directory.path());
    options.terminals = 8;
    options.salesPerPhase = 2;
    options.serverThreads = 8;

    const auto report = runFleetSimulation(options);

    REQUIRE(report.salesRungUp == 48);
    REQUIRE(report.passed());
}

TEST_CASE("Fleet: a single terminal is not a fleet", "[fleet]")
{
    TempDirectory directory;
    auto options = fastOptions(directory.path());
    options.terminals = 1;

    REQUIRE_THROWS_AS((void)runFleetSimulation(options), std::invalid_argument);
}

TEST_CASE("Fleet: a report that does not add up does not pass", "[fleet]")
{
    FleetReport report;
    report.queuesDrained = true;
    report.isolationHeld = true;
    report.salesRungUp = report.uniqueIds = report.serverStored = 10;
    report.tillTotalCents = report.serverTotalCents = 1000;
    report.duplicatesAbsorbed = 1;
    report.conflictsRefused = 1;
    report.terminals.push_back({.name = "POS-1", .deadLetters = 1});
    REQUIRE(report.passed());

    SECTION("a sale is missing")
    {
        report.serverStored = 9;
        REQUIRE_FALSE(report.passed());
    }
    SECTION("a sale is doubled")
    {
        report.serverStored = 11;
        REQUIRE_FALSE(report.passed());
    }
    SECTION("the money does not match")
    {
        report.serverTotalCents = 999;
        REQUIRE_FALSE(report.passed());
    }
    SECTION("a queue did not drain")
    {
        report.terminals.front().pending = 1;
        REQUIRE_FALSE(report.passed());
    }
    SECTION("the tampered replay changed something")
    {
        report.tamperedAmountStored = 1;
        REQUIRE_FALSE(report.passed());
    }
}
