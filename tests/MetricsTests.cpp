// Metrics, the Prometheus rendering, and the /health /ready /metrics endpoints.

#include "resilientpos/network/HttpServerClient.hpp"
#include "resilientpos/observability/Metrics.hpp"
#include "resilientpos/observability/OpsEndpoints.hpp"
#include "resilientpos/observability/OpsHttpServer.hpp"
#include "resilientpos/observability/PosMetrics.hpp"
#include "resilientpos/server/NetworkSimulationMode.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/server/ServerMetrics.hpp"
#include "resilientpos/server/TransactionHandler.hpp"
#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/sync/SyncEngine.hpp"
#include "RunningServer.hpp"
#include "fakes/FakeServerClient.hpp"
#include "fakes/FakeTransactionRepository.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::httpGet;
using resilientpos::network::HttpServerClient;
using resilientpos::network::SendResultType;
using resilientpos::observability::Counter;
using resilientpos::observability::handleOpsRequest;
using resilientpos::observability::Histogram;
using resilientpos::observability::MetricsRegistry;
using resilientpos::observability::OpsEndpoints;
using resilientpos::observability::OpsHttpServer;
using resilientpos::observability::PosMetrics;
using resilientpos::observability::Readiness;
using resilientpos::server::handleTransactionRequest;
using resilientpos::server::NetworkSimulationMode;
using resilientpos::server::ProcessedTransactionStore;
using resilientpos::server::ServerMetrics;
using resilientpos::server::ServerOptions;
using resilientpos::service::TransactionService;
using resilientpos::sync::RetryPolicy;
using resilientpos::sync::SyncEngine;
using resilientpos::sync::SyncOptions;
using resilientpos::test::FakeServerClient;
using resilientpos::test::FakeTransactionRepository;
using resilientpos::test::RunningServer;

namespace {

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

Transaction savePending(FakeTransactionRepository& repository, Money amount)
{
    auto tx = Transaction::create(amount);
    tx.status = TransactionStatus::PendingSync;
    repository.save(tx);
    return tx;
}

OpsEndpoints fixedEndpoints(bool ready)
{
    return OpsEndpoints{
        .metrics = [] { return std::string{"up 1\n"}; },
        .readiness = [ready] { return Readiness{ready, {{"database", ready ? "ok" : "down"}}}; },
    };
}

} // namespace

// --- Primitives ----------------------------------------------------------------

TEST_CASE("Counter counts, also from many threads", "[metrics]")
{
    Counter counter;
    counter.increment();
    counter.increment(4);
    REQUIRE(counter.value() == 5);

    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 1000; ++i) {
                    counter.increment();
                }
            });
        }
    }
    REQUIRE(counter.value() == 8005);
}

TEST_CASE("Histogram buckets are cumulative and bounds are inclusive", "[metrics]")
{
    Histogram histogram{{10, 100}};

    histogram.observe(5ms);
    histogram.observe(10ms);   // on the bound: belongs to le="10"
    histogram.observe(50ms);
    histogram.observe(5s);     // past every bound: +Inf only

    REQUIRE(histogram.cumulativeCounts() == std::vector<std::uint64_t>{2, 3});
    REQUIRE(histogram.count() == 4);
    REQUIRE(histogram.sumMs() == 5065.0);
}

TEST_CASE("Histogram refuses unsorted buckets", "[metrics]")
{
    REQUIRE_THROWS_AS(Histogram({100, 10}), std::invalid_argument);
}

// --- Registry ------------------------------------------------------------------

TEST_CASE("Registry renders the Prometheus text format", "[metrics][prometheus]")
{
    MetricsRegistry registry;
    registry.counter("sales_total", "Sales rung up").increment(3);
    registry.gauge("queue_size", "Sales waiting", [] { return 2.0; });
    auto& latency = registry.histogram("latency_ms", "Send latency", {10, 100});
    latency.observe(5ms);
    latency.observe(50ms);

    REQUIRE(registry.renderPrometheus() ==
        "# HELP latency_ms Send latency\n"
        "# TYPE latency_ms histogram\n"
        "latency_ms_bucket{le=\"10\"} 1\n"
        "latency_ms_bucket{le=\"100\"} 2\n"
        "latency_ms_bucket{le=\"+Inf\"} 2\n"
        "latency_ms_sum 55\n"
        "latency_ms_count 2\n"
        "# HELP queue_size Sales waiting\n"
        "# TYPE queue_size gauge\n"
        "queue_size 2\n"
        "# HELP sales_total Sales rung up\n"
        "# TYPE sales_total counter\n"
        "sales_total 3\n");
}

