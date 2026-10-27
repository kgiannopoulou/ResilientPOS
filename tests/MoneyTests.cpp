#include "resilientpos/core/Money.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>

using resilientpos::core::Money;
using resilientpos::core::formatMoney;

TEST_CASE("Money stores cents", "[money]")
{
    const Money money{350};

    REQUIRE(money.cents() == 350);
}

TEST_CASE("Money adds correctly", "[money]")
{
    SECTION("operator+")
    {
        REQUIRE((Money{350} + Money{350}).cents() == 700);
    }

    SECTION("operator+=")
    {
        Money total{100};
        total += Money{250};
        REQUIRE(total.cents() == 350);
    }

    SECTION("adding a negative amount (refund)")
    {
        REQUIRE((Money{500} + Money{-150}).cents() == 350);
    }

    SECTION("overflow is detected")
    {
        Money big{std::numeric_limits<std::int64_t>::max()};
        REQUIRE_THROWS_AS(big += Money{1}, std::overflow_error);
    }
}

TEST_CASE("Money compares correctly", "[money]")
{
    REQUIRE(Money{350} == Money{350});
    REQUIRE_FALSE(Money{350} == Money{351});
    REQUIRE(Money{350} + Money{350} == Money{700});
}

TEST_CASE("Money formats 3.50 euros correctly", "[money][format]")
{
    REQUIRE(formatMoney(Money{350}) == "€3.50");
    REQUIRE(formatMoney(Money{350} + Money{350}) == "€7.00");
}

TEST_CASE("Money formats 0.05 euros correctly", "[money][format]")
{
    REQUIRE(formatMoney(Money{5}) == "€0.05");
    REQUIRE(formatMoney(Money{0}) == "€0.00");
    REQUIRE(formatMoney(Money{-5}) == "-€0.05");
    REQUIRE(formatMoney(Money{123456}) == "€1234.56");
}
