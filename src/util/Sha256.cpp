#include "resilientpos/util/Sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace resilientpos::util {

namespace {

constexpr std::size_t kBlockBytes = 64;

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

using State = std::array<std::uint32_t, 8>;

void compress(State& state, const unsigned char* block)
{
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        const auto* p = block + i * 4;
        w[i] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16)
             | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t i = 0; i < 64; ++i) {
        const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choice = (e & f) ^ (~e & g);
        const auto t1 = h + s1 + choice + kRoundConstants[i] + w[i];
        const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    const State mixed{a, b, c, d, e, f, g, h};
    for (std::size_t i = 0; i < state.size(); ++i) {
        state[i] += mixed[i];
    }
}

} // namespace

std::string sha256Hex(std::string_view data)
{
    State state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t offset = 0;
    for (; data.size() - offset >= kBlockBytes; offset += kBlockBytes) {
        compress(state, bytes + offset);
    }

    // Padding: the leftover bytes, 0x80, zeros, then the bit length, big-endian.
    // Needs a second block when fewer than 9 bytes are free in the first.
    std::array<unsigned char, kBlockBytes * 2> tail{};
    const std::size_t leftover = data.size() - offset;
    std::copy_n(bytes + offset, leftover, tail.begin());
    tail[leftover] = 0x80;
    const std::size_t tailBytes = leftover + 9 <= kBlockBytes ? kBlockBytes : kBlockBytes * 2;

    const std::uint64_t bitLength = std::uint64_t{data.size()} * 8;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tailBytes - 1 - i] = static_cast<unsigned char>(bitLength >> (i * 8));
    }
    for (std::size_t block = 0; block < tailBytes; block += kBlockBytes) {
        compress(state, tail.data() + block);
    }

    constexpr std::string_view kHex = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (const auto word : state) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            hex.push_back(kHex[(word >> shift) & 0xF]);
        }
    }
    return hex;
}

} // namespace resilientpos::util
