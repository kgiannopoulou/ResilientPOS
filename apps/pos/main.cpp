#include "resilientpos/core/Money.hpp"

#include <iostream>

int main()
{
    using resilientpos::core::Money;

    std::cout << "=========== ResilientPOS ===========\n";

    const Money coffee{350};
    std::cout << "Coffee:     " << coffee << '\n';
    std::cout << "2x Coffee:  " << coffee + coffee << '\n';

    return 0;
}
