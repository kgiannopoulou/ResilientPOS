#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/server/TransactionHandler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using resilientpos::server::handleTransactionRequest;
using resilientpos::server::ProcessedTransactionStore;

namespace {

constexpr const char* kValidRequest =
    R"({"transaction_id":"550e8400-e29b-41d4-a716-446655440000","amount_cents":700})";

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("Handler accepts a new transaction", "[server][handler]")
{
    ProcessedTransactionStore store{":memory:"};

    const auto response = handleTransactionRequest(kValidRequest, store);

    REQUIRE(response.httpStatus == 200);
    REQUIRE(response.body == R"({"status":"ACCEPTED"})");
    REQUIRE(store.amountFor("550e8400-e29b-41d4-a716-446655440000") == 700);
}

TEST_CASE("Handler answers ALREADY_PROCESSED for a resent transaction", "[server][handler][idempotency]")
{
    ProcessedTransactionStore store{":memory:"};

    REQUIRE(handleTransactionRequest(kValidRequest, store).body == R"({"status":"ACCEPTED"})");
    const auto again = handleTransactionRequest(kValidRequest, store);

    REQUIRE(again.httpStatus == 200);
    REQUIRE(again.body == R"({"status":"ALREADY_PROCESSED"})");
    REQUIRE(store.count() == 1);
}

TEST_CASE("Handler rejects malformed requests without storing anything", "[server][handler]")
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
