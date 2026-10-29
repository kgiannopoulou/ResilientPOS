#include "resilientpos/core/Money.hpp"
#include "resilientpos/core/Transaction.hpp"
#include "resilientpos/network/HttpServerClient.hpp"
#include "resilientpos/service/TransactionService.hpp"
#include "resilientpos/storage/SQLiteTransactionRepository.hpp"
#include "resilientpos/storage/TransactionRepository.hpp"

#include <charconv>
#include <chrono>
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

using resilientpos::core::Money;
using resilientpos::core::TransactionStatus;
using resilientpos::network::HttpServerClient;
using resilientpos::network::SendResultType;
using resilientpos::service::TransactionService;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::storage::TransactionRepository;

namespace {

constexpr std::string_view kDefaultDatabase = "data/pos.db";
constexpr std::string_view kDefaultServerHost = "127.0.0.1";
constexpr std::uint16_t kDefaultServerPort = 8080;

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

void newTransaction(TransactionService& service)
{
    auto product = readLine("Product: ");
    if (!product) {
        return;
    }
    if (product->empty()) {
        product = "Item";
    }

    const auto priceCents = readInteger("Price cents: ", 1, 100'000'000);
    if (!priceCents) {
        return;
    }
    const auto quantity = readInteger("Quantity: ", 1, 10'000);
    if (!quantity) {
        return;
    }

    const Money total = Money{*priceCents} * *quantity;
    std::cout << '\n'
              << *quantity << " x " << *product << " @ " << Money{*priceCents} << '\n'
              << "Total: " << total << "\n\n"
              << "Processing transaction...\n\n";

    // Saved to SQLite first, then sent. Only a storage failure can throw here.
    const auto transaction = service.createTransaction(total);
    const auto& result = service.lastSendResult();

    switch (result.type) {
    case SendResultType::Accepted:
        std::cout << "[NETWORK] Connected\n"
                  << "[SERVER] Transaction accepted\n";
        break;
    case SendResultType::AlreadyProcessed:
        std::cout << "[NETWORK] Connected\n"
                  << "[SERVER] Transaction already processed\n";
        break;
    case SendResultType::ServerError:
        std::cout << "[NETWORK] Connected\n"
                  << "[SERVER] Error: " << result.message << "\n\n"
                  << "Transaction saved locally.\n";
        break;
    case SendResultType::NetworkError:
        std::cout << "[NETWORK] Server unavailable (" << result.message << ")\n\n"
                  << "Transaction saved locally.\n";
        break;
    }

    std::cout << "\nTransaction: " << transaction.id << '\n'
              << "Status: " << transaction.status << '\n';
    if (transaction.status == TransactionStatus::PendingSync) {
        std::cout << "\nPOS CAN CONTINUE OPERATING\n";
    }
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

void listPending(TransactionRepository& repository)
{
    const auto pending = repository.findPending();
    if (pending.empty()) {
        std::cout << "No transactions waiting to sync.\n";
        return;
    }

    std::cout << padRight("ID", 12) << padRight("AMOUNT", 13) << padRight("STATUS", 15)
              << "CREATED\n"
              << std::string(62, '-') << '\n';
    Money total{0};
    for (const auto& tx : pending) {
        std::cout << padRight(tx.id.substr(0, 8), 12)
                  << padRight(resilientpos::core::formatMoney(tx.amount), 13)
                  << padRight(std::string{resilientpos::core::toString(tx.status)}, 15)
                  << formatUtc(tx.createdAt) << '\n';
        total += tx.amount;
    }
    std::cout << "\nPending synchronization: " << pending.size() << '\n'
              << "Pending total: " << total << '\n';
}

std::uint16_t parsePort(std::string_view text)
{
    std::uint16_t port{};
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, port);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument{"Invalid server port: " + std::string{text}};
    }
    return port;
}

} // namespace

// Usage: resilient-pos [database] [server-host] [server-port]
int main(int argc, char* argv[])
{
    std::cout << "=========== ResilientPOS ===========\n";

    try {
        const std::filesystem::path databasePath =
            argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::path{kDefaultDatabase};
        const std::string serverHost = argc > 2 ? argv[2] : std::string{kDefaultServerHost};
        const std::uint16_t serverPort = argc > 3 ? parsePort(argv[3]) : kDefaultServerPort;

        if (databasePath.has_parent_path()) {
            std::filesystem::create_directories(databasePath.parent_path());
        }
        SQLiteTransactionRepository repository{databasePath.string()};
        HttpServerClient serverClient{serverHost, serverPort};
        TransactionService service{repository, serverClient};

        std::cout << "Database: " << databasePath.string() << '\n'
                  << "Server:   http://" << serverHost << ':' << serverPort << '\n';

        while (true) {
            std::cout << "\n"
                         "1. New transaction\n"
                         "2. List pending\n"
                         "3. Exit\n\n";

            const auto choice = readLine("Choice: ");
            if (!choice || *choice == "3") {
                break;
            }
            std::cout << '\n';

            try {
                if (*choice == "1") {
                    newTransaction(service);
                } else if (*choice == "2") {
                    listPending(repository);
                } else {
                    std::cout << "Unknown option '" << *choice << "'.\n";
                }
            } catch (const std::overflow_error& e) {
                std::cout << "Amount too large: " << e.what() << '\n';
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 1;
    }

    std::cout << "Bye.\n";
    return 0;
}
