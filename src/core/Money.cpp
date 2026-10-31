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

Money operator*(const Money& unitPrice, std::int64_t quantity)
{
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();

    const std::int64_t a = unitPrice.cents();
    const std::int64_t b = quantity;
    const bool overflows =
        (a > 0 && b > 0 && a > max / b) ||
        (a > 0 && b < 0 && b < min / a) ||
        (a < 0 && b > 0 && a < min / b) ||
        (a < 0 && b < 0 && a < max / b);
    if (overflows) {
        throw std::overflow_error{"Money multiplication overflow"};
    }

    return Money{a * b};
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

std::optional<Money> parseMoney(std::string_view text)
{
    constexpr std::string_view kEuroSign = "\xE2\x82\xAC"; // € in UTF-8
    if (text.starts_with(kEuroSign)) {
        text.remove_prefix(kEuroSign.size());
    }
    if (text.empty()) {
        return std::nullopt;
    }

    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    std::int64_t euros = 0;
    std::size_t i = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
        const int digit = text[i] - '0';
        if (euros > (max - digit) / 10) {
            return std::nullopt;
        }
        euros = euros * 10 + digit;
    }
    if (i == 0) {
        return std::nullopt;  // no whole-euro digits (".50" is not accepted)
    }

    std::int64_t cents = 0;
    if (i < text.size()) {
        if (text[i] != '.' && text[i] != ',') {
            return std::nullopt;
        }
        const std::string_view decimals = text.substr(i + 1);
        if (decimals.empty() || decimals.size() > 2) {
            return std::nullopt;
        }
        for (const char c : decimals) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
        }
        cents = (decimals[0] - '0') * 10 + (decimals.size() == 2 ? decimals[1] - '0' : 0);
    }

    if (euros > (max - cents) / 100) {
        return std::nullopt;
    }
    return Money{euros * 100 + cents};
}

std::ostream& operator<<(std::ostream& os, const Money& money)
{
    return os << formatMoney(money);
}

} // namespace resilientpos::core
