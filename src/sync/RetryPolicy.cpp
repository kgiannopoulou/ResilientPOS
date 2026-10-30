#include "resilientpos/sync/RetryPolicy.hpp"

#include <stdexcept>

namespace resilientpos::sync {

RetryPolicy::RetryPolicy(std::chrono::milliseconds initialDelay, std::chrono::milliseconds maxDelay)
    : initialDelay_{initialDelay}
    , maxDelay_{maxDelay}
{
    if (initialDelay_.count() <= 0 || maxDelay_ < initialDelay_) {
        throw std::invalid_argument{"RetryPolicy needs 0 < initialDelay <= maxDelay"};
    }
}

std::chrono::milliseconds RetryPolicy::nextDelay(unsigned int attempt) const
{
    // Double step by step and stop at the cap, instead of computing
    // initialDelay << attempt, which overflows for large attempts.
    auto delay = initialDelay_;
    for (unsigned int i = 0; i < attempt && delay < maxDelay_; ++i) {
        delay = delay > maxDelay_ / 2 ? maxDelay_ : delay * 2;
    }
    return delay < maxDelay_ ? delay : maxDelay_;
}

} // namespace resilientpos::sync
