#include "resilientpos/observability/Metrics.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <stdexcept>
#include <utility>
#include <variant>

namespace resilientpos::observability {

Histogram::Histogram(std::vector<double> upperBoundsMs)
    : bounds_{std::move(upperBoundsMs)}
    , buckets_{std::make_unique<std::atomic<std::uint64_t>[]>(bounds_.size() + 1)}
{
    if (!std::ranges::is_sorted(bounds_)) {
        throw std::invalid_argument{"Histogram bucket bounds must be sorted"};
    }
}

void Histogram::observe(std::chrono::microseconds duration) noexcept
{
    const auto micros = std::max<std::chrono::microseconds::rep>(duration.count(), 0);
    const double ms = static_cast<double>(micros) / 1000.0;

    const auto bucket = std::ranges::lower_bound(bounds_, ms) - bounds_.begin();
    buckets_[static_cast<std::size_t>(bucket)].fetch_add(1, std::memory_order_relaxed);
    sumMicros_.fetch_add(static_cast<std::uint64_t>(micros), std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t Histogram::count() const noexcept
{
    return count_.load(std::memory_order_relaxed);
}

double Histogram::sumMs() const noexcept
{
    return static_cast<double>(sumMicros_.load(std::memory_order_relaxed)) / 1000.0;
}

std::vector<std::uint64_t> Histogram::cumulativeCounts() const
{
    std::vector<std::uint64_t> counts;
    counts.reserve(bounds_.size());
    std::uint64_t running = 0;
    for (std::size_t i = 0; i < bounds_.size(); ++i) {
        running += buckets_[i].load(std::memory_order_relaxed);
        counts.push_back(running);
    }
    return counts;
}

std::vector<double> defaultLatencyBucketsMs()
{
    return {1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
}

struct MetricsRegistry::Metric {
    using Gauge = std::function<double()>;

    std::string help;
    std::variant<std::unique_ptr<Counter>, std::unique_ptr<Histogram>, Gauge> value;
};

MetricsRegistry::MetricsRegistry() = default;
MetricsRegistry::~MetricsRegistry() = default;

Counter& MetricsRegistry::counter(const std::string& name, std::string_view help)
{
    const std::scoped_lock lock{mutex_};
    auto& slot = metrics_[name];
    if (!slot) {
        slot = std::make_unique<Metric>(Metric{std::string{help}, std::make_unique<Counter>()});
    }
    auto* existing = std::get_if<std::unique_ptr<Counter>>(&slot->value);
    if (existing == nullptr) {
        throw std::invalid_argument{"Metric '" + name + "' is not a counter"};
    }
    return **existing;
}

Histogram& MetricsRegistry::histogram(const std::string& name, std::string_view help,
                                      std::vector<double> upperBoundsMs)
{
    const std::scoped_lock lock{mutex_};
    auto& slot = metrics_[name];
    if (!slot) {
        slot = std::make_unique<Metric>(Metric{
            std::string{help}, std::make_unique<Histogram>(std::move(upperBoundsMs))});
    }
    auto* existing = std::get_if<std::unique_ptr<Histogram>>(&slot->value);
    if (existing == nullptr) {
        throw std::invalid_argument{"Metric '" + name + "' is not a histogram"};
    }
    return **existing;
}

void MetricsRegistry::gauge(const std::string& name, std::string_view help,
                            std::function<double()> read)
{
    const std::scoped_lock lock{mutex_};
    auto& slot = metrics_[name];
    if (slot && !std::holds_alternative<Metric::Gauge>(slot->value)) {
        throw std::invalid_argument{"Metric '" + name + "' is not a gauge"};
    }
    slot = std::make_unique<Metric>(Metric{std::string{help}, std::move(read)});
}

std::string MetricsRegistry::renderPrometheus() const
{
    const std::scoped_lock lock{mutex_};

    std::string text;
    const auto header = [&text](const std::string& name, const Metric& metric, std::string_view type) {
        text += std::format("# HELP {} {}\n# TYPE {} {}\n", name, metric.help, name, type);
    };

    for (const auto& [name, metric] : metrics_) {
        if (const auto* counter = std::get_if<std::unique_ptr<Counter>>(&metric->value)) {
            header(name, *metric, "counter");
            text += std::format("{} {}\n", name, (*counter)->value());
        } else if (const auto* histogram = std::get_if<std::unique_ptr<Histogram>>(&metric->value)) {
            header(name, *metric, "histogram");
            const auto& bounds = (*histogram)->upperBoundsMs();
            const auto cumulative = (*histogram)->cumulativeCounts();
            for (std::size_t i = 0; i < bounds.size(); ++i) {
                text += std::format("{}_bucket{{le=\"{}\"}} {}\n", name, bounds[i], cumulative[i]);
            }
            text += std::format("{}_bucket{{le=\"+Inf\"}} {}\n", name, (*histogram)->count());
            text += std::format("{}_sum {}\n", name, (*histogram)->sumMs());
            text += std::format("{}_count {}\n", name, (*histogram)->count());
        } else if (const auto* gauge = std::get_if<Metric::Gauge>(&metric->value)) {
            try {
                const double value = (*gauge)();
                header(name, *metric, "gauge");
                text += std::format("{} {}\n", name, value);
            } catch (const std::exception&) {
                continue;
            }
        }
    }
    return text;
}

} // namespace resilientpos::observability
