// ResilientPOS till: wires the objects together and runs the terminal UI.
// No business logic lives here; see TransactionService and SyncEngine.
//
// Terminal output (what the cashier sees) and diagnostics (what an engineer
// reads) are separate: the CLI prints to stdout, the engine logs through
// spdlog to data/pos.log. Run `tail -f data/pos.log` beside the till.

#include "resilientpos/core/Money.hpp"
#include "resilientpos/core/Transaction.hpp"
#include "resilientpos/network/HttpServerClient.hpp"
#include "resilientpos/network/MonitoredServerClient.hpp"
#include "resilientpos/observability/Metrics.hpp"
#include "resilientpos/observability/OpsHttpServer.hpp"
#include "resilientpos/observability/PosMetrics.hpp"
#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/storage/TransactionRepository.hpp"
#include "resilientpos/sync/SyncEngine.hpp"
#include "resilientpos/util/Logging.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using resilientpos::core::Money;
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::network::FailureClass;
using resilientpos::network::HttpServerClient;
using resilientpos::network::MonitoredServerClient;
using resilientpos::network::SendResult;
using resilientpos::network::SendResultType;
using resilientpos::observability::MetricsRegistry;
using resilientpos::observability::OpsEndpoints;
using resilientpos::observability::OpsHttpServer;
using resilientpos::observability::PosMetrics;
using resilientpos::observability::Readiness;
using resilientpos::service::TransactionEvents;
using resilientpos::service::TransactionService;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::storage::TransactionRepository;
using resilientpos::sync::RetryPolicy;
using resilientpos::sync::SyncEngine;
using resilientpos::sync::SyncOptions;

namespace {

constexpr std::string_view kDefaultDatabase = "data/pos.db";
constexpr std::string_view kDefaultServerHost = "127.0.0.1";
constexpr std::uint16_t kDefaultServerPort = 8080;
constexpr std::size_t kListLimit = 20;

const std::string kRule(40, '-');
const std::string kBanner(40, '=');

std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

// Prints `prompt` and reads one line. nullopt on end of input (Ctrl+D / Ctrl+Z).
std::optional<std::string> readLine(std::string_view prompt)
{
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
        return std::nullopt;
    }
    return std::string{trim(line)};
}

// Asks until the user types an integer in [min, max]. nullopt on end of input.
std::optional<std::int64_t> readInteger(std::string_view prompt, std::int64_t min, std::int64_t max)
{
    while (true) {
        const auto line = readLine(prompt);
        if (!line) {
            return std::nullopt;
        }

        std::int64_t value{};
        const auto* begin = line->data();
        const auto* end = begin + line->size();
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec == std::errc{} && ptr == end && !line->empty() && value >= min && value <= max) {
            return value;
        }
        std::cout << "  Please enter a whole number between " << min << " and " << max << ".\n";
    }
}

// Asks until the user types a positive price such as 3.50. nullopt on end of input.
std::optional<Money> readPrice(std::string_view prompt)
{
    constexpr std::int64_t kMaxPriceCents = 100'000'000;  // 1,000,000.00
    while (true) {
        const auto line = readLine(prompt);
        if (!line) {
            return std::nullopt;
        }
        if (const auto price = resilientpos::core::parseMoney(*line);
            price && price->cents() > 0 && price->cents() <= kMaxPriceCents) {
            return price;
        }
        std::cout << "  Please enter a price like 3.50 (at most two decimals).\n";
    }
}

std::string shortId(const std::string& id)
{
    return id.substr(0, 8) + "...";
}

// Pads to `width` display columns. Counts UTF-8 code points, not bytes,
// so "€5.50" lines up with plain ASCII.
std::string padRight(const std::string& text, std::size_t width)
{
    std::size_t columns = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++columns;
        }
    }
    return columns >= width ? text + ' ' : text + std::string(width - columns, ' ');
}