TEST_CASE("Asking for a metric twice returns the same one", "[metrics]")
{
    MetricsRegistry registry;

    auto& first = registry.counter("sales_total", "help");
    auto& second = registry.counter("sales_total", "help");
    first.increment();

    REQUIRE(&first == &second);
    REQUIRE(second.value() == 1);
}

TEST_CASE("One name cannot be two kinds of metric", "[metrics]")
{
    MetricsRegistry registry;
    (void)registry.counter("x", "help");

    REQUIRE_THROWS_AS(registry.histogram("x", "help"), std::invalid_argument);
    REQUIRE_THROWS_AS(registry.gauge("x", "help", [] { return 0.0; }), std::invalid_argument);
}

TEST_CASE("Gauges are read at scrape time", "[metrics]")
{
    MetricsRegistry registry;
    double value = 1;
    registry.gauge("depth", "help", [&] { return value; });

    REQUIRE(contains(registry.renderPrometheus(), "depth 1\n"));
    value = 7;
    REQUIRE(contains(registry.renderPrometheus(), "depth 7\n"));
}

TEST_CASE("A broken gauge does not cost the rest of the scrape", "[metrics]")
{
    MetricsRegistry registry;
    registry.gauge("broken", "help", []() -> double { throw std::runtime_error{"disk gone"}; });
    registry.counter("fine_total", "help").increment();

    const auto text = registry.renderPrometheus();

    REQUIRE_FALSE(contains(text, "broken"));
    REQUIRE(contains(text, "fine_total 1\n"));
}

// --- The till ------------------------------------------------------------------

TEST_CASE("Till metrics follow a sale from created to synced", "[metrics][service]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    MetricsRegistry registry;
    PosMetrics metrics{registry, repository};
    TransactionService service{repository, server, {}, &metrics};

    (void)service.createTransaction(Money{700});

    REQUIRE(metrics.transactionsCreated.value() == 1);
    REQUIRE(metrics.transactionsSynced.value() == 1);
    REQUIRE(metrics.syncFailures.value() == 0);
    REQUIRE(metrics.syncDuration.count() == 1);
}

TEST_CASE("Till metrics show an offline sale and its recovery", "[metrics][sync][offline]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    server.resultToReturn = {SendResultType::NetworkError, "down"};
    MetricsRegistry registry;
    PosMetrics metrics{registry, repository};
    SyncEngine engine{repository, server, RetryPolicy{}, SyncOptions{.metrics = &metrics}};
    TransactionService service{repository, server, {}, &metrics};

    (void)service.createTransaction(Money{700});
    REQUIRE(contains(registry.renderPrometheus(), "transactions_pending 1\n"));
    REQUIRE(metrics.syncFailures.value() == 1);
    REQUIRE(metrics.retryAttempts.value() == 0);  // the first send is not a retry

    engine.syncPendingTransactions();
    REQUIRE(metrics.syncFailures.value() == 2);
    REQUIRE(metrics.retryAttempts.value() == 1);

    server.setResult({SendResultType::Accepted, "back"});
    engine.syncPendingTransactions();

    REQUIRE(metrics.retryAttempts.value() == 2);
    REQUIRE(metrics.transactionsSynced.value() == 1);
    REQUIRE(metrics.syncDuration.count() == 3);
    REQUIRE(contains(registry.renderPrometheus(), "transactions_pending 0\n"));
}

TEST_CASE("Till metrics count conflicts and dead letters", "[metrics][dlq][idempotency]")
{
    FakeTransactionRepository repository;
    savePending(repository, Money{100});
    FakeServerClient server;
    server.resultToReturn = {SendResultType::IdempotencyConflict, "HTTP 409"};
    MetricsRegistry registry;
    PosMetrics metrics{registry, repository};
    SyncEngine engine{repository, server, RetryPolicy{}, SyncOptions{.metrics = &metrics}};

    engine.syncPendingTransactions();

    REQUIRE(metrics.idempotencyConflicts.value() == 1);
    REQUIRE(metrics.transactionsDeadLettered.value() == 1);
    REQUIRE(contains(registry.renderPrometheus(), "transactions_dead_letter 1\n"));
}

