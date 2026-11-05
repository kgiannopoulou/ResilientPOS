#include "resilientpos/simulation/FleetSimulation.hpp"

#include "resilientpos/core/Money.hpp"
#include "resilientpos/core/Transaction.hpp"
#include "resilientpos/network/HttpServerClient.hpp"
#include "resilientpos/network/ServerClient.hpp"
#include "resilientpos/observability/Metrics.hpp"
#include "resilientpos/observability/PosMetrics.hpp"
#include "resilientpos/server/HttpTransactionServer.hpp"
#include "resilientpos/server/NetworkSimulationMode.hpp"
#include "resilientpos/server/ProcessedTransactionStore.hpp"
#include "resilientpos/server/ServerMetrics.hpp"
#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/sync/RetryPolicy.hpp"
#include "resilientpos/sync/SyncEngine.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace resilientpos::simulation {

namespace {

using core::Money;
using core::Transaction;
using core::TransactionStatus;
using network::SendResult;
using network::SendResultType;
using server::NetworkSimulationMode;

// A terminal's own network cable. Unplugging it affects this terminal only.
class LinkSwitch final : public network::ServerClient {
public:
    explicit LinkSwitch(network::ServerClient& inner) : inner_{inner} {}

    SendResult send(const Transaction& transaction) override
    {
        if (!up_) {
            return {SendResultType::NetworkError, "terminal link is down"};
        }
        return inner_.send(transaction);
    }

    void set(bool up) noexcept { up_ = up; }
    [[nodiscard]] bool up() const noexcept { return up_; }

private:
    network::ServerClient& inner_;
    std::atomic<bool> up_{true};
};

// Remembers each delay the jitter picks, to show that terminals drift apart.
class DelayLog {
public:
    void add(std::chrono::milliseconds delay)
    {
        const std::scoped_lock lock{mutex_};
        delays_.push_back(delay);
    }

    [[nodiscard]]
    std::vector<std::chrono::milliseconds> delays() const
    {
        const std::scoped_lock lock{mutex_};
        return delays_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::chrono::milliseconds> delays_;
};

sync::RetryPolicy recordingPolicy(const FleetOptions& options, const std::shared_ptr<DelayLog>& log)
{
    return sync::RetryPolicy{
        options.retryBase,
        options.retryCap,
        [log, jitter = sync::fullJitter()](std::chrono::milliseconds ceiling) {
            const auto delay = jitter(ceiling);
            log->add(delay);
            return delay;
        },
    };
}

// One till, fully independent of the others: its own file, worker and metrics.
class Terminal {
public:
    Terminal(std::string name, const std::filesystem::path& database, std::uint16_t serverPort,
             const FleetOptions& options)
        : name_{std::move(name)}
        , repository_{database.string()}
        , http_{"127.0.0.1", serverPort, options.clientTimeout}
        , link_{http_}
        , metrics_{registry_, repository_}
        , delays_{std::make_shared<DelayLog>()}
        , engine_{repository_, link_, recordingPolicy(options, delays_),
                  sync::SyncOptions{.metrics = &metrics_}}
        , service_{repository_, link_, wiredToEngine(), &metrics_}
    {
        engine_.start();
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    void ringUpSales(std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i) {
            const auto amount = Money{250 + static_cast<std::int64_t>(sales_.size()) * 50};
            sales_.push_back(service_.createTransaction(amount));
        }
    }

    void setLink(bool up)
    {
        link_.set(up);
        if (up) {
            engine_.notifyServerReachable();
        }
    }

    [[nodiscard]] bool linkUp() const noexcept { return link_.up(); }

    // A copy of a sale this terminal holds, for the tampering step.
    [[nodiscard]] const Transaction& firstSale() const { return sales_.front(); }
    [[nodiscard]] const std::vector<Transaction>& sales() const noexcept { return sales_; }

    // Saves a transaction straight into the queue, as a corrupted till would.
    void injectIntoQueue(const Transaction& transaction)
    {
        repository_.save(transaction);
        engine_.notifyPendingTransaction();
    }

    [[nodiscard]]
    std::size_t pending()
    {
        return repository_.findPending().size();
    }

    [[nodiscard]]
    std::size_t deadLetters()
    {
        return repository_.findDeadLetters().size();
    }

