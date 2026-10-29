#pragma once

#include "resilientpos/core/Money.hpp"
#include "resilientpos/core/Transaction.hpp"
#include "resilientpos/network/SendResult.hpp"
#include "resilientpos/network/ServerClient.hpp"
#include "resilientpos/storage/TransactionRepository.hpp"

namespace resilientpos::service {

// Rings up a sale. The one rule it exists to enforce:
//
//   PERSIST FIRST, SEND SECOND.
//
//   create transaction
//          |
//          v
//   save to SQLite  (as PENDING_SYNC)   <- the sale is now safe
//          |
//          v
//   send to server
//        /   \
//   success   failure
//      |         |
//   SYNCED   PENDING_SYNC  (kept for the sync worker; the till carries on)
//
// A network or server failure never fails the sale. Only a storage failure
// does (it throws), because then the sale really is not recorded anywhere.
class TransactionService {
public:
    TransactionService(
        storage::TransactionRepository& repository,
        network::ServerClient& serverClient
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
    network::SendResult lastSendResult_{network::SendResultType::NetworkError, "nothing sent yet"};
};

} // namespace resilientpos::service
