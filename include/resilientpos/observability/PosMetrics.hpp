#pragma once

#include "resilientpos/network/SendResult.hpp"
#include "resilientpos/observability/Metrics.hpp"
#include "resilientpos/storage/TransactionRepository.hpp"

#include <chrono>

namespace resilientpos::observability {

// What one till reports. Each field is a metric in the registry:
//
//   transactions_created_total        sales rung up
//   transactions_synced_total         sales the server confirmed
//   sync_failures_total               sends that failed, for any reason
//   retry_attempts_total              resends by the sync engine
//   idempotency_conflicts_total       the server said this ID is a different sale
//   transactions_dead_lettered_total  sales given up on
//   sync_duration_ms                  how long each send took (histogram)
//   transactions_pending              offline queue size right now (gauge)
//   transactions_dead_letter          dead letter queue size right now (gauge)
//
// The two gauges read `repository` at scrape time, so it must outlive `registry`
// being scraped.
struct PosMetrics {
    PosMetrics(MetricsRegistry& registry, storage::TransactionRepository& repository);

    // Bookkeeping shared by every send, from the service and the sync engine.
    void recordSend(const network::SendResult& result, std::chrono::microseconds took) noexcept;

    Counter& transactionsCreated;
    Counter& transactionsSynced;
    Counter& syncFailures;
    Counter& retryAttempts;
    Counter& idempotencyConflicts;
    Counter& transactionsDeadLettered;
    Histogram& syncDuration;
};

} // namespace resilientpos::observability
