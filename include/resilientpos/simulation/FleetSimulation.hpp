#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace resilientpos::simulation {

struct FleetOptions {
    std::size_t terminals{3};     // at least 2
    std::size_t salesPerPhase{5};  // per terminal, in each of the three trading phases
    unsigned int serverThreads{4};
    std::filesystem::path dataDirectory{"data/simulation"};

    // How long the server stays down once the terminals have finished selling.
    std::chrono::milliseconds outage{std::chrono::seconds{2}};
    std::chrono::milliseconds clientTimeout{std::chrono::seconds{1}};
    std::chrono::milliseconds retryBase{std::chrono::milliseconds{100}};
    std::chrono::milliseconds retryCap{std::chrono::seconds{1}};
    // Longest the queues may take to drain before the run counts as failed.
    std::chrono::milliseconds drainTimeout{std::chrono::seconds{60}};

    // Receives each line of the narrative. Optional.
    std::function<void(std::string_view)> print;
};

struct TerminalReport {
    std::string name;
    std::size_t sales{0};  // rung up by its cashier
    std::size_t synced{0};
    std::size_t pending{0};
    std::size_t deadLetters{0};
    std::uint64_t retryAttempts{0};
    std::uint64_t syncFailures{0};
    std::vector<std::chrono::milliseconds> retryDelays;  // the jittered waits it drew
};

struct FleetReport {
    std::vector<TerminalReport> terminals;

    std::size_t salesRungUp{0};
    std::size_t uniqueIds{0};
    std::size_t serverStored{0};
    std::size_t missingOnServer{0};     // sales the server does not have
    std::size_t wrongAmountOnServer{0};  // sales it has with a different amount
    std::int64_t tillTotalCents{0};
    std::int64_t serverTotalCents{0};

    bool isolationHeld{false};  // others kept syncing while one terminal was cut off
    std::uint64_t duplicatesAbsorbed{0};
    std::uint64_t conflictsRefused{0};
    std::uint64_t tamperedAmountStored{0};  // should stay 0: the replay must change nothing
    bool queuesDrained{false};

    std::string serverMetrics;  // final /metrics scrape

    // Every sale on the server exactly once, nothing pending, the one tampered
    // replay refused and parked in the dead letter queue.
    [[nodiscard]]
    bool passed() const noexcept;
};

// Runs a whole shop against one central server, for real: N terminals, each
// with its own SQLite file, sync worker and metrics, talking HTTP over TCP to a
// multi-threaded server on its own SQLite file.
//
//   POS-1 ──┐
//   POS-2 ──┼──► central server ──► server.db
//   POS-3 ──┘
//     │
//   pos-N.db (one each)
//
//   1. Normal trading       every sale goes straight through
//   2. Link failure         POS-1 alone is cut off; the others are unaffected
//   3. Server outage        the server stops; every terminal keeps selling and
//                           backs off on its own jittered schedule
//   4. Lost responses       the server returns but drops replies after
//                           committing: the ambiguous failure
//   5. Recovery             queues drain without duplicates
//   6. Tampered replay      a reused ID with a new amount is refused and
//                           parked in the dead letter queue
//   7. Verification         every sale is compared against the server
//
// Files from an earlier run in `dataDirectory` (only the simulation's own
// databases) are replaced.
[[nodiscard]]
FleetReport runFleetSimulation(const FleetOptions& options);

} // namespace resilientpos::simulation
