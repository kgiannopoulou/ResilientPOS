#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/sync/SyncEngine.hpp"
#include "TempDatabase.hpp"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::SendResult;
using resilientpos::network::SendResultType;
using resilientpos::network::ServerClient;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::storage::TransactionRepository;
using resilientpos::sync::SyncEngine;
using resilientpos::test::TempDatabase;

namespace {

// Answers `defaultAnswer` unless an ID has its own scripted answer, and
// records the order of sends and each row's stored status while in flight.
class ScriptedServerClient final : public ServerClient {
public:
    ScriptedServerClient(TransactionRepository& repository, SendResultType defaultAnswer)
        : repository_{repository}
        , defaultAnswer_{defaultAnswer}
    {
    }

    SendResult send(const Transaction& transaction) override
    {
        sentIds.push_back(transaction.id);
        statusWhileSending.push_back(repository_.findById(transaction.id)->status);
        if (throwFor == transaction.id) {
            throw std::runtime_error{"socket exploded"};
        }
        const auto scripted = answers.find(transaction.id);
        return {scripted != answers.end() ? scripted->second : defaultAnswer_, "scripted"};
    }

    std::map<std::string, SendResultType> answers;
    std::string throwFor;
    std::vector<std::string> sentIds;
    std::vector<TransactionStatus> statusWhileSending;

private:
    TransactionRepository& repository_;
    SendResultType defaultAnswer_;
};

Transaction savePending(TransactionRepository& repository, Money amount)
{
    auto tx = Transaction::create(amount);
    tx.status = TransactionStatus::PendingSync;
    repository.save(tx);
    return tx;
}

} // namespace

TEST_CASE("Sync with an empty queue sends nothing", "[sync]")
{
    SQLiteTransactionRepository repository{":memory:"};
    ScriptedServerClient server{repository, SendResultType::Accepted};
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(server.sentIds.empty());
    REQUIRE(report.attempted == 0);
    REQUIRE(report.synced == 0);
    REQUIRE(report.stillPending == 0);
}

TEST_CASE("Sync sends every pending transaction oldest first and marks it SYNCED", "[sync]")
{
    SQLiteTransactionRepository repository{":memory:"};
    const auto a = savePending(repository, Money{1000});
    const auto b = savePending(repository, Money{2000});
    const auto c = savePending(repository, Money{3000});
    ScriptedServerClient server{repository, SendResultType::Accepted};
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(server.sentIds == std::vector<std::string>{a.id, b.id, c.id});
    REQUIRE(report.attempted == 3);
    REQUIRE(report.synced == 3);
    REQUIRE(report.failed == 0);
    REQUIRE(report.stillPending == 0);
    for (const auto& tx : {a, b, c}) {
        REQUIRE(repository.findById(tx.id)->status == TransactionStatus::Synced);
    }
}

TEST_CASE("Sync marks a transaction SYNCING while it is being sent", "[sync]")
{
    SQLiteTransactionRepository repository{":memory:"};
    savePending(repository, Money{1000});
    ScriptedServerClient server{repository, SendResultType::Accepted};
    SyncEngine engine{repository, server};

    engine.syncPendingTransactions();

    REQUIRE(server.statusWhileSending == std::vector<TransactionStatus>{TransactionStatus::Syncing});
}

TEST_CASE("Sync returns failed sends to PENDING_SYNC and carries on", "[sync][offline]")
{
    for (const auto failure : {SendResultType::NetworkError, SendResultType::ServerError}) {
        SQLiteTransactionRepository repository{":memory:"};
        const auto a = savePending(repository, Money{1000});
        const auto b = savePending(repository, Money{2000});
        const auto c = savePending(repository, Money{3000});
        ScriptedServerClient server{repository, SendResultType::Accepted};
        server.answers[b.id] = failure;
        SyncEngine engine{repository, server};

        const auto report = engine.syncPendingTransactions();

        REQUIRE(server.sentIds.size() == 3);  // one failure does not stop the pass
        REQUIRE(report.synced == 2);
        REQUIRE(report.failed == 1);
        REQUIRE(report.stillPending == 1);
        REQUIRE(repository.findById(a.id)->status == TransactionStatus::Synced);
        REQUIRE(repository.findById(b.id)->status == TransactionStatus::PendingSync);
        REQUIRE(repository.findById(c.id)->status == TransactionStatus::Synced);
    }
}