std::string formatUtc(std::chrono::system_clock::time_point tp)
{
    return std::format("{:%Y-%m-%d %H:%M:%S} UTC", std::chrono::floor<std::chrono::seconds>(tp));
}

// Everything main() owns, so the screens can reach it.
struct Till {
    TransactionRepository& repository;
    MonitoredServerClient& serverClient;
    TransactionService& service;
    SyncEngine& syncEngine;
    std::string serverUrl;
};

void showMainScreen(Till& till)
{
    const auto connectivity = till.serverClient.snapshot();
    std::cout << '\n'
              << kBanner << '\n'
              << "              ResilientPOS\n"
              << kBanner << "\n\n"
              << "Network: " << resilientpos::network::toString(connectivity.state) << '\n'
              << "Pending transactions: " << till.repository.findPending().size() << '\n'
              << "Dead letters: " << till.repository.findDeadLetters().size() << "\n\n"
              << "1. New transaction\n"
              << "2. List transactions\n"
              << "3. Force synchronization\n"
              << "4. Network status\n"
              << "5. Dead letter queue\n"
              << "6. Exit\n\n";
}

void newTransaction(Till& till)
{
    auto product = readLine("Product: ");
    if (!product) {
        return;
    }
    if (product->empty()) {
        product = "Item";
    }
    const auto unitPrice = readPrice("Unit price: \xE2\x82\xAC");
    if (!unitPrice) {
        return;
    }
    const auto quantity = readInteger("Quantity: ", 1, 10'000);
    if (!quantity) {
        return;
    }

    const Money total = *unitPrice * *quantity;
    std::cout << '\n'
              << kRule << '\n'
              << *quantity << " x " << *product << " @ " << *unitPrice << '\n'
              << "Total: " << total << '\n'
              << kRule << "\n\n"
              << "Processing transaction...\n\n" << std::flush;

    // Saved to SQLite first, then sent (the service's events print the
    // progress lines as it happens). Only a storage failure can throw here.
    const auto transaction = till.service.createTransaction(total);

    std::cout << "\nID:     " << shortId(transaction.id) << '\n'
              << "Status: " << transaction.status << '\n';
    if (transaction.status == TransactionStatus::PendingSync) {
        std::cout << "\nTransaction safely stored locally.\n"
                     "POS can continue operating.\n";
    } else if (transaction.status == TransactionStatus::DeadLetter) {
        std::cout << "\nThe server refused this sale, so it will not be retried.\n"
                     "It is kept locally: see 5. Dead letter queue.\n";
    }
}

// Progress lines for a sale, printed from inside createTransaction().
TransactionEvents cliEvents(SyncEngine& syncEngine)
{
    return TransactionEvents{
        .persisted = [](const Transaction&) {
            std::cout << "[LOCAL] Transaction persisted\n"
                      << "[NETWORK] Sending...\n" << std::flush;
        },
        .completed = [&syncEngine](const Transaction&, const SendResult& result) {
            switch (result.type) {
            case SendResultType::Accepted:
                std::cout << "[SERVER] Accepted\n";
                break;
            case SendResultType::AlreadyProcessed:
                std::cout << "[SERVER] Already processed\n";
                break;
            case SendResultType::ServerError:
                std::cout << "[SERVER] Error: " << result.message << '\n';
                break;
            case SendResultType::ServerUnavailable:
                std::cout << "[SERVER] Temporarily unavailable\n";
                break;
            case SendResultType::Rejected:
                std::cout << "[SERVER] Rejected: " << result.message << '\n';
                break;
            case SendResultType::NetworkError:
                std::cout << "[NETWORK] Server unavailable\n";
                break;
            case SendResultType::IdempotencyConflict:
                std::cout << "[SERVER] Conflict: this ID belongs to a different sale\n";
                break;
            }

            // Wire the sale's outcome into the background worker. A refusal is
            // still an answer: the server is evidently up.
            const auto failureClass = resilientpos::network::classify(result.type);
            if (failureClass == FailureClass::None || failureClass == FailureClass::Permanent) {
                syncEngine.notifyServerReachable();
            } else {
                syncEngine.notifyPendingTransaction();
            }
        },
    };
}

void listTransactions(TransactionRepository& repository)
{
    const auto all = repository.findAll();
    if (all.empty()) {
        std::cout << "No transactions yet.\n";
        return;
    }

    const auto first = all.size() > kListLimit ? all.size() - kListLimit : 0;
    std::cout << padRight("ID", 13) << padRight("AMOUNT", 13) << padRight("STATUS", 15)
              << padRight("TRIES", 7) << "CREATED\n"
              << std::string(72, '-') << '\n';
    std::size_t pending = 0;
    std::size_t deadLetters = 0;
    Money pendingTotal{0};
    for (std::size_t i = first; i < all.size(); ++i) {
        const auto& tx = all[i];
        std::cout << padRight(shortId(tx.id), 13)
                  << padRight(resilientpos::core::formatMoney(tx.amount), 13)
                  << padRight(std::string{resilientpos::core::toString(tx.status)}, 15)
                  << padRight(std::to_string(tx.syncAttempts), 7)
                  << formatUtc(tx.createdAt) << '\n';
    }
    for (const auto& tx : all) {
        if (tx.status == TransactionStatus::DeadLetter) {
            ++deadLetters;
        } else if (tx.status != TransactionStatus::Synced) {
            ++pending;
            pendingTotal += tx.amount;
        }
    }
    if (first > 0) {
        std::cout << "(showing the latest " << kListLimit << " of " << all.size() << ")\n";
    }
    std::cout << "\nTotal transactions: " << all.size() << '\n'
              << "Waiting to sync:    " << pending << " (" << pendingTotal << ")\n"
              << "Dead letters:       " << deadLetters << '\n';
}

void forceSynchronization(Till& till)
{
    std::cout << "[SYNC] Starting synchronization...\n\n" << std::flush;
    const auto report = till.syncEngine.syncPendingTransactions();

    for (const auto& outcome : report.outcomes) {
        std::cout << "[SYNC] " << shortId(outcome.transactionId) << " -> ";
        if (outcome.deadLettered) {
            std::cout << "DEAD_LETTER (" << outcome.message << ")\n";
            continue;
        }
        switch (outcome.result) {
        case SendResultType::Accepted:
            std::cout << "SYNCED\n";
            break;
        case SendResultType::AlreadyProcessed:
            std::cout << "ALREADY_PROCESSED -> SYNCED (server had it; no duplicate)\n";
            break;
        case SendResultType::NetworkError:
        case SendResultType::ServerUnavailable:
        case SendResultType::ServerError:
        case SendResultType::Rejected:
        case SendResultType::IdempotencyConflict:
            std::cout << "FAILED (" << outcome.message << "), still PENDING_SYNC\n";
            break;
        }
    }
    if (!report.outcomes.empty()) {
        std::cout << '\n';
    }
    std::cout << report.synced << " transaction(s) synchronized.\n"
              << report.stillPending << " transaction(s) pending.\n";
    if (report.deadLettered > 0) {
        std::cout << report.deadLettered << " transaction(s) moved to the dead letter queue.\n";
    }
}

void printDeadLetter(std::size_t number, const Transaction& tx)
{
    std::cout << padRight(std::to_string(number), 4) << padRight(shortId(tx.id), 13)
              << padRight(resilientpos::core::formatMoney(tx.amount), 13)
              << padRight(tx.lastFailure ? tx.lastFailure->code : "UNKNOWN", 22)
              << padRight(std::to_string(tx.syncAttempts), 9)
              << (tx.lastFailure ? formatUtc(tx.lastFailure->at) : "-") << '\n';
    if (tx.lastFailure) {
        std::cout << "    Reason: " << tx.lastFailure->reason << '\n';
    }
}

// Inspect what was given up on, then requeue one, all, or none of it.
void showDeadLetterQueue(Till& till)
{
    const auto deadLetters = till.syncEngine.deadLetters();
    if (deadLetters.empty()) {
        std::cout << "The dead letter queue is empty.\n";
        return;
    }

    std::cout << deadLetters.size() << " transaction(s) need attention. They are kept "
                 "locally and are not being retried.\n\n"
              << padRight("#", 4) << padRight("ID", 13) << padRight("AMOUNT", 13)
              << padRight("FAILURE", 22) << padRight("RESENDS", 9) << "LAST FAILURE\n"
              << std::string(86, '-') << '\n';
    for (std::size_t i = 0; i < deadLetters.size(); ++i) {
        printDeadLetter(i + 1, deadLetters[i]);
    }

    const auto choice = readLine("\nNumber to retry, 'a' to retry all, Enter to go back: ");
    if (!choice || choice->empty()) {
        return;
    }
    if (*choice == "a") {
        std::cout << till.syncEngine.retryAllDeadLetters() << " transaction(s) requeued.\n";
        return;
    }

    std::size_t number{};
    const auto* begin = choice->data();
    const auto* end = begin + choice->size();
    const auto [ptr, ec] = std::from_chars(begin, end, number);
    if (ec != std::errc{} || ptr != end || number < 1 || number > deadLetters.size()) {
        std::cout << "No dead letter number '" << *choice << "'.\n";
        return;
    }
    const auto& transaction = deadLetters[number - 1];
    if (till.syncEngine.retryDeadLetter(transaction.id)) {
        std::cout << shortId(transaction.id) << " requeued as PENDING_SYNC.\n";
    }
}

void showNetworkStatus(Till& till)
{
    const auto connectivity = till.serverClient.snapshot();
    const auto worker = till.syncEngine.workerStatus();

    std::cout << "Server:        " << till.serverUrl << '\n'
              << "Network:       " << resilientpos::network::toString(connectivity.state) << '\n';
    if (connectivity.lastResult) {
        std::cout << "Last result:   " << resilientpos::network::toString(connectivity.lastResult->type)
                  << " (" << connectivity.lastResult->message << ")\n";
    }
    std::cout << "Last contact:  "
              << (connectivity.lastContactAt ? formatUtc(*connectivity.lastContactAt) : "never") << '\n'
              << "Sends:         " << connectivity.sends << " (" << connectivity.failures << " failed)\n\n"
              << "Sync worker:   " << (worker.running ? "running" : "stopped") << '\n';
    if (worker.nextRetryAt) {
        const auto wait = std::chrono::ceil<std::chrono::seconds>(
            *worker.nextRetryAt - std::chrono::steady_clock::now());
        std::cout << "Backing off:   retry #" << worker.consecutiveFailures + 1 << " in "
                  << std::max<std::int64_t>(wait.count(), 0) << "s\n";
    } else if (worker.running) {
        std::cout << "Waiting for:   new pending transactions (idle, not polling)\n";
    }
    std::cout << "Pending:       " << till.repository.findPending().size() << '\n';
}

std::uint16_t parsePort(std::string_view text)
{
    std::uint16_t port{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, port);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument{"Invalid server port: " + std::string{text}};
    }
    return port;
}

