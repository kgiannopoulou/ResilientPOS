#include "resilientpos/service/TransactionService.hpp"

#include <spdlog/spdlog.h>

#include <exception>
#include <utility>

namespace resilientpos::service {

using core::TransactionStatus;
using network::SendResult;
using network::SendResultType;

TransactionService::TransactionService(
    storage::TransactionRepository& repository,
    network::ServerClient& serverClient,
    TransactionEvents events)
    : repository_{repository}
    , serverClient_{serverClient}
    , events_{std::move(events)}
{
}

core::Transaction TransactionService::createTransaction(core::Money amount)
{
    auto transaction = core::Transaction::create(amount);
    const auto shortId = transaction.id.substr(0, 8);

    repository_.save(transaction);  // throws -> nothing was sent, sale not taken
    spdlog::info("Transaction created id={} amount={}", shortId, amount.cents());
    if (events_.persisted) {
        events_.persisted(transaction);
    }

    // Only after successful persistence.
    try {
        lastSendResult_ = serverClient_.send(transaction);
    } catch (const std::exception& e) {
        // ServerClient should report failures, not throw; if one does anyway,
        // the sale is already on disk, so treat it like the network being down.
        lastSendResult_ = SendResult{SendResultType::NetworkError, e.what()};
    }

    // Accepted and AlreadyProcessed both mean the server holds it.
    // If this update fails after the server accepted, the row stays CREATED,
    // is requeued on restart and the resend gets ALREADY_PROCESSED: still no
    // duplicate.
    transaction.status = lastSendResult_.successful() ? TransactionStatus::Synced
                                                      : TransactionStatus::PendingSync;
    repository_.updateStatus(transaction.id, transaction.status);

    if (lastSendResult_.successful()) {
        spdlog::info("Transaction synced id={} result={}",
                     shortId, network::toString(lastSendResult_.type));
    } else {
        spdlog::warn("Server unavailable id={} result={} reason=\"{}\"",
                     shortId, network::toString(lastSendResult_.type), lastSendResult_.message);
        spdlog::info("Transaction queued id={} status=PENDING_SYNC", shortId);
    }

    if (events_.completed) {
        events_.completed(transaction, lastSendResult_);
    }
    return transaction;
}

const SendResult& TransactionService::lastSendResult() const noexcept
{
    return lastSendResult_;
}

} // namespace resilientpos::service
