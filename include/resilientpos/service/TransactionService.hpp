#pragma once

#include "resilientpos/core/Money.hpp"
#include "resilientpos/core/Transaction.hpp"
#include "resilientpos/network/SendResult.hpp"
#include "resilientpos/network/ServerClient.hpp"
#include "resilientpos/storage/TransactionRepository.hpp"

#include <functional>

namespace resilientpos::service {

// Optional hooks around createTransaction(), so the UI can show progress and
// the sync worker can be woken without the service knowing about either.
struct TransactionEvents {
    // The sale is on disk; the send is about to start.
    std::function<void(const core::Transaction&)> persisted;
    // The send finished; the transaction carries its final status.
    std::function<void(const core::Transaction&, const network::SendResult&)> completed;
};

// Rings up a sale. The one rule it exists to enforce:
//
//   PERSIST FIRST, SEND SECOND.
//
//   create transaction
//          |
//          v
//   save to SQLite  (as CREATED)   <- the sale is now safe
//          |
//          v
//   send to server
//        /   \
//   success   failure
//      |         |
//   SYNCED   PENDING_SYNC  (queued for the sync worker; the till carries on)
//
// While the request is in flight the row is CREATED, which the sync worker
// leaves alone, so the same sale is never sent by both at once. If the till
// dies mid-send the CREATED row is requeued at the next start (see
// TransactionRepository::recoverInterrupted()).
//
// A network or server failure never fails the sale. Only a storage failure
// does (it throws), because then the sale really is not recorded anywhere.
class TransactionService {
public:
    TransactionService(
        storage::TransactionRepository& repository,
        network::ServerClient& serverClient,
        TransactionEvents events = {}
    );

    // Returns the transaction with its final status (SYNCED or PENDING_SYNC).
    // Throws storage::StorageError if the sale could not be saved locally.
    core::Transaction createTransaction(core::Money amount);

    // What the server said about the last createTransaction() call,
    // for the CLI to report ("server unavailable", "accepted", ...).
    [[nodiscard]]
    const network::SendResult& lastSendResult() const noexcept;

private:
    storage::TransactionRepository& repository_;
    network::ServerClient& serverClient_;
    TransactionEvents events_;
    network::SendResult lastSendResult_{network::SendResultType::NetworkError, "nothing sent yet"};
};

} // namespace resilientpos::service
