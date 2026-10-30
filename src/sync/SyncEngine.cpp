#include "resilientpos/sync/SyncEngine.hpp"

#include <exception>
#include <string>
#include <utility>

namespace resilientpos::sync {

using core::TransactionStatus;
using network::SendResult;
using network::SendResultType;

namespace {

// Same 8-character short ID the POS "List pending" screen shows.
std::string shortId(const std::string& id)
{
    return id.substr(0, 8);
}

} // namespace

SyncEngine::SyncEngine(
    storage::TransactionRepository& repository,
    network::ServerClient& serverClient,
    Logger logger)
    : repository_{repository}
    , serverClient_{serverClient}
    , logger_{std::move(logger)}
{
}

SyncReport SyncEngine::syncPendingTransactions()
{
    log("[SYNC] Starting synchronization...");
    log("");

    SyncReport report;
    for (auto& transaction : repository_.findPending()) {
        ++report.attempted;
        const auto label = "[SYNC] " + shortId(transaction.id);

        // Marked before the request goes out. If the till dies right here the
        // row is left SYNCING, and findPending() hands it back on the next run.
        repository_.updateStatus(transaction.id, TransactionStatus::Syncing);

        SendResult result{SendResultType::NetworkError, ""};
        try {
            result = serverClient_.send(transaction);
        } catch (const std::exception& e) {
            // ServerClient should report, not throw; if it does, retry later.
            result = SendResult{SendResultType::NetworkError, e.what()};
        }

        if (result.successful()) {
            repository_.updateStatus(transaction.id, TransactionStatus::Synced);
            ++report.synced;
            if (result.type == SendResultType::AlreadyProcessed) {
                ++report.alreadyProcessed;
                log(label + " → already processed by server");
                log(label + " → SYNCED");
            } else {
                log(label + " → SUCCESS");
            }
        } else {
            repository_.updateStatus(transaction.id, TransactionStatus::PendingSync);
            ++report.failed;
            log(label + " → FAILED (" + std::string{network::toString(result.type)} + ": "
                + result.message + "), still PENDING_SYNC");
        }
    }

    report.stillPending = repository_.findPending().size();

    if (report.attempted > 0) {
        log("");
    }
    log(std::to_string(report.synced) + " transactions synchronized.");
    log(std::to_string(report.stillPending) + " transactions pending.");
    return report;
}

void SyncEngine::log(std::string_view line) const
{
    if (logger_) {
        logger_(line);
    }
}

} // namespace resilientpos::sync
