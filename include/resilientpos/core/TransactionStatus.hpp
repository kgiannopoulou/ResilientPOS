#pragma once

#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace resilientpos::core {

// Lifecycle of a sale on the till. Only these transitions are legal:
//
//   CREATED ──────────────────────────────► SYNCED      first send accepted
//      │                                      ▲
//      ▼                                      │
//   PENDING_SYNC ──► SYNCING ─────────────────┘         resend accepted
//         ▲             │
//         └─────────────┘                               resend failed
//
//   CREATED  the TransactionService is sending it right now
//   SYNCING  the SyncEngine is resending it right now
//
// A CREATED or SYNCING row found on disk at startup belongs to a run that died
// mid-send; it is returned to PENDING_SYNC (both legal moves) and resent.
// SYNCED is final: nothing can move a sale the server holds back into the queue.
enum class TransactionStatus {
    Created,
    PendingSync,
    Syncing,
    Synced
};

// True if a transaction may move from `from` to `to` (see the diagram above).
// Staying in the same state is not a transition and returns false.
[[nodiscard]]
bool canTransition(TransactionStatus from, TransactionStatus to) noexcept;

// Thrown when code asks for a transition canTransition() rejects. This is a
// programming error, never an expected runtime condition.
class InvalidTransitionError : public std::logic_error {
public:
    InvalidTransitionError(TransactionStatus from, TransactionStatus to);

    [[nodiscard]] TransactionStatus from() const noexcept { return from_; }
    [[nodiscard]] TransactionStatus to() const noexcept { return to_; }

private:
    TransactionStatus from_;
    TransactionStatus to_;
};

// TransactionStatus::PendingSync -> "PENDING_SYNC"
[[nodiscard]]
std::string_view toString(TransactionStatus status) noexcept;

// "PENDING_SYNC" -> TransactionStatus::PendingSync, unknown text -> nullopt.
// Inverse of toString(); used when reading statuses back from storage.
[[nodiscard]]
std::optional<TransactionStatus> transactionStatusFromString(std::string_view text) noexcept;

std::ostream& operator<<(std::ostream& os, TransactionStatus status);

} // namespace resilientpos::core
