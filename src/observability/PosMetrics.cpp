#include "resilientpos/observability/PosMetrics.hpp"

namespace resilientpos::observability {

PosMetrics::PosMetrics(MetricsRegistry& registry, storage::TransactionRepository& repository)
    : transactionsCreated{registry.counter("transactions_created_total", "Sales rung up on this till")}
    , transactionsSynced{registry.counter("transactions_synced_total", "Sales the server confirmed")}
    , syncFailures{registry.counter("sync_failures_total", "Failed attempts to send a sale")}
    , retryAttempts{registry.counter("retry_attempts_total", "Resends by the sync engine")}
    , idempotencyConflicts{registry.counter("idempotency_conflicts_total",
                                            "Sales refused because their ID holds a different payload")}
    , transactionsDeadLettered{registry.counter("transactions_dead_lettered_total", "Sales given up on")}
    , syncDuration{registry.histogram("sync_duration_ms", "Duration of each send to the server")}
{
    registry.gauge("transactions_pending", "Sales waiting in the offline queue", [&repository] {
        return static_cast<double>(repository.findPending().size());
    });
    registry.gauge("transactions_dead_letter", "Sales in the dead letter queue", [&repository] {
        return static_cast<double>(repository.findDeadLetters().size());
    });
}

void PosMetrics::recordSend(const network::SendResult& result, std::chrono::microseconds took) noexcept
{
    syncDuration.observe(took);
    if (result.successful()) {
        transactionsSynced.increment();
        return;
    }
    syncFailures.increment();
    if (result.type == network::SendResultType::IdempotencyConflict) {
        idempotencyConflicts.increment();
    }
}

} // namespace resilientpos::observability
