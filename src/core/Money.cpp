#include "resilientpos/core/Money.hpp"

#include <limits>
#include <ostream>
#include <stdexcept>

namespace resilientpos::core {

Money::Money(std::int64_t cents)
    : cents_{cents}
{
}

std::int64_t Money::cents() const noexcept
{
    return cents_;
}

Money& Money::operator+=(const Money& other)
{
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();

    if ((other.cents_ > 0 && cents_ > max - other.cents_) ||
        (other.cents_ < 0 && cents_ < min - other.cents_)) {
        throw std::overflow_error{"Money addition overflow"};
    }

    cents_ += other.cents_;
    return *this;
}

Money operator+(Money lhs, const Money& rhs)
{
    lhs += rhs;
    return lhs;
}

bool operator==(const Money& lhs, const Money& rhs)
{
    return lhs.cents() == rhs.cents();
}

std::string formatMoney(const Money& money)
{
    const std::int64_t cents = money.cents();

    // Work on the unsigned magnitude so INT64_MIN does not overflow on negation.
    const std::uint64_t magnitude = cents < 0
        ? std::uint64_t{0} - static_cast<std::uint64_t>(cents)
        : static_cast<std::uint64_t>(cents);

    const std::uint64_t euros = magnitude / 100;
    const std::uint64_t remainder = magnitude % 100;

    std::string result;
    if (cents < 0) {
        result += '-';
    }
    result += "\xE2\x82\xAC"; // € in UTF-8
    result += std::to_string(euros);
    result += '.';
    if (remainder < 10) {
        result += '0';
    }
    result += std::to_string(remainder);
    return result;
}

std::ostream& operator<<(std::ostream& os, const Money& money)
{
    return os << formatMoney(money);
}

} // namespace resilientpos::core
