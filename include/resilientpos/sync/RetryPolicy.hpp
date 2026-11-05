#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

namespace resilientpos::sync {

// Picks the actual wait for a backoff ceiling, somewhere in [0, ceiling].
// A std::function so tests can inject a deterministic one.
using Jitter = std::function<std::chrono::milliseconds(std::chrono::milliseconds ceiling)>;

// Full jitter: uniform in [0, ceiling]. Seeded from std::random_device, so two
// tills started in the same instant still draw different delays. Copies share
// one generator; safe to call from several threads.
[[nodiscard]]
Jitter fullJitter();

// The same, from a fixed seed: reproducible sequences for tests.
[[nodiscard]]
Jitter fullJitter(std::uint64_t seed);

// Always the ceiling: plain exponential backoff, for tests asserting exact timing.
[[nodiscard]]
Jitter noJitter();

// Exponential backoff with full jitter between sync attempts:
//
//   attempt     0    1    2    3    4     5+
//   ceiling     1s   2s   4s   8s   16s   30s (cap)
//   actual      random(0, ceiling), e.g. 0.6s, 1.3s, 2.1s, 5.7s, 9.2s ...
//
// ceiling = min(initialDelay * 2^attempt, maxDelay). Without the cap a till
// that was offline for an afternoon would wait hours before trying again.
//
// Why the randomness: backoff alone spaces out one till's retries, not a fleet's.
// A hundred tills that lost the server at the same moment share the same
// schedule, so they would all hit it at 1s, 3s, 7s, ... together, the thundering
// herd that can knock a recovering server straight back over. Spreading each
// wait over the whole [0, ceiling] window breaks that lockstep.
class RetryPolicy {
public:
    explicit RetryPolicy(
        std::chrono::milliseconds initialDelay = std::chrono::seconds{1},
        std::chrono::milliseconds maxDelay = std::chrono::seconds{30},
        Jitter jitter = fullJitter()
    );

    // The longest retry number `attempt` (0 = first retry) may wait.
    // Never overflows: large attempts simply return maxDelay.
    [[nodiscard]]
    std::chrono::milliseconds
    ceiling(unsigned int attempt) const;

    // How long to actually wait before retry number `attempt`: the jitter's
    // pick for ceiling(attempt), clamped to [0, ceiling] whatever it returned.
    [[nodiscard]]
    std::chrono::milliseconds
    nextDelay(unsigned int attempt) const;

private:
    std::chrono::milliseconds initialDelay_;
    std::chrono::milliseconds maxDelay_;
    Jitter jitter_;
};

} // namespace resilientpos::sync