    [[nodiscard]]
    TerminalReport report()
    {
        TerminalReport result;
        result.name = name_;
        result.sales = sales_.size();
        for (const auto& transaction : repository_.findAll()) {
            switch (transaction.status) {
            case TransactionStatus::Synced:     ++result.synced; break;
            case TransactionStatus::DeadLetter: ++result.deadLetters; break;
            default:                            ++result.pending; break;
            }
        }
        result.retryAttempts = metrics_.retryAttempts.value();
        result.syncFailures = metrics_.syncFailures.value();
        result.retryDelays = delays_->delays();
        return result;
    }

private:
    service::TransactionEvents wiredToEngine()
    {
        return service::TransactionEvents{
            .persisted = {},
            .completed = [this](const Transaction&, const SendResult& result) {
                if (result.successful()) {
                    engine_.notifyServerReachable();
                } else {
                    engine_.notifyPendingTransaction();
                }
            },
        };
    }

    std::string name_;
    storage::SQLiteTransactionRepository repository_;
    network::HttpServerClient http_;
    LinkSwitch link_;
    observability::MetricsRegistry registry_;
    observability::PosMetrics metrics_;
    std::shared_ptr<DelayLog> delays_;
    sync::SyncEngine engine_;
    service::TransactionService service_;
    std::vector<Transaction> sales_;  // only the cashier's thread touches this
};

// The central server, which unlike a Terminal can be stopped and brought back.
class ServerHost {
public:
    ServerHost(server::ProcessedTransactionStore& store, observability::MetricsRegistry& registry,
               unsigned int ioThreads)
        : store_{store}
    {
        options_.ioThreads = ioThreads;
        options_.metrics = &registry;
    }

    ServerHost(const ServerHost&) = delete;
    ServerHost& operator=(const ServerHost&) = delete;

    ~ServerHost() { stop(); }

    void start(std::uint16_t port, NetworkSimulationMode mode)
    {
        // The port was only just released; give the OS a moment to let go of it.
        constexpr int kBindAttempts = 50;
        for (int attempt = 1;; ++attempt) {
            try {
                server_ = std::make_unique<server::HttpTransactionServer>(
                    store_, "127.0.0.1", port, options_);
                break;
            } catch (const std::exception&) {
                if (attempt == kBindAttempts) {
                    throw;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }
        server_->setSimulationMode(mode);
        thread_ = std::jthread{[this] { server_->run(); }};
    }

    void stop()
    {
        if (server_) {
            server_->stop();
            thread_.join();
            server_.reset();
        }
    }

    void setMode(NetworkSimulationMode mode) { server_->setSimulationMode(mode); }

    [[nodiscard]] std::uint16_t port() const { return server_->port(); }

private:
    server::ProcessedTransactionStore& store_;
    server::ServerOptions options_;
    std::unique_ptr<server::HttpTransactionServer> server_;
    std::jthread thread_;
};

using Fleet = std::vector<std::unique_ptr<Terminal>>;

// Each cashier works on a thread of its own, all at the same time.
void ringUpEverywhere(Fleet& fleet, std::size_t salesEach)
{
    std::vector<std::jthread> cashiers;
    cashiers.reserve(fleet.size());
    std::mutex errorMutex;
    std::exception_ptr error;
    for (auto& terminal : fleet) {
        cashiers.emplace_back([&, till = terminal.get()] {
            try {
                till->ringUpSales(salesEach);
            } catch (...) {
                const std::scoped_lock lock{errorMutex};
                error = std::current_exception();
            }
        });
    }
    cashiers.clear();  // joins
    if (error) {
        std::rethrow_exception(error);
    }
}

template <typename Condition>
bool waitUntil(std::chrono::milliseconds timeout, Condition condition)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

std::size_t totalPending(Fleet& fleet)
{
    std::size_t pending = 0;
    for (auto& terminal : fleet) {
        pending += terminal->pending();
    }
    return pending;
}

void removePreviousRun(const std::filesystem::path& directory, std::size_t terminals)
{
    std::vector<std::string> databases{"server.db"};
    databases.reserve(terminals + 1);
    for (std::size_t i = 1; i <= terminals; ++i) {
        databases.push_back(std::format("pos-{}.db", i));
    }
    for (const auto& database : databases) {
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::error_code ignored;
            std::filesystem::remove(directory / (database + suffix), ignored);
        }
    }
}

class Narrator {
public:
    explicit Narrator(std::function<void(std::string_view)> print) : print_{std::move(print)} {}

