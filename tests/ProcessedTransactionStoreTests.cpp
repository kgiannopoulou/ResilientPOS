#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "TempDatabase.hpp"

#include <catch2/catch_test_macros.hpp>

using resilientpos::server::ProcessedTransactionStore;
using resilientpos::server::RecordOutcome;
using resilientpos::test::TempDatabase;

TEST_CASE("Server store inserts a new transaction", "[server][store]")
{
    ProcessedTransactionStore store{":memory:"};

    REQUIRE(store.record("abc", 700) == RecordOutcome::Inserted);
    REQUIRE(store.amountFor("abc") == 700);
    REQUIRE(store.count() == 1);
}

TEST_CASE("Server store does not insert the same ID twice", "[server][store][idempotency]")
{
    ProcessedTransactionStore store{":memory:"};

    REQUIRE(store.record("abc", 700) == RecordOutcome::Inserted);
    REQUIRE(store.record("abc", 700) == RecordOutcome::AlreadyExists);
    REQUIRE(store.record("abc", 700) == RecordOutcome::AlreadyExists);

    REQUIRE(store.count() == 1);
}

TEST_CASE("Server store keeps the original amount on a duplicate ID", "[server][store][idempotency]")
{
    ProcessedTransactionStore store{":memory:"};

    REQUIRE(store.record("abc", 700) == RecordOutcome::Inserted);
    REQUIRE(store.record("abc", 999) == RecordOutcome::AlreadyExists);

    REQUIRE(store.amountFor("abc") == 700);
}

TEST_CASE("Server store reports unknown IDs as absent", "[server][store]")
{
    ProcessedTransactionStore store{":memory:"};

    REQUIRE_FALSE(store.amountFor("missing").has_value());
    REQUIRE(store.count() == 0);
}

TEST_CASE("Server store remembers transactions after a restart", "[server][store]")
{
    TempDatabase db;
    {
        ProcessedTransactionStore store{db.path()};
        REQUIRE(store.record("abc", 700) == RecordOutcome::Inserted);
    }

    ProcessedTransactionStore reopened{db.path()};
    REQUIRE(reopened.record("abc", 700) == RecordOutcome::AlreadyExists);
    REQUIRE(reopened.count() == 1);
}
