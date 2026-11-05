#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace resilientpos::observability {

// A number that only goes up. Lock-free: safe to bump from any thread.
class Counter {
public:
    void increment(std::uint64_t by = 1) noexcept { value_.fetch_add(by, std::memory_order_relaxed); }

    [[nodiscard]]
    std::uint64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t> value_{0};
};

// Distribution of durations over fixed millisecond buckets. Lock-free, and the
// sum is kept in whole microseconds so it can be an atomic integer.
class Histogram {
public:
    explicit Histogram(std::vector<double> upperBoundsMs);

    void observe(std::chrono::microseconds duration) noexcept;

    [[nodiscard]] std::uint64_t count() const noexcept;
    [[nodiscard]] double sumMs() const noexcept;

    // Bucket bounds and how many observations fell at or below each, cumulative
    // as Prometheus expects. The +Inf bucket is count().
    [[nodiscard]] const std::vector<double>& upperBoundsMs() const noexcept { return bounds_; }
    [[nodiscard]] std::vector<std::uint64_t> cumulativeCounts() const;

private:
    std::vector<double> bounds_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> buckets_;  // one per bound, plus overflow
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> sumMicros_{0};
};

// Latency buckets that suit a LAN round trip through to a retry timeout.
[[nodiscard]]
std::vector<double> defaultLatencyBucketsMs();

// Owns a process's metrics and renders them in the Prometheus text format.
//
// Asking for a name that already exists returns the existing metric, so two
// components can share one and a server restarted within a process keeps
// counting. A gauge is a callback, read at scrape time: a value that lives in
// the database, such as the queue size, is then always current and has no
// second copy to keep in step. Re-registering a gauge replaces its callback.
class MetricsRegistry {
public:
    MetricsRegistry();
    ~MetricsRegistry();

    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

    // The returned references stay valid for the registry's lifetime.
    // Throws std::invalid_argument if `name` is already a different kind of metric.
    Counter& counter(const std::string& name, std::string_view help);
    Histogram& histogram(const std::string& name, std::string_view help,
                         std::vector<double> upperBoundsMs = defaultLatencyBucketsMs());
    void gauge(const std::string& name, std::string_view help, std::function<double()> read);

    // Sorted by name. A gauge whose callback throws is left out of that scrape,
    // because one broken reading must not cost the rest of the metrics.
    [[nodiscard]]
    std::string renderPrometheus() const;

private:
    struct Metric;

    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Metric>> metrics_;
};

} // namespace resilientpos::observability
