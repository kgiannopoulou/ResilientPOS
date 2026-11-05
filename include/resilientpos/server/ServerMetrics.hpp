#pragma once

#include "resilientpos/observability/Metrics.hpp"

namespace resilientpos::server {

// What the central server reports:
//
//   server_requests_total                requests that reached the handler
//                                        (not those a simulation mode refused)
//   server_transactions_accepted_total   stored for the first time
//   server_transactions_duplicate_total  retries absorbed (ALREADY_PROCESSED)
//   idempotency_conflicts_total          same ID, different payload (409)
//   server_transactions_rejected_total   malformed or invalid (400 / 422)
//   server_errors_total                  storage failures (500 / 503)
//   server_request_duration_ms           time in the handler (histogram)
//
// plus the gauges server_transactions_stored and server_io_threads, registered
// by HttpTransactionServer.
struct ServerMetrics {
    explicit ServerMetrics(observability::MetricsRegistry& registry)
        : requests{registry.counter("server_requests_total", "Requests that reached the transaction handler")}
        , accepted{registry.counter("server_transactions_accepted_total", "Transactions stored for the first time")}
        , duplicates{registry.counter("server_transactions_duplicate_total", "Retries absorbed as ALREADY_PROCESSED")}
        , conflicts{registry.counter("idempotency_conflicts_total", "Requests reusing an ID with a different payload")}
        , rejected{registry.counter("server_transactions_rejected_total", "Malformed or invalid requests")}
        , errors{registry.counter("server_errors_total", "Requests failed by a storage error")}
        , requestDuration{registry.histogram("server_request_duration_ms", "Time spent handling a request")}
    {
    }

    observability::Counter& requests;
    observability::Counter& accepted;
    observability::Counter& duplicates;
    observability::Counter& conflicts;
    observability::Counter& rejected;
    observability::Counter& errors;
    observability::Histogram& requestDuration;
};

} // namespace resilientpos::server
