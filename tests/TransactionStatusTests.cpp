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

TEST_CASE("TransactionStatus parses back from its string", "[status]")
{
    using resilientpos::core::transactionStatusFromString;

    for (const auto status : {TransactionStatus::Created,
                              TransactionStatus::PendingSync,
                              TransactionStatus::Syncing,
                              TransactionStatus::Synced,
                              TransactionStatus::Failed}) {
        REQUIRE(transactionStatusFromString(toString(status)) == status);
    }

    REQUIRE_FALSE(transactionStatusFromString("pending_sync").has_value());
    REQUIRE_FALSE(transactionStatusFromString("").has_value());
}