TEST_CASE("Sync treats ALREADY_PROCESSED as success", "[sync][idempotency]")
{
    SQLiteTransactionRepository repository{":memory:"};
    const auto tx = savePending(repository, Money{1040});
    ScriptedServerClient server{repository, SendResultType::AlreadyProcessed};
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.synced == 1);
    REQUIRE(report.alreadyProcessed == 1);
    REQUIRE(report.stillPending == 0);
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::Synced);
}

TEST_CASE("Sync keeps a transaction pending if the client throws", "[sync][offline]")
{
    SQLiteTransactionRepository repository{":memory:"};
    const auto tx = savePending(repository, Money{1000});
    ScriptedServerClient server{repository, SendResultType::Accepted};
    server.throwFor = tx.id;
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(report.failed == 1);
    REQUIRE(repository.findById(tx.id)->status == TransactionStatus::PendingSync);
}

TEST_CASE("Synced transactions are not sent again", "[sync]")
{
    SQLiteTransactionRepository repository{":memory:"};
    savePending(repository, Money{1000});
    ScriptedServerClient server{repository, SendResultType::Accepted};
    SyncEngine engine{repository, server};

    engine.syncPendingTransactions();
    const auto second = engine.syncPendingTransactions();

    REQUIRE(server.sentIds.size() == 1);
    REQUIRE(second.attempted == 0);
}

TEST_CASE("Sync resends a transaction left SYNCING by a crash", "[sync][durability]")
{
    TempDatabase db;
    std::string id;
    {
        SQLiteTransactionRepository repository{db.path()};
        id = savePending(repository, Money{1000}).id;
        repository.updateStatus(id, TransactionStatus::Syncing);
    } // till dies mid-sync

    SQLiteTransactionRepository repository{db.path()};
    ScriptedServerClient server{repository, SendResultType::AlreadyProcessed};
    SyncEngine engine{repository, server};

    const auto report = engine.syncPendingTransactions();

    REQUIRE(server.sentIds == std::vector<std::string>{id});
    REQUIRE(report.synced == 1);
    REQUIRE(repository.findById(id)->status == TransactionStatus::Synced);
}

TEST_CASE("Sync logs each outcome and a summary", "[sync]")
{
    SQLiteTransactionRepository repository{":memory:"};
    const auto accepted = savePending(repository, Money{1000});
    const auto duplicate = savePending(repository, Money{2000});
    const auto offline = savePending(repository, Money{3000});
    ScriptedServerClient server{repository, SendResultType::Accepted};
    server.answers[duplicate.id] = SendResultType::AlreadyProcessed;
    server.answers[offline.id] = SendResultType::NetworkError;

    std::vector<std::string> lines;
    SyncEngine engine{repository, server, [&lines](std::string_view line) {
        lines.emplace_back(line);
    }};
    engine.syncPendingTransactions();

    const auto contains = [&lines](const std::string& text) {
        for (const auto& line : lines) {
            if (line == text) {
                return true;
            }
        }
        return false;
    };
    REQUIRE(lines.front() == "[SYNC] Starting synchronization...");
    REQUIRE(contains("[SYNC] " + accepted.id.substr(0, 8) + " → SUCCESS"));
    REQUIRE(contains("[SYNC] " + duplicate.id.substr(0, 8) + " → already processed by server"));
    REQUIRE(contains("[SYNC] " + duplicate.id.substr(0, 8) + " → SYNCED"));
    REQUIRE(contains("2 transactions synchronized."));
    REQUIRE(lines.back() == "1 transactions pending.");
}
