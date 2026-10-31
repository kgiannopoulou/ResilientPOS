#include "resilientpos/sync/SyncEngine.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <string>

namespace resilientpos::sync {

using core::TransactionStatus;
using network::SendResult;
using network::SendResultType;

namespace {

// Same 8-character short ID the POS screens show.
std::string shortId(const std::string& id)
{
    return id.substr(0, 8);
}

} // namespace

SyncEngine::SyncEngine(
    storage::TransactionRepository& repository,
    network::ServerClient& serverClient,
    RetryPolicy retryPolicy)
    : repository_{repository}
    , serverClient_{serverClient}
    , retryPolicy_{retryPolicy}
{
    if (const auto recovered = repository_.recoverInterrupted(); recovered > 0) {
        spdlog::warn("Recovered interrupted transactions count={} status=PENDING_SYNC", recovered);
    }
}

SyncEngine::~SyncEngine()
{
    stop();
}

SyncReport SyncEngine::syncPendingTransactions()
{
    const std::scoped_lock passLock{passMutex_};

    SyncReport report;
    const auto pending = repository_.findPending();
    if (!pending.empty()) {
        spdlog::info("Sync pass started pending={}", pending.size());
    }

    for (const auto& transaction : pending) {
        // Claim the row before the request goes out. If the till dies right
        // here the row is left SYNCING and recoverInterrupted() requeues it on
        // the next start. A row that is not PENDING_SYNC any more is skipped.
        if (!repository_.tryUpdateStatus(transaction.id, TransactionStatus::PendingSync,
                                         TransactionStatus::Syncing)) {
            continue;
        }
        ++report.attempted;

        SendResult result{SendResultType::NetworkError, ""};
        try {
            result = serverClient_.send(transaction);
        } catch (const std::exception& e) {
            // ServerClient should report, not throw; if it does, retry later.
            result = SendResult{SendResultType::NetworkError, e.what()};
        }
        report.outcomes.push_back({transaction.id, result.type, result.message});

        if (result.successful()) {
            repository_.updateStatus(transaction.id, TransactionStatus::Synced);
            ++report.synced;
            if (result.type == SendResultType::AlreadyProcessed) {
                ++report.alreadyProcessed;
            }
            spdlog::info("Transaction synced id={} result={}",
                         shortId(transaction.id), network::toString(result.type));
            continue;
        }

        repository_.updateStatus(transaction.id, TransactionStatus::PendingSync);
        ++report.failed;
        spdlog::warn("Transaction sync failed id={} result={} reason=\"{}\"",
                     shortId(transaction.id), network::toString(result.type), result.message);

        if (result.type == SendResultType::NetworkError) {
            spdlog::warn("Server unreachable, ending sync pass early");
            break;
        }
    }

    report.stillPending = repository_.findPending().size();
    if (report.attempted > 0) {
        spdlog::info("Sync pass finished synced={} already_processed={} failed={} pending={}",
                     report.synced, report.alreadyProcessed, report.failed, report.stillPending);
    }
    return report;
}

void SyncEngine::start()
{
    const std::scoped_lock lock{stateMutex_};
    if (worker_.joinable()) {
        return;
    }
    status_ = WorkerStatus{};
    status_.running = true;
    worker_ = std::jthread{[this](const std::stop_token& stopToken) { run(stopToken); }};
}

void SyncEngine::stop()
{
    if (!worker_.joinable()) {
        return;
    }
    // request_stop() wakes the condition-variable wait below (it is
    // registered with the stop token); join() waits for any in-flight send.
    worker_.request_stop();
    worker_.join();

    const std::scoped_lock lock{stateMutex_};
    status_ = WorkerStatus{};
}

void SyncEngine::notifyPendingTransaction()
{
    {
        const std::scoped_lock lock{stateMutex_};
        pendingSignalled_ = true;
    }
    wakeUp_.notify_all();
}

void SyncEngine::notifyServerReachable()
{
    {
        const std::scoped_lock lock{stateMutex_};
        reachableSignalled_ = true;
    }
    wakeUp_.notify_all();
}

WorkerStatus SyncEngine::workerStatus() const
{
    const std::scoped_lock lock{stateMutex_};
    return status_;
}

void SyncEngine::run(const std::stop_token& stopToken)
{
    spdlog::info("Sync worker started");

    while (!stopToken.stop_requested()) {
        {
            // Signals that arrive from here on are for work this pass may
            // miss, so they will trigger another pass.
            const std::scoped_lock lock{stateMutex_};
            pendingSignalled_ = false;
            reachableSignalled_ = false;
            status_.nextRetryAt.reset();
        }

        bool failed = false;
        try {
            failed = syncPendingTransactions().failed > 0;
        } catch (const std::exception& e) {
            // E.g. the disk is full. Keep the worker alive and try again later.
            spdlog::error("Sync pass aborted: {}", e.what());
            failed = true;
        }

        std::unique_lock lock{stateMutex_};
        if (failed) {
            const auto delay = retryPolicy_.nextDelay(status_.consecutiveFailures);
            ++status_.consecutiveFailures;
            status_.nextRetryAt = std::chrono::steady_clock::now() + delay;
            spdlog::warn("Sync retry scheduled attempt={} delay_ms={}",
                         status_.consecutiveFailures, delay.count());
            // Returns early only if stop is requested or the server is known
            // to be back; new pending sales just wait for the retry.
            wakeUp_.wait_for(lock, stopToken, delay, [this] { return reachableSignalled_; });
        } else {
            status_.consecutiveFailures = 0;
            // Nothing left to do: sleep until a sale is queued (or shutdown).
            wakeUp_.wait(lock, stopToken, [this] { return pendingSignalled_; });
        }
    }

    spdlog::info("Sync worker stopped");
}

} // namespace resilientpos::sync
