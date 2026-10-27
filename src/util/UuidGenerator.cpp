#include "resilientpos/util/UuidGenerator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace resilientpos::util {

namespace {

std::mt19937_64& engine()
{
    // One engine per thread, seeded from the OS entropy source.
    thread_local std::mt19937_64 rng = [] {
        std::random_device rd;
        std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64{seed};
    }();
    return rng;
}

} // namespace

std::string UuidGenerator::generate()
{
    std::array<std::uint8_t, 16> bytes{};
    const std::uint64_t high = engine()();
    const std::uint64_t low = engine()();
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(high >> (56 - 8 * i));
        bytes[i + 8] = static_cast<std::uint8_t>(low >> (56 - 8 * i));
    }

    // RFC 9562: version 4 (random) in the high nibble of byte 6,
    // variant 0b10 in the top two bits of byte 8.
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80);

    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            result += '-';
        }
        result += hex[bytes[i] >> 4];
        result += hex[bytes[i] & 0x0F];
    }
    return result;
}

} // namespace resilientpos::util
