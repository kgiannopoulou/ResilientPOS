// The central server without sockets: its store and its request handler.

#include "resilientpos/server/NetworkSimulationMode.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/server/TransactionHandler.hpp"
#include "TempDatabase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using resilientpos::server::handleTransactionRequest;
using resilientpos::server::NetworkSimulationMode;
using resilientpos::server::ProcessedTransactionStore;
using resilientpos::server::RecordOutcome;
using resilientpos::test::TempDatabase;

namespace {

constexpr const char* kValidRequest =
    R"({"transaction_id":"550e8400-e29b-41d4-a716-446655440000","amount_cents":700})";

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

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

TEST_CASE("Server accepts a new transaction", "[server][handler]")
{
    ProcessedTransactionStore store{":memory:"};

    const auto response = handleTransactionRequest(kValidRequest, store);

    REQUIRE(response.httpStatus == 200);
    REQUIRE(response.body == R"({"status":"ACCEPTED"})");
    REQUIRE(store.amountFor("550e8400-e29b-41d4-a716-446655440000") == 700);
}

TEST_CASE("Server does not process a duplicate ID twice", "[server][handler][idempotency]")
{
    ProcessedTransactionStore store{":memory:"};

    for (int i = 0; i < 5; ++i) {
        (void)handleTransactionRequest(kValidRequest, store);
    }

    REQUIRE(store.count() == 1);
    REQUIRE(store.amountFor("550e8400-e29b-41d4-a716-446655440000") == 700);
}

TEST_CASE("Server answers ALREADY_PROCESSED for a duplicate ID", "[server][handler][idempotency]")
{
    ProcessedTransactionStore store{":memory:"};

    REQUIRE(handleTransactionRequest(kValidRequest, store).body == R"({"status":"ACCEPTED"})");
    const auto again = handleTransactionRequest(kValidRequest, store);

    REQUIRE(again.httpStatus == 200);
    REQUIRE(again.body == R"({"status":"ALREADY_PROCESSED"})");
}

TEST_CASE("Server rejects malformed requests without storing anything", "[server][handler]")
{
    ProcessedTransactionStore store{":memory:"};

    const char* badRequests[] = {
        "not json",
        "[1, 2, 3]",
        R"({"amount_cents":700})",
        R"({"transaction_id":"","amount_cents":700})",
        R"({"transaction_id":42,"amount_cents":700})",
        R"({"transaction_id":"abc"})",
        R"({"transaction_id":"abc","amount_cents":"700"})",
        R"({"transaction_id":"abc","amount_cents":7.5})",
    };

    for (const char* body : badRequests) {
        INFO("body: " << body);
        const auto response = handleTransactionRequest(body, store);
        REQUIRE(response.httpStatus == 400);
        REQUIRE(contains(response.body, R"("status":"REJECTED")"));
    }
    REQUIRE(store.count() == 0);
}

TEST_CASE("Simulation modes round-trip through their CLI names", "[server][simulation]")
{
    using resilientpos::server::kAllNetworkSimulationModes;
    using resilientpos::server::networkSimulationModeFromString;

    for (const auto mode : kAllNetworkSimulationModes) {
        REQUIRE(networkSimulationModeFromString(toString(mode)) == mode);
    }
    REQUIRE(toString(NetworkSimulationMode::DropBeforeProcessing) == "drop-before");
    REQUIRE(toString(NetworkSimulationMode::DropAfterProcessing) == "drop-after");
    REQUIRE(networkSimulationModeFromString("drop") == NetworkSimulationMode::DropAfterProcessing);
    REQUIRE_FALSE(networkSimulationModeFromString("offline").has_value());
}
