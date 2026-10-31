#pragma once

#include "resilientpos/network/ServerClient.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace resilientpos::test {

// Test double for the central server: no sockets, no HTTP. Records every
// transaction it is handed and answers with `resultToReturn`, unless a
// transaction ID has its own scripted answer in `resultFor`.
//
// Thread-safe, so it can sit behind a running SyncEngine worker. While a
// worker is running, change answers with setResult() and read with the
// locked accessors; the public fields are for single-threaded tests.
class FakeServerClient final : public network::ServerClient {
public:
    network::SendResult send(const core::Transaction& transaction) override
    {
        std::function<void(const core::Transaction&)> hook;
        network::SendResult answer{network::SendResultType::Accepted, ""};
        bool shouldThrow = false;
        {
            const std::scoped_lock lock{mutex_};
            sentTransactions.push_back(transaction);
            hook = onSend;
            shouldThrow = throwOnSend;
            const auto scripted = resultFor.find(transaction.id);
            answer = scripted != resultFor.end()
                ? network::SendResult{scripted->second, "scripted"}
                : resultToReturn;
        }

        if (hook) {
            hook(transaction);  // outside the lock: may block or call back in
        }
        if (shouldThrow) {
            throw std::runtime_error{"socket exploded"};
        }
        return answer;
    }

    void setResult(network::SendResult result)
    {
        const std::scoped_lock lock{mutex_};
        resultToReturn = std::move(result);
    }

    [[nodiscard]]
    std::size_t sendCount() const
    {
        const std::scoped_lock lock{mutex_};
        return sentTransactions.size();
    }

    [[nodiscard]]
    std::vector<std::string> sentIds() const
    {
        const std::scoped_lock lock{mutex_};
        std::vector<std::string> ids;
        for (const auto& tx : sentTransactions) {
            ids.push_back(tx.id);
        }
        return ids;
    }

    network::SendResult resultToReturn{network::SendResultType::Accepted, "fake"};
    std::map<std::string, network::SendResultType> resultFor;
    bool throwOnSend{false};
    // Runs during every send, before the answer is returned.
    std::function<void(const core::Transaction&)> onSend;

    std::vector<core::Transaction> sentTransactions;

private:
    mutable std::mutex mutex_;
};

} // namespace resilientpos::test
