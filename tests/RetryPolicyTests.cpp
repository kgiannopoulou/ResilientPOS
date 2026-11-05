#include "resilientpos/sync/RetryPolicy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace std::chrono_literals;
using resilientpos::sync::fullJitter;
using resilientpos::sync::noJitter;
using resilientpos::sync::RetryPolicy;

TEST_CASE("Retry ceiling doubles from one second", "[retry]")
{
    const RetryPolicy policy;

    REQUIRE(policy.ceiling(0) == 1s);
    REQUIRE(policy.ceiling(1) == 2s);
    REQUIRE(policy.ceiling(2) == 4s);
    REQUIRE(policy.ceiling(3) == 8s);
    REQUIRE(policy.ceiling(4) == 16s);
}

TEST_CASE("Retry ceiling is capped at thirty seconds", "[retry]")
{
    const RetryPolicy policy;

    REQUIRE(policy.ceiling(5) == 30s);   // 32s would exceed the cap
    REQUIRE(policy.ceiling(6) == 30s);
    REQUIRE(policy.ceiling(10) == 30s);
}

TEST_CASE("Huge attempt counts do not overflow", "[retry]")
{
    const RetryPolicy policy;

    REQUIRE(policy.ceiling(64) == 30s);
    REQUIRE(policy.ceiling(1000) == 30s);
    REQUIRE(policy.ceiling(std::numeric_limits<unsigned int>::max()) == 30s);
    REQUIRE(policy.nextDelay(std::numeric_limits<unsigned int>::max()) <= 30s);
}

TEST_CASE("Retry policy accepts a custom base and cap", "[retry]")
{
    const RetryPolicy policy{100ms, 1s};

    REQUIRE(policy.ceiling(0) == 100ms);
    REQUIRE(policy.ceiling(1) == 200ms);
    REQUIRE(policy.ceiling(2) == 400ms);
    REQUIRE(policy.ceiling(3) == 800ms);
    REQUIRE(policy.ceiling(4) == 1s);
}

TEST_CASE("Retry policy rejects nonsensical settings", "[retry]")
{
    REQUIRE_THROWS_AS(RetryPolicy(0ms, 1s), std::invalid_argument);
    REQUIRE_THROWS_AS(RetryPolicy(2s, 1s), std::invalid_argument);
    REQUIRE_THROWS_AS(RetryPolicy(1s, 2s, nullptr), std::invalid_argument);
}

TEST_CASE("No jitter waits exactly the ceiling", "[retry][jitter]")
{
    const RetryPolicy policy{1s, 30s, noJitter()};

    REQUIRE(policy.nextDelay(0) == 1s);
    REQUIRE(policy.nextDelay(3) == 8s);
    REQUIRE(policy.nextDelay(10) == 30s);
}

TEST_CASE("Injected jitter decides the delay", "[retry][jitter]")
{
    std::vector<std::chrono::milliseconds> ceilingsSeen;
    const RetryPolicy policy{1s, 30s, [&](std::chrono::milliseconds ceiling) {
        ceilingsSeen.push_back(ceiling);
        return ceiling / 4;
    }};

    REQUIRE(policy.nextDelay(0) == 250ms);
    REQUIRE(policy.nextDelay(2) == 1s);
    REQUIRE(ceilingsSeen == std::vector<std::chrono::milliseconds>{1s, 4s});
}

TEST_CASE("A misbehaving jitter function cannot escape the window", "[retry][jitter]")
{
    const RetryPolicy tooLong{1s, 30s, [](std::chrono::milliseconds) { return 1h; }};
    const RetryPolicy negative{1s, 30s, [](std::chrono::milliseconds) { return -5s; }};

    REQUIRE(tooLong.nextDelay(0) == 1s);
    REQUIRE(negative.nextDelay(0) == 0ms);
}

TEST_CASE("Full jitter stays inside [0, ceiling] and covers the window", "[retry][jitter]")
{
    const RetryPolicy policy{1s, 30s, fullJitter(42)};

    for (unsigned int attempt = 0; attempt < 8; ++attempt) {
        auto lowest = policy.ceiling(attempt);
        auto highest = 0ms;
        for (int i = 0; i < 1000; ++i) {
            const auto delay = policy.nextDelay(attempt);
            REQUIRE(delay >= 0ms);
            REQUIRE(delay <= policy.ceiling(attempt));
            lowest = std::min(lowest, delay);
            highest = std::max(highest, delay);
        }
        // Reaches both ends of the window, not just its middle.
        REQUIRE(lowest < policy.ceiling(attempt) / 10);
        REQUIRE(highest > policy.ceiling(attempt) * 9 / 10);
    }
}

TEST_CASE("A fixed seed reproduces the same delays", "[retry][jitter]")
{
    const RetryPolicy first{1s, 30s, fullJitter(7)};
    const RetryPolicy second{1s, 30s, fullJitter(7)};
    const RetryPolicy other{1s, 30s, fullJitter(8)};

    std::vector<std::chrono::milliseconds> a;
    std::vector<std::chrono::milliseconds> b;
    std::vector<std::chrono::milliseconds> c;
    for (unsigned int attempt = 0; attempt < 20; ++attempt) {
        a.push_back(first.nextDelay(attempt));
        b.push_back(second.nextDelay(attempt));
        c.push_back(other.nextDelay(attempt));
    }

    REQUIRE(a == b);
    REQUIRE(a != c);
}

TEST_CASE("Jitter spreads a fleet that failed at the same moment", "[retry][jitter]")
{
    // 100 tills, one retry each at attempt 4 (a 16 s window), bucketed by second.
    // Without jitter all 100 land in a single second.
    constexpr std::size_t kTills = 100;
    std::array<std::size_t, 17> perSecond{};
    for (std::size_t till = 0; till < kTills; ++till) {
        const RetryPolicy policy{1s, 30s, fullJitter(1000 + till)};
        const auto delay = policy.nextDelay(4);
        ++perSecond[static_cast<std::size_t>(
            std::chrono::duration_cast<std::chrono::seconds>(delay).count())];
    }

    const auto busiest = *std::ranges::max_element(perSecond);
    REQUIRE(busiest < kTills / 4);  // uniform would be ~6 per second
}

TEST_CASE("Copies of a policy share one generator rather than repeating it", "[retry][jitter]")
{
    const RetryPolicy original{1s, 30s, fullJitter(99)};
    const RetryPolicy copy = original;  // NOLINT(performance-unnecessary-copy-initialization)

    std::vector<std::chrono::milliseconds> a;
    std::vector<std::chrono::milliseconds> b;
    for (int i = 0; i < 20; ++i) {
        a.push_back(original.nextDelay(6));
        b.push_back(copy.nextDelay(6));
    }

    REQUIRE(a != b);
}
