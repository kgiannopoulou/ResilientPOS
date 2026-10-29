#include "resilientpos/service/TransactionService.hpp"

#include <exception>

namespace resilientpos::service {

using core::TransactionStatus;
using network::SendResult;
using network::SendResultType;

TransactionService::TransactionService(
    storage::TransactionRepository& repository,
    network::ServerClient& serverClient)
    : repository_{repository}
    , serverClient_{serverClient}
{
}

core::Transaction TransactionService::createTransaction(core::Money amount)
{
    auto transaction = core::Transaction::create(amount);

    // Saved as PENDING_SYNC rather than CREATED: if the till dies while the
    // request is in flight, the sale is already queued for the sync worker
    // instead of stuck in a state nothing picks up.
    transaction.status = TransactionStatus::PendingSync;
    repository_.save(transaction);  // throws -> nothing was sent, sale not taken

    // Only after successful persistence.
    try {
        lastSendResult_ = serverClient_.send(transaction);
    } catch (const std::exception& e) {
        // ServerClient should report failures, not throw; if one does anyway,
        // the sale is already on disk, so treat it like the network being down.
        lastSendResult_ = SendResult{SendResultType::NetworkError, e.what()};
    }

    // Accepted and AlreadyProcessed both mean the server holds it.
    // If this update fails after the server accepted, the row stays
    // PENDING_SYNC and the resend gets ALREADY_PROCESSED: still no duplicate.
    transaction.status = lastSendResult_.successful() ? TransactionStatus::Synced
                                                      : TransactionStatus::PendingSync;
    repository_.updateStatus(transaction.id, transaction.status);
    return transaction;
}

const SendResult& TransactionService::lastSendResult() const noexcept
{
    return lastSendResult_;
}

} // namespace resilientpos::service
