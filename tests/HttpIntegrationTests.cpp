// End-to-end over a real socket: HttpServerClient -> HttpTransactionServer.

#include "resilientpos/core/Transaction.hpp"
#include "resilientpos/network/HttpServerClient.hpp"
#include "resilientpos/server/HttpTransactionServer.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <thread>

using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::HttpServerClient;
using resilientpos::network::SendResultType;
using resilientpos::server::HttpTransactionServer;
using resilientpos::server::ProcessedTransactionStore;
using resilientpos::service::TransactionService;
using resilientpos::storage::SQLiteTransactionRepository;

namespace {

// Runs a server on a free local port for the lifetime of the object.
class RunningServer {
public:
    explicit RunningServer(ProcessedTransactionStore& store)
        : server_{store, "127.0.0.1", 0}
        , thread_{[this] { server_.run(); }}
    {
    }

    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    ~RunningServer()
    {
        server_.stop();
        thread_.join();
    }

    [[nodiscard]]
    std::uint16_t port() const { return server_.port(); }

private:
    HttpTransactionServer server_;
    std::thread thread_;
};

// A port that nothing is listening on: bind one, then shut the server down.
std::uint16_t closedPort()
{
    ProcessedTransactionStore store{":memory:"};
    RunningServer server{store};
    return server.port();
}

} // namespace

TEST_CASE("Client gets ACCEPTED, then ALREADY_PROCESSED for the same transaction", "[http][idempotency]")
{
    ProcessedTransactionStore store{":memory:"};
    RunningServer server{store};
    HttpServerClient client{"127.0.0.1", server.port()};

    const auto tx = Transaction::create(Money{700});

    const auto first = client.send(tx);
    INFO(first.message);
    REQUIRE(first.type == SendResultType::Accepted);

    const auto second = client.send(tx);
    INFO(second.message);
    REQUIRE(second.type == SendResultType::AlreadyProcessed);
    REQUIRE(second.successful());

    REQUIRE(store.count() == 1);
    REQUIRE(store.amountFor(tx.id) == 700);
}

TEST_CASE("Client reports a stopped server as NetworkError", "[http][offline]")
{
    HttpServerClient client{"127.0.0.1", closedPort(), std::chrono::seconds{2}};

    const auto result = client.send(Transaction::create(Money{700}));

    REQUIRE(result.type == SendResultType::NetworkError);
    REQUIRE_FALSE(result.successful());
}

TEST_CASE("Online sale ends SYNCED on the POS and processed on the server", "[http][service]")
{
    ProcessedTransactionStore serverStore{":memory:"};
    RunningServer server{serverStore};

    SQLiteTransactionRepository posRepository{":memory:"};
    HttpServerClient client{"127.0.0.1", server.port()};
    TransactionService service{posRepository, client};

    const auto tx = service.createTransaction(Money{350} * 2);

    REQUIRE(tx.status == TransactionStatus::Synced);
    REQUIRE(posRepository.findById(tx.id)->status == TransactionStatus::Synced);
    REQUIRE(serverStore.amountFor(tx.id) == 700);
}
