#include "resilientpos/core/Money.hpp"
#include "resilientpos/core/Transaction.hpp"

#include <iostream>

int main()
{
    using resilientpos::core::Money;
    using resilientpos::core::Transaction;

    std::cout << "=========== ResilientPOS ===========\n";

    const Money coffee{350};
    std::cout << "Coffee:     " << coffee << '\n';
    std::cout << "2x Coffee:  " << coffee + coffee << '\n';

    const auto sale = Transaction::create(coffee + coffee);
    std::cout << '\n'
              << "Transaction " << sale.id << '\n'
              << "  Amount:   " << sale.amount << '\n'
              << "  Status:   " << sale.status << '\n'
              << "  Attempts: " << sale.syncAttempts << '\n';

    return 0;
}
