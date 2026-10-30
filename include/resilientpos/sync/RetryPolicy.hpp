#pragma once

#include <chrono>

namespace resilientpos::sync {

// Exponential backoff between sync attempts:
//
//   attempt   0    1    2    3    4     5+
//   delay     1s   2s   4s   8s   16s   30s (cap)
//
// delay = min(initialDelay * 2^attempt, maxDelay). Without the cap a till
// that was offline for an afternoon would wait hours before trying again.
// Deterministic for now (no jitter), so every delay can be tested exactly.
class RetryPolicy {
public:
    explicit RetryPolicy(
        std::chrono::milliseconds initialDelay = std::chrono::seconds{1},
        std::chrono::milliseconds maxDelay = std::chrono::seconds{30}
    );

    // How long to wait before retry number `attempt` (0 = first retry).
    // Never overflows: large attempts simply return maxDelay.
    [[nodiscard]]
    std::chrono::milliseconds
    nextDelay(unsigned int attempt) const;

private:
    std::chrono::milliseconds initialDelay_;
    std::chrono::milliseconds maxDelay_;
};

} // namespace resilientpos::sync
