#include "resilientpos/core/Money.hpp"
#include "resilientpos/core/Transaction.hpp"
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
using resilientpos::core::Transaction;
using resilientpos::core::TransactionStatus;
using resilientpos::storage::SQLiteTransactionRepository;
using resilientpos::storage::TransactionRepository;

namespace {

constexpr std::string_view kDefaultDatabase = "data/resilientpos.db";

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

void newTransaction(TransactionRepository& repository)
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
              << "Total: " << total << "\n\n";

    auto transaction = Transaction::create(total);

    // Persist first, then mark it ready for sync. If the till dies between the
    // two calls the sale is still on disk (as CREATED) rather than lost.
    repository.save(transaction);
    repository.updateStatus(transaction.id, TransactionStatus::PendingSync);
    transaction.status = TransactionStatus::PendingSync;

    std::cout << "Transaction created:\n"
              << transaction.id << "\n\n"
              << "Status: " << transaction.status << '\n';
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

    std::cout << pending.size() << " transaction(s) waiting to sync:\n\n";
    Money total{0};
    for (const auto& tx : pending) {
        std::cout << "  " << tx.id
                  << "  " << formatUtc(tx.createdAt)
                  << "  " << tx.amount
                  << "  " << tx.status << '\n';
        total += tx.amount;
    }
    std::cout << "\nPending total: " << total << '\n';
}

} // namespace

int main(int argc, char* argv[])
{
    const std::filesystem::path databasePath =
        argc > 1 ? std::filesystem::path{argv[1]} : std::filesystem::path{kDefaultDatabase};

    std::cout << "=========== ResilientPOS ===========\n";

    try {
        if (databasePath.has_parent_path()) {
            std::filesystem::create_directories(databasePath.parent_path());
        }
        SQLiteTransactionRepository repository{databasePath.string()};
        std::cout << "Database: " << databasePath.string() << '\n';

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
                    newTransaction(repository);
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
