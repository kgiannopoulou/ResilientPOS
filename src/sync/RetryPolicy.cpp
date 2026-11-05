#include "resilientpos/sync/RetryPolicy.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <utility>

namespace resilientpos::sync {

namespace {

Jitter uniformJitter(std::uint64_t seed)
{
    struct Source {
        explicit Source(std::uint64_t s) : generator{s} {}

        std::mutex mutex;
        std::mt19937_64 generator;
    };

    return [source = std::make_shared<Source>(seed)](std::chrono::milliseconds ceiling) {
        std::uniform_int_distribution<std::chrono::milliseconds::rep> pick{0, ceiling.count()};
        const std::scoped_lock lock{source->mutex};
        return std::chrono::milliseconds{pick(source->generator)};
    };
}

} // namespace

Jitter fullJitter()
{
    std::random_device device;
    return uniformJitter((std::uint64_t{device()} << 32) | device());
}

Jitter fullJitter(std::uint64_t seed)
{
    return uniformJitter(seed);
}

Jitter noJitter()
{
    return [](std::chrono::milliseconds ceiling) { return ceiling; };
}

RetryPolicy::RetryPolicy(std::chrono::milliseconds initialDelay,
                         std::chrono::milliseconds maxDelay,
                         Jitter jitter)
    : initialDelay_{initialDelay}
    , maxDelay_{maxDelay}
    , jitter_{std::move(jitter)}
{
    if (initialDelay_.count() <= 0 || maxDelay_ < initialDelay_) {
        throw std::invalid_argument{"RetryPolicy needs 0 < initialDelay <= maxDelay"};
    }
    if (!jitter_) {
        throw std::invalid_argument{"RetryPolicy needs a jitter function"};
    }
}

std::chrono::milliseconds RetryPolicy::ceiling(unsigned int attempt) const
{
    // Double step by step and stop at the cap, instead of computing
    // initialDelay << attempt, which overflows for large attempts.
    auto delay = initialDelay_;
    for (unsigned int i = 0; i < attempt && delay < maxDelay_; ++i) {
        delay = delay > maxDelay_ / 2 ? maxDelay_ : delay * 2;
    }
    return delay < maxDelay_ ? delay : maxDelay_;
}

std::chrono::milliseconds RetryPolicy::nextDelay(unsigned int attempt) const
{
    const auto limit = ceiling(attempt);
    return std::clamp(jitter_(limit), std::chrono::milliseconds::zero(), limit);
}

} // namespace resilientpos::sync
