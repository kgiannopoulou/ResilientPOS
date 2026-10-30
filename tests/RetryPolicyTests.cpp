#include "resilientpos/sync/RetryPolicy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <limits>
#include <stdexcept>

using namespace std::chrono_literals;
using resilientpos::sync::RetryPolicy;

TEST_CASE("Retry delay doubles from one second", "[retry]")
{
    const RetryPolicy policy;

    REQUIRE(policy.nextDelay(0) == 1s);
    REQUIRE(policy.nextDelay(1) == 2s);
    REQUIRE(policy.nextDelay(2) == 4s);
    REQUIRE(policy.nextDelay(3) == 8s);
    REQUIRE(policy.nextDelay(4) == 16s);
}

TEST_CASE("Retry delay is capped at thirty seconds", "[retry]")
{
    const RetryPolicy policy;

    REQUIRE(policy.nextDelay(5) == 30s);   // 32s would exceed the cap
    REQUIRE(policy.nextDelay(6) == 30s);
    REQUIRE(policy.nextDelay(10) == 30s);
}

TEST_CASE("Huge attempt counts do not overflow", "[retry]")
{
    const RetryPolicy policy;

    REQUIRE(policy.nextDelay(64) == 30s);
    REQUIRE(policy.nextDelay(1000) == 30s);
    REQUIRE(policy.nextDelay(std::numeric_limits<unsigned int>::max()) == 30s);
}

TEST_CASE("Retry policy accepts a custom base and cap", "[retry]")
{
    const RetryPolicy policy{100ms, 1s};

    REQUIRE(policy.nextDelay(0) == 100ms);
    REQUIRE(policy.nextDelay(1) == 200ms);
    REQUIRE(policy.nextDelay(2) == 400ms);
    REQUIRE(policy.nextDelay(3) == 800ms);
    REQUIRE(policy.nextDelay(4) == 1s);
}

TEST_CASE("Retry policy rejects nonsensical settings", "[retry]")
{
    REQUIRE_THROWS_AS(RetryPolicy(0ms, 1s), std::invalid_argument);
    REQUIRE_THROWS_AS(RetryPolicy(2s, 1s), std::invalid_argument);
}
