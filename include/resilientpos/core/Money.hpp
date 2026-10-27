#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace resilientpos::core {

// Monetary amount stored as an integer number of euro cents.
// Never use floating point for money: 0.1 + 0.2 != 0.3.
class Money {
public:
    explicit Money(std::int64_t cents);

    [[nodiscard]]
    std::int64_t cents() const noexcept;

    // Throws std::overflow_error if the result does not fit in 64 bits.
    Money& operator+=(const Money& other);

private:
    std::int64_t cents_;
};

[[nodiscard]]
Money operator+(Money lhs, const Money& rhs);

[[nodiscard]]
bool operator==(const Money& lhs, const Money& rhs);

// Money{350} -> "€3.50", Money{-5} -> "-€0.05"
[[nodiscard]]
std::string formatMoney(const Money& money);

std::ostream& operator<<(std::ostream& os, const Money& money);

} // namespace resilientpos::core