// Diagnostics go to a file beside the database, never into the till's screen.
void setUpLogging(const std::filesystem::path& databasePath)
{
    auto logPath = databasePath;
    logPath.replace_extension(".log");
    resilientpos::util::installDefaultLogger(spdlog::basic_logger_mt("pos", logPath.string()));
}

} // namespace

// Being offline is normal for a till, so it does not make it unready: all it
// needs to take a sale is its own database.
OpsEndpoints tillEndpoints(MetricsRegistry& registry, TransactionRepository& repository,
                           MonitoredServerClient& serverClient)
{
    return OpsEndpoints{
        .metrics = [&registry] { return registry.renderPrometheus(); },
        .readiness = [&repository, &serverClient] {
            const auto pending = repository.findPending().size();
            return Readiness{true, {
                {"database", "ok"},
                {"pending", std::to_string(pending)},
                {"network", std::string{resilientpos::network::toString(serverClient.snapshot().state)}},
            }};
        },
    };
}

// Usage: resilient-pos [--metrics-port N] [database] [server-host] [server-port]
int main(int argc, char* argv[])
{
    try {
        std::optional<std::uint16_t> metricsPort;
        std::vector<std::string_view> positional;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg{argv[i]};
            if (arg == "--metrics-port") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument{"--metrics-port needs a value"};
                }
                metricsPort = parsePort(argv[++i]);
            } else {
                positional.push_back(arg);
            }
        }

        const std::filesystem::path databasePath =
            !positional.empty() ? std::filesystem::path{positional[0]}
                                : std::filesystem::path{kDefaultDatabase};
        const std::string serverHost =
            positional.size() > 1 ? std::string{positional[1]} : std::string{kDefaultServerHost};
        const std::uint16_t serverPort =
            positional.size() > 2 ? parsePort(positional[2]) : kDefaultServerPort;

        if (databasePath.has_parent_path()) {
            std::filesystem::create_directories(databasePath.parent_path());
        }
        setUpLogging(databasePath);
        spdlog::info("POS starting database={} server={}:{}", databasePath.string(), serverHost, serverPort);

        // Declaration order is shutdown order, reversed: the sync worker
        // stops and joins first, and only then do the client and SQLite go.
        SQLiteTransactionRepository repository{databasePath.string()};
        HttpServerClient httpClient{serverHost, serverPort};
        MonitoredServerClient serverClient{httpClient};
        MetricsRegistry registry;
        PosMetrics metrics{registry, repository};
        SyncEngine syncEngine{repository, serverClient, RetryPolicy{},
                              SyncOptions{.metrics = &metrics}};  // recovers interrupted sends
        TransactionService service{repository, serverClient, cliEvents(syncEngine), &metrics};
        syncEngine.start();

        std::optional<OpsHttpServer> opsServer;
        if (metricsPort) {
            opsServer.emplace(tillEndpoints(registry, repository, serverClient),
                              "127.0.0.1", *metricsPort);
        }

        Till till{repository, serverClient, service, syncEngine,
                  "http://" + serverHost + ":" + std::to_string(serverPort)};

        std::cout << "Database: " << databasePath.string() << '\n'
                  << "Server:   " << till.serverUrl << '\n';
        if (opsServer) {
            std::cout << "Metrics:  http://127.0.0.1:" << opsServer->port() << "/metrics\n";
        }

        while (true) {
            showMainScreen(till);
            const auto choice = readLine("> ");
            if (!choice || *choice == "6") {
                break;
            }
            if (choice->empty()) {
                continue;  // just redraw the main screen
            }
            std::cout << '\n';

            try {
                if (*choice == "1") {
                    newTransaction(till);
                } else if (*choice == "2") {
                    listTransactions(repository);
                } else if (*choice == "3") {
                    forceSynchronization(till);
                } else if (*choice == "4") {
                    showNetworkStatus(till);
                } else if (*choice == "5") {
                    showDeadLetterQueue(till);
                } else {
                    std::cout << "Unknown option '" << *choice << "'.\n";
                }
            } catch (const std::overflow_error& e) {
                std::cout << "Amount too large: " << e.what() << '\n';
            }
        }

        std::cout << "\nStopping sync worker..." << std::flush;
        syncEngine.stop();
        std::cout << " done.\n";
        spdlog::info("POS stopped pending={}", repository.findPending().size());
    } catch (const std::exception& e) {
        spdlog::critical("Fatal: {}", e.what());
        std::cerr << "Fatal: " << e.what() << '\n';
        return 1;
    }

    std::cout << "Bye.\n";
    return 0;
}
