#include "resilientpos/network/SendResult.hpp"
#include "resilientpos/network/ServerClient.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::network::SendResult;
using resilientpos::network::SendResultType;
using resilientpos::network::ServerClient;

TEST_CASE("Accepted and AlreadyProcessed count as success", "[network]")
{
    REQUIRE(SendResult{SendResultType::Accepted, ""}.successful());
    REQUIRE(SendResult{SendResultType::AlreadyProcessed, "duplicate"}.successful());
}

TEST_CASE("Network and server errors do not count as success", "[network]")
{
    REQUIRE_FALSE(SendResult{SendResultType::NetworkError, "timeout"}.successful());
    REQUIRE_FALSE(SendResult{SendResultType::ServerError, "500"}.successful());
}

TEST_CASE("SendResultType converts to string", "[network]")
{
    using resilientpos::network::toString;

    REQUIRE(toString(SendResultType::Accepted) == "ACCEPTED");
    REQUIRE(toString(SendResultType::AlreadyProcessed) == "ALREADY_PROCESSED");
    REQUIRE(toString(SendResultType::NetworkError) == "NETWORK_ERROR");
    REQUIRE(toString(SendResultType::ServerError) == "SERVER_ERROR");
}

namespace {

// Minimal in-memory server: accepts each ID once, reports repeats as duplicates.
class FakeServerClient final : public ServerClient {
public:
    SendResult send(const Transaction& transaction) override
    {
        for (const auto& id : received_) {
            if (id == transaction.id) {
                return {SendResultType::AlreadyProcessed, "duplicate"};
            }
        }
        received_.push_back(transaction.id);
        return {SendResultType::Accepted, "stored"};
    }

private:
    std::vector<std::string> received_;
};

} // namespace

TEST_CASE("Resending the same transaction is still a success", "[network]")
{
    FakeServerClient fake;
    ServerClient& server = fake; // callers only see the interface
    const auto tx = Transaction::create(Money{350});

    const auto first = server.send(tx);
    const auto retry = server.send(tx);

    REQUIRE(first.type == SendResultType::Accepted);
    REQUIRE(retry.type == SendResultType::AlreadyProcessed);
    REQUIRE(first.successful());
    REQUIRE(retry.successful());
}