    void line(std::string_view text = {}) const
    {
        if (print_) {
            print_(text);
        }
    }

    void phase(int number, std::string_view title) const
    {
        line();
        line(std::format("=== {}. {} ===", number, title));
    }

    void status(Fleet& fleet, std::size_t serverStored, std::string_view serverState) const
    {
        line();
        line(std::format("{:<10}{:<7}{:>7}{:>7}{:>9}{:>9}{:>10}",
                         "TERMINAL", "LINK", "SALES", "SYNCED", "PENDING", "DEAD", "RETRIES"));
        for (auto& terminal : fleet) {
            const auto report = terminal->report();
            line(std::format("{:<10}{:<7}{:>7}{:>7}{:>9}{:>9}{:>10}",
                             report.name, terminal->linkUp() ? "up" : "DOWN", report.sales,
                             report.synced, report.pending, report.deadLetters,
                             report.retryAttempts));
        }
        line(std::format("{:<10}{:<7}stored {}", "SERVER", serverState, serverStored));
    }

private:
    std::function<void(std::string_view)> print_;
};

std::string describeDelays(const std::vector<std::chrono::milliseconds>& delays)
{
    constexpr std::size_t kShown = 8;
    std::string text;
    for (std::size_t i = 0; i < std::min(delays.size(), kShown); ++i) {
        text += std::format("{}ms ", delays[i].count());
    }
    if (delays.size() > kShown) {
        text += std::format("... ({} in all)", delays.size());
    }
    return text.empty() ? "none" : text;
}

} // namespace

bool FleetReport::passed() const noexcept
{
    const bool nothingLeft = std::ranges::all_of(terminals, [](const TerminalReport& terminal) {
        return terminal.pending == 0;
    });
    std::size_t deadLetters = 0;
    for (const auto& terminal : terminals) {
        deadLetters += terminal.deadLetters;
    }

    return queuesDrained
        && nothingLeft
        && isolationHeld
        && uniqueIds == salesRungUp
        && serverStored == salesRungUp
        && missingOnServer == 0
        && wrongAmountOnServer == 0
        && tillTotalCents == serverTotalCents
        && duplicatesAbsorbed > 0
        && conflictsRefused == 1
        && tamperedAmountStored == 0
        && deadLetters == 1;
}

FleetReport runFleetSimulation(const FleetOptions& options)
{
    if (options.terminals < 2) {
        throw std::invalid_argument{"The fleet simulation needs at least 2 terminals"};
    }

    const Narrator say{options.print};
    std::filesystem::create_directories(options.dataDirectory);
    removePreviousRun(options.dataDirectory, options.terminals);

    server::ProcessedTransactionStore store{
        (options.dataDirectory / "server.db").string(), options.serverThreads};
    observability::MetricsRegistry serverRegistry;
    const server::ServerMetrics serverMetrics{serverRegistry};
    ServerHost host{store, serverRegistry, options.serverThreads};
    host.start(0, NetworkSimulationMode::Normal);
    const std::uint16_t port = host.port();

    Fleet fleet;
    fleet.reserve(options.terminals);
    for (std::size_t i = 1; i <= options.terminals; ++i) {
        fleet.push_back(std::make_unique<Terminal>(
            std::format("POS-{}", i),
            options.dataDirectory / std::format("pos-{}.db", i),
            port, options));
    }

    FleetReport report;

    say.line(std::format("{} terminals, each with its own database and sync worker.", fleet.size()));
    say.line(std::format("One central server on port {} with {} I/O threads.", port, options.serverThreads));

    say.phase(1, "Normal trading");
    ringUpEverywhere(fleet, options.salesPerPhase);
    (void)waitUntil(options.drainTimeout, [&] { return totalPending(fleet) == 0; });
    say.status(fleet, store.count(), "up");

    say.phase(2, "POS-1 loses its link, everyone else keeps trading");
    Terminal& isolated = *fleet.front();
    isolated.setLink(false);
    ringUpEverywhere(fleet, options.salesPerPhase);
    report.isolationHeld =
        waitUntil(options.drainTimeout, [&] { return totalPending(fleet) == isolated.pending(); })
        && isolated.pending() == options.salesPerPhase;
    say.status(fleet, store.count(), "up");

    say.phase(3, "The central server goes down");
    host.stop();
    ringUpEverywhere(fleet, options.salesPerPhase);
    say.line("Every terminal keeps selling and backs off on its own schedule...");
    std::this_thread::sleep_for(options.outage);
    say.status(fleet, store.count(), "DOWN");

    say.phase(4, "Server returns, but replies are lost after it commits");
    const auto acceptedBefore = serverMetrics.accepted.value();
    host.start(port, NetworkSimulationMode::DropAfterProcessing);
    isolated.setLink(true);
    (void)waitUntil(options.drainTimeout, [&] {
        return serverMetrics.accepted.value() >= acceptedBefore + fleet.size();
    });
    say.line("The server stored these sales; the terminals got no answer and still count them as pending.");
    say.status(fleet, store.count(), "lossy");

    say.phase(5, "Network healthy: the queues drain");
    host.setMode(NetworkSimulationMode::Normal);
    report.queuesDrained = waitUntil(options.drainTimeout, [&] { return totalPending(fleet) == 0; });
    say.status(fleet, store.count(), "up");

    say.phase(6, "Tampered replay: a known ID comes back with a new amount");
    Terminal& rogue = *fleet.back();
    auto tampered = fleet.front()->firstSale();
    const auto genuineAmount = tampered.amount;
    tampered.amount = Money{genuineAmount.cents() * 100};
    tampered.status = TransactionStatus::PendingSync;
    tampered.syncAttempts = 0;
    rogue.injectIntoQueue(tampered);
    (void)waitUntil(options.drainTimeout, [&] { return rogue.deadLetters() == 1; });
    say.line(std::format("{} replays {} as {} (the genuine amount is {}).",
                         rogue.name(), tampered.id.substr(0, 8),
                         core::formatMoney(tampered.amount), core::formatMoney(genuineAmount)));
    report.conflictsRefused = serverMetrics.conflicts.value();
    report.tamperedAmountStored = store.amountFor(tampered.id) == genuineAmount.cents() ? 0 : 1;
    say.line(std::format("Server: 409 IDEMPOTENCY_CONFLICT. {}: moved to DEAD_LETTER, not retried.",
                         rogue.name()));
    say.status(fleet, store.count(), "up");

    say.phase(7, "Verification");
    std::set<std::string> ids;
    report.terminals.reserve(fleet.size());
    for (auto& terminal : fleet) {
        for (const auto& sale : terminal->sales()) {
            ++report.salesRungUp;
            ids.insert(sale.id);
            report.tillTotalCents += sale.amount.cents();

            const auto stored = store.amountFor(sale.id);
            if (!stored) {
                ++report.missingOnServer;
            } else if (*stored != sale.amount.cents()) {
                ++report.wrongAmountOnServer;
            } else {
                report.serverTotalCents += *stored;
            }
        }
        report.terminals.push_back(terminal->report());
    }
    report.uniqueIds = ids.size();
    report.serverStored = store.count();
    report.duplicatesAbsorbed = serverMetrics.duplicates.value();
    report.serverMetrics = serverRegistry.renderPrometheus();

    say.line(std::format("Sales rung up on terminals:   {}", report.salesRungUp));
    say.line(std::format("Unique transaction IDs:       {}", report.uniqueIds));
    say.line(std::format("Rows on the server:           {}", report.serverStored));
    say.line(std::format("Missing on the server:        {}", report.missingOnServer));
    say.line(std::format("Wrong amount on the server:   {}", report.wrongAmountOnServer));
    say.line(std::format("Till total / server total:    {} / {}",
                         core::formatMoney(Money{report.tillTotalCents}),
                         core::formatMoney(Money{report.serverTotalCents})));
    say.line(std::format("Duplicates absorbed:          {}", report.duplicatesAbsorbed));
    say.line(std::format("Conflicts refused:            {}", report.conflictsRefused));
    say.line();
    say.line("Backoff delays drawn (full jitter, no two terminals in step):");
    for (const auto& terminal : report.terminals) {
        say.line(std::format("  {}: {}", terminal.name, describeDelays(terminal.retryDelays)));
    }
    say.line();
    say.line(report.passed() ? "PASS: every sale is on the server exactly once."
                             : "FAIL: the server does not match the terminals.");
    return report;
}

} // namespace resilientpos::simulation
