#include "resilientpos/sync/SyncEngine.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace resilientpos::sync {

using core::TransactionStatus;
using network::FailureClass;
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
    RetryPolicy retryPolicy,
    SyncOptions options)
    : repository_{repository}
    , serverClient_{serverClient}
    , retryPolicy_{std::move(retryPolicy)}
    , options_{options}
{
    if (options_.maxFailedAttempts == 0) {
        throw std::invalid_argument{"SyncOptions::maxFailedAttempts must be at least 1"};
    }
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

        const auto sendStarted = std::chrono::steady_clock::now();
        SendResult result{SendResultType::NetworkError, ""};
        try {
            result = serverClient_.send(transaction);
        } catch (const std::exception& e) {
            // ServerClient should report, not throw; if it does, retry later.
            result = SendResult{SendResultType::NetworkError, e.what()};
        }
        if (options_.metrics != nullptr) {
            options_.metrics->retryAttempts.increment();
            options_.metrics->recordSend(
                result, std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - sendStarted));
        }

        if (result.successful()) {
            report.outcomes.push_back({transaction.id, result.type, result.message, false});
            repository_.updateStatus(transaction.id, TransactionStatus::Synced);
            ++report.synced;
            if (result.type == SendResultType::AlreadyProcessed) {
                ++report.alreadyProcessed;
            }
            spdlog::info("Transaction synced id={} result={}",
                         shortId(transaction.id), network::toString(result.type));
            continue;
        }

        const FailureClass failureClass = network::classify(result.type);
        const bool countsAgainstLimit = failureClass == FailureClass::Transient;
        const bool limitReached = countsAgainstLimit
            && transaction.failedAttempts + 1 >= options_.maxFailedAttempts;
        const bool giveUp = failureClass == FailureClass::Permanent || limitReached;

        repository_.recordFailure(
            transaction.id,
            giveUp ? TransactionStatus::DeadLetter : TransactionStatus::PendingSync,
            network::toSyncFailure(result),
            countsAgainstLimit);
        report.outcomes.push_back({transaction.id, result.type, result.message, giveUp});

        if (giveUp) {
            ++report.deadLettered;
            if (options_.metrics != nullptr) {
                options_.metrics->transactionsDeadLettered.increment();
            }
            spdlog::error("Transaction dead-lettered id={} result={} attempts={} reason=\"{}\"",
                          shortId(transaction.id), network::toString(result.type),
                          transaction.syncAttempts + 1, result.message);
            continue;
        }

        ++report.failed;
        spdlog::warn("Transaction sync failed id={} result={} reason=\"{}\"",
                     shortId(transaction.id), network::toString(result.type), result.message);

        if (failureClass == FailureClass::Unreachable) {
            spdlog::warn("Server unreachable, ending sync pass early");
            break;
        }
    }

    report.stillPending = repository_.findPending().size();
    if (report.attempted > 0) {
        spdlog::info("Sync pass finished synced={} already_processed={} failed={} "
                     "dead_lettered={} pending={}",
                     report.synced, report.alreadyProcessed, report.failed,
                     report.deadLettered, report.stillPending);
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

std::vector<core::Transaction> SyncEngine::deadLetters()
{
    return repository_.findDeadLetters();
}

bool SyncEngine::retryDeadLetter(const std::string& id)
{
    if (!repository_.requeueDeadLetter(id)) {
        return false;
    }
    spdlog::info("Dead letter requeued id={} status=PENDING_SYNC", shortId(id));
    notifyPendingTransaction();
    return true;
}

std::size_t SyncEngine::retryAllDeadLetters()
{
    std::size_t requeued = 0;
    for (const auto& transaction : repository_.findDeadLetters()) {
        if (repository_.requeueDeadLetter(transaction.id)) {
            ++requeued;
        }
    }
    if (requeued > 0) {
        spdlog::info("Dead letters requeued count={} status=PENDING_SYNC", requeued);
        notifyPendingTransaction();
    }
    return requeued;
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
            const auto ceiling = retryPolicy_.ceiling(status_.consecutiveFailures);
            const auto delay = retryPolicy_.nextDelay(status_.consecutiveFailures);
            ++status_.consecutiveFailures;
            status_.nextRetryAt = std::chrono::steady_clock::now() + delay;
            spdlog::warn("Sync retry scheduled attempt={} delay_ms={} ceiling_ms={}",
                         status_.consecutiveFailures, delay.count(), ceiling.count());
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
