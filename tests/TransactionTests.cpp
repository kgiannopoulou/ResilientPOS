#include "resilientpos/core/Transaction.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;

TEST_CASE("New transaction has an ID", "[transaction]")
{
    const auto tx = Transaction::create(Money{350});

    REQUIRE_FALSE(tx.id.empty());
}

TEST_CASE("New transactions get different IDs", "[transaction]")
{
    const auto first = Transaction::create(Money{350});
    const auto second = Transaction::create(Money{350});

    REQUIRE(first.id != second.id);
}

TEST_CASE("New transaction contains correct amount", "[transaction]")
{
    const auto tx = Transaction::create(Money{1299});

    REQUIRE(tx.amount == Money{1299});
}

TEST_CASE("New transaction starts CREATED", "[transaction]")
{
    const auto tx = Transaction::create(Money{350});

    REQUIRE(tx.status == TransactionStatus::Created);
    REQUIRE(tx.syncAttempts == 0);
}

TEST_CASE("New transaction contains creation timestamp", "[transaction]")
{
    const auto before = std::chrono::system_clock::now();
    const auto tx = Transaction::create(Money{350});
    const auto after = std::chrono::system_clock::now();

    REQUIRE(tx.createdAt >= before);
    REQUIRE(tx.createdAt <= after);
}
