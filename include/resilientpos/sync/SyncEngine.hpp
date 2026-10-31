#pragma once

#include "resilientpos/network/SendResult.hpp"
#include "resilientpos/network/ServerClient.hpp"
#include "resilientpos/storage/TransactionRepository.hpp"
#include "resilientpos/sync/RetryPolicy.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace resilientpos::sync {

// What happened to one transaction during a pass.
struct SyncOutcome {
    std::string transactionId;
    network::SendResultType result;
    std::string message;
};

// What one syncPendingTransactions() pass achieved.
struct SyncReport {
    std::size_t attempted{0};
    std::size_t synced{0};            // includes alreadyProcessed
    std::size_t alreadyProcessed{0};  // server had it: a resend after a lost reply
    std::size_t failed{0};            // left PENDING_SYNC for the next pass
    std::size_t stillPending{0};      // PENDING_SYNC/SYNCING rows after the pass
    std::vector<SyncOutcome> outcomes;  // one per attempted transaction, in order
};

// Snapshot of the background worker, for the till's "Network status" screen.
struct WorkerStatus {
    bool running{false};
    unsigned int consecutiveFailures{0};
    // Set while the worker is backing off after a failed pass.
    std::optional<std::chrono::steady_clock::time_point> nextRetryAt;
};

// Drains the offline queue:
//
//            SyncEngine
//             /      \
//            v        v
//   TransactionRepository   ServerClient
//
//   repository.findPending()          (oldest first)
//          |
//   for each transaction
//          |
//   claim: PENDING_SYNC -> SYNCING    (compare-and-set; skip if someone else has it)
//          |
//   serverClient.send()
//        /        \
//   success      failure
//      |            |
//   SYNCED     PENDING_SYNC
//
// "Success" includes ALREADY_PROCESSED: the server stored the sale on an
// earlier attempt whose reply was lost, so resending is how it becomes SYNCED.
//
// A NetworkError ends the pass early: if the server is unreachable for one
// sale it is unreachable for all of them, and each attempt can take seconds.
// A ServerError is about that one request, so the pass carries on.
//
// Two ways to drive it:
//
//   syncPendingTransactions()   one pass, on the calling thread ("Force sync")
//   start()                     background worker on a std::jthread:
//
//        wake ──► sync pass ──► nothing failed? ──► sleep until notified
//         ▲                          │
//         │                       failed
//         │                          ▼
//         └──── timed wait: RetryPolicy::nextDelay(consecutiveFailures)
//
// The worker never polls. With an empty queue it sleeps on a condition
// variable until notifyPendingTransaction() says there is work; after a
// failure it waits out an exponential backoff instead of hammering a server
// that is down. Passes are serialized, so a forced sync and the worker never
// send the same sale at the same time.
//
// Construction performs crash recovery (repository.recoverInterrupted()), so
// create the engine once at startup, before any sale is rung up.
//
// Shutdown is RAII: the destructor (or stop()) requests stop, which wakes the
// worker out of any wait; the in-flight send, if any, finishes; the jthread
// joins. Only then may the repository and client it uses be destroyed, so
// declare the engine after them.
class SyncEngine {
public:
    SyncEngine(
        storage::TransactionRepository& repository,
        network::ServerClient& serverClient,
        RetryPolicy retryPolicy = RetryPolicy{}
    );
    ~SyncEngine();

    SyncEngine(const SyncEngine&) = delete;
    SyncEngine& operator=(const SyncEngine&) = delete;

    // One pass over every pending transaction. Thread-safe: waits if the
    // worker is mid-pass. Storage errors propagate.
    SyncReport syncPendingTransactions();

    // Starts the background worker; it runs a first pass straight away.
    // Calling start() on a running engine does nothing.
    void start();

    // Stops the worker and waits for it to finish. Safe to call repeatedly.
    void stop();

    // "A new sale is waiting": wakes the worker if it is idle. During a
    // backoff the hint is remembered but does not cut the wait short, so a
    // queue of offline sales cannot turn into a retry storm.
    void notifyPendingTransaction();

    // "The server just answered": ends a backoff early, since the outage is
    // evidently over. Used when a live sale goes through while sales queue.
    void notifyServerReachable();

    [[nodiscard]]
    WorkerStatus workerStatus() const;

private:
    void run(std::stop_token stopToken);

    storage::TransactionRepository& repository_;
    network::ServerClient& serverClient_;
    RetryPolicy retryPolicy_;

    std::mutex passMutex_;  // one pass at a time

    mutable std::mutex stateMutex_;  // guards everything below, and the cv
    std::condition_variable_any wakeUp_;
    bool pendingSignalled_{false};
    bool reachableSignalled_{false};
    WorkerStatus status_;

    // Last member: destroyed (stop requested + joined) before everything the
    // worker thread touches.
    std::jthread worker_;
};

} // namespace resilientpos::sync
