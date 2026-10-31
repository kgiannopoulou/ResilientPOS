#include "resilientpos/core/TransactionStatus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sstream>

using resilientpos::core::canTransition;
using resilientpos::core::InvalidTransitionError;
using resilientpos::core::TransactionStatus;
using resilientpos::core::toString;

TEST_CASE("TransactionStatus converts to CLI string", "[status]")
{
    REQUIRE(toString(TransactionStatus::Created) == "CREATED");
    REQUIRE(toString(TransactionStatus::PendingSync) == "PENDING_SYNC");
    REQUIRE(toString(TransactionStatus::Syncing) == "SYNCING");
    REQUIRE(toString(TransactionStatus::Synced) == "SYNCED");
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
                              TransactionStatus::Synced}) {
        REQUIRE(transactionStatusFromString(toString(status)) == status);
    }

    REQUIRE_FALSE(transactionStatusFromString("pending_sync").has_value());
    REQUIRE_FALSE(transactionStatusFromString("FAILED").has_value());
    REQUIRE_FALSE(transactionStatusFromString("").has_value());
}

TEST_CASE("State machine allows exactly the documented transitions", "[status][state-machine]")
{
    using enum TransactionStatus;
    constexpr TransactionStatus all[] = {Created, PendingSync, Syncing, Synced};

    const auto allowed = [](TransactionStatus from, TransactionStatus to) {
        return (from == Created && to == PendingSync)
            || (from == Created && to == Synced)
            || (from == PendingSync && to == Syncing)
            || (from == Syncing && to == Synced)
            || (from == Syncing && to == PendingSync);
    };

    for (const auto from : all) {
        for (const auto to : all) {
            INFO(toString(from) << " -> " << toString(to));
            REQUIRE(canTransition(from, to) == allowed(from, to));
        }
    }
}

TEST_CASE("State machine rejects nonsense transitions", "[status][state-machine]")
{
    using enum TransactionStatus;

    REQUIRE_FALSE(canTransition(Synced, Created));      // a synced sale is final
    REQUIRE_FALSE(canTransition(Synced, PendingSync));  // never resend what the server holds
    REQUIRE_FALSE(canTransition(PendingSync, Synced));  // must be claimed (SYNCING) first
    REQUIRE_FALSE(canTransition(Created, Syncing));     // the worker never takes in-flight sales
    REQUIRE_FALSE(canTransition(PendingSync, PendingSync));
}

TEST_CASE("InvalidTransitionError names both states", "[status][state-machine]")
{
    const InvalidTransitionError error{TransactionStatus::Synced, TransactionStatus::Created};

    REQUIRE(error.from() == TransactionStatus::Synced);
    REQUIRE(error.to() == TransactionStatus::Created);
    REQUIRE(std::string{error.what()} == "Invalid transaction status transition SYNCED -> CREATED");
}
