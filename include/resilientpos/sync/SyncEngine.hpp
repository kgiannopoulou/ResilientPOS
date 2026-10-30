#pragma once

#include "resilientpos/network/ServerClient.hpp"
#include "resilientpos/storage/TransactionRepository.hpp"

#include <cstddef>
#include <functional>
#include <string_view>

namespace resilientpos::sync {

// What one syncPendingTransactions() pass achieved.
struct SyncReport {
    std::size_t attempted{0};
    std::size_t synced{0};            // includes alreadyProcessed
    std::size_t alreadyProcessed{0};  // server had it: a resend after a lost reply
    std::size_t failed{0};            // left PENDING_SYNC for the next pass
    std::size_t stillPending{0};      // PENDING_SYNC rows after the pass
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
//   status = SYNCING
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
// Synchronous on purpose: the caller decides when to run a pass. No threads.
class SyncEngine {
public:
    using Logger = std::function<void(std::string_view)>;

    SyncEngine(
        storage::TransactionRepository& repository,
        network::ServerClient& serverClient,
        Logger logger = {}
    );

    // One pass over every pending transaction. A failure moves on to the next
    // sale rather than stopping the pass. Storage errors propagate.
    SyncReport syncPendingTransactions();

private:
    void log(std::string_view line) const;

    storage::TransactionRepository& repository_;
    network::ServerClient& serverClient_;
    Logger logger_;
};

} // namespace resilientpos::sync