TEST_CASE("Service counts a sale the server refused outright", "[metrics][dlq][service]")
{
    FakeTransactionRepository repository;
    FakeServerClient server;
    server.resultToReturn = {SendResultType::Rejected, "HTTP 422"};
    MetricsRegistry registry;
    PosMetrics metrics{registry, repository};
    TransactionService service{repository, server, {}, &metrics};

    (void)service.createTransaction(Money{700});

    REQUIRE(metrics.transactionsDeadLettered.value() == 1);
    REQUIRE(metrics.syncFailures.value() == 1);
}

TEST_CASE("The metric names are the documented ones", "[metrics][prometheus]")
{
    FakeTransactionRepository repository;
    MetricsRegistry registry;
    PosMetrics metrics{registry, repository};

    const auto text = registry.renderPrometheus();

    for (const char* name : {"transactions_created_total", "transactions_synced_total",
                             "transactions_pending", "sync_failures_total", "sync_duration_ms",
                             "retry_attempts_total", "idempotency_conflicts_total",
                             "transactions_dead_lettered_total", "transactions_dead_letter"}) {
        INFO(name);
        REQUIRE(contains(text, std::string{"# TYPE "} + name + ' '));
    }
}

// --- The server ----------------------------------------------------------------

TEST_CASE("Handler counts each kind of outcome", "[metrics][server][handler]")
{
    ProcessedTransactionStore store{":memory:"};
    MetricsRegistry registry;
    ServerMetrics metrics{registry};
    constexpr const char* kSale = R"({"transaction_id":"a","amount_cents":700})";

    (void)handleTransactionRequest(kSale, store, &metrics);
    (void)handleTransactionRequest(kSale, store, &metrics);
    (void)handleTransactionRequest(R"({"transaction_id":"a","amount_cents":900})", store, &metrics);
    (void)handleTransactionRequest("not json", store, &metrics);
    (void)handleTransactionRequest(R"({"transaction_id":"b","amount_cents":0})", store, &metrics);

    REQUIRE(metrics.requests.value() == 5);
    REQUIRE(metrics.accepted.value() == 1);
    REQUIRE(metrics.duplicates.value() == 1);
    REQUIRE(metrics.conflicts.value() == 1);
    REQUIRE(metrics.rejected.value() == 2);
    REQUIRE(metrics.requestDuration.count() == 5);
}

// --- Endpoints -----------------------------------------------------------------

TEST_CASE("Ops endpoints answer without any sockets", "[metrics][ops]")
{
    const auto endpoints = fixedEndpoints(true);

    const auto health = handleOpsRequest("GET", "/health", endpoints);
    REQUIRE(health->httpStatus == 200);
    REQUIRE(health->body == R"({"status":"UP"})");

    const auto ready = handleOpsRequest("GET", "/ready", endpoints);
    REQUIRE(ready->httpStatus == 200);
    REQUIRE(contains(ready->body, R"("status":"READY")"));
    REQUIRE(contains(ready->body, R"("database":"ok")"));

    const auto metrics = handleOpsRequest("GET", "/metrics?format=text", endpoints);
    REQUIRE(metrics->httpStatus == 200);
    REQUIRE(metrics->body == "up 1\n");
    REQUIRE(contains(metrics->contentType, "version=0.0.4"));
}

TEST_CASE("Not ready is a 503 but the process is still healthy", "[metrics][ops]")
{
    const auto endpoints = fixedEndpoints(false);

    REQUIRE(handleOpsRequest("GET", "/ready", endpoints)->httpStatus == 503);
    REQUIRE(contains(handleOpsRequest("GET", "/ready", endpoints)->body, "NOT_READY"));
    REQUIRE(handleOpsRequest("GET", "/health", endpoints)->httpStatus == 200);
}

TEST_CASE("A readiness check that throws means not ready", "[metrics][ops]")
{
    const OpsEndpoints endpoints{
        .metrics = [] { return std::string{}; },
        .readiness = []() -> Readiness { throw std::runtime_error{"database gone"}; },
    };

    const auto ready = handleOpsRequest("GET", "/ready", endpoints);

    REQUIRE(ready->httpStatus == 503);
    REQUIRE(contains(ready->body, "database gone"));
}

