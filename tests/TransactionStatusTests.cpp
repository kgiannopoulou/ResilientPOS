#include "resilientpos/core/TransactionStatus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

using resilientpos::core::TransactionStatus;
using resilientpos::core::toString;

TEST_CASE("TransactionStatus converts to CLI string", "[status]")
{
    REQUIRE(toString(TransactionStatus::Created) == "CREATED");
    REQUIRE(toString(TransactionStatus::PendingSync) == "PENDING_SYNC");
    REQUIRE(toString(TransactionStatus::Syncing) == "SYNCING");
    REQUIRE(toString(TransactionStatus::Synced) == "SYNCED");
    REQUIRE(toString(TransactionStatus::Failed) == "FAILED");
}

TEST_CASE("TransactionStatus streams as its string", "[status]")
{
    std::ostringstream out;
    out << TransactionStatus::PendingSync;
    REQUIRE(out.str() == "PENDING_SYNC");
}