TEST_CASE("Ops endpoints leave other paths alone and refuse other methods", "[metrics][ops]")
{
    const auto endpoints = fixedEndpoints(true);

    REQUIRE_FALSE(handleOpsRequest("POST", "/transactions", endpoints).has_value());
    REQUIRE_FALSE(handleOpsRequest("GET", "/healthz", endpoints).has_value());
    REQUIRE(handleOpsRequest("POST", "/metrics", endpoints)->httpStatus == 405);
}

TEST_CASE("Server serves health, readiness and metrics over HTTP", "[metrics][ops][http]")
{
    ProcessedTransactionStore store{":memory:"};
    RunningServer server{store};
    HttpServerClient client{"127.0.0.1", server.port()};

    auto tx = Transaction::create(Money{700});
    REQUIRE(client.send(tx).type == SendResultType::Accepted);
    REQUIRE(client.send(tx).type == SendResultType::AlreadyProcessed);
    tx.amount = Money{900};
    REQUIRE(client.send(tx).type == SendResultType::IdempotencyConflict);

    const auto health = httpGet("127.0.0.1", server.port(), "/health");
    REQUIRE(health->status == 200);
    REQUIRE(health->body == R"({"status":"UP"})");

    const auto ready = httpGet("127.0.0.1", server.port(), "/ready");
    REQUIRE(ready->status == 200);
    REQUIRE(contains(ready->body, R"("database":"ok")"));

    const auto metrics = httpGet("127.0.0.1", server.port(), "/metrics");
    REQUIRE(metrics->status == 200);
    REQUIRE(contains(metrics->contentType, "text/plain"));
    REQUIRE(contains(metrics->body, "server_requests_total 3\n"));
    REQUIRE(contains(metrics->body, "server_transactions_accepted_total 1\n"));
    REQUIRE(contains(metrics->body, "server_transactions_duplicate_total 1\n"));
    REQUIRE(contains(metrics->body, "idempotency_conflicts_total 1\n"));
    REQUIRE(contains(metrics->body, "server_transactions_stored 1\n"));
    REQUIRE(contains(metrics->body, "server_request_duration_ms_count 3\n"));
    REQUIRE(contains(metrics->body, "server_io_threads 4\n"));
}

TEST_CASE("Monitoring still works during a simulated outage", "[metrics][ops][http][simulation]")
{
    ProcessedTransactionStore store{":memory:"};
    RunningServer server{store};

    for (const auto mode : {NetworkSimulationMode::Reject, NetworkSimulationMode::Timeout,
                            NetworkSimulationMode::DropBeforeProcessing}) {
        server.setMode(mode);
        INFO("mode " << toString(mode));

        const auto ready = httpGet("127.0.0.1", server.port(), "/ready");
        REQUIRE(ready.has_value());
        REQUIRE(ready->status == 200);
        REQUIRE(contains(ready->body, std::string{toString(mode)}));
    }
}

TEST_CASE("Metrics survive a server restart when the registry is shared", "[metrics][http]")
{
    ProcessedTransactionStore store{":memory:"};
    MetricsRegistry registry;
    ServerOptions options;
    options.metrics = &registry;

    for (int run = 0; run < 2; ++run) {
        RunningServer server{store, 0, options};
        HttpServerClient client{"127.0.0.1", server.port()};
        REQUIRE(client.send(Transaction::create(Money{700})).type == SendResultType::Accepted);
    }

    REQUIRE(contains(registry.renderPrometheus(), "server_transactions_accepted_total 2\n"));
}

TEST_CASE("The till's listener serves the same endpoints", "[metrics][ops][http]")
{
    OpsHttpServer ops{fixedEndpoints(true), "127.0.0.1", 0};

    REQUIRE(httpGet("127.0.0.1", ops.port(), "/health")->status == 200);
    REQUIRE(httpGet("127.0.0.1", ops.port(), "/ready")->status == 200);
    REQUIRE(httpGet("127.0.0.1", ops.port(), "/metrics")->body == "up 1\n");
    REQUIRE(httpGet("127.0.0.1", ops.port(), "/nope")->status == 404);
}

TEST_CASE("httpGet reports an unreachable host as no response", "[http]")
{
    REQUIRE_FALSE(httpGet("127.0.0.1", resilientpos::test::closedPort(), "/health", 300ms).has_value());
}
