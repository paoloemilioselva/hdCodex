#include "hdcodex/core/hash.h"

#include <array>
#include <bit>
#include <iomanip>
#include <sstream>
#include <vector>

namespace hdcodex {
namespace {

constexpr std::array<std::uint32_t, 64> K = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

[[nodiscard]] constexpr std::uint32_t Choose(
    std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
{
    return (x & y) ^ (~x & z);
}

[[nodiscard]] constexpr std::uint32_t Majority(
    std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
{
    return (x & y) ^ (x & z) ^ (y & z);
}

} // namespace

std::string Sha256(std::span<const std::byte> bytes)
{
    const std::uint64_t bitLength = static_cast<std::uint64_t>(bytes.size()) * 8U;
    std::vector<std::uint8_t> message;
    message.reserve(((bytes.size() + 9U + 63U) / 64U) * 64U);
    for (const std::byte byte : bytes) {
        message.push_back(std::to_integer<std::uint8_t>(byte));
    }
    message.push_back(0x80U);
    while ((message.size() % 64U) != 56U) {
        message.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<std::uint8_t>(bitLength >> shift));
    }

    std::array<std::uint32_t, 8> hash = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };

    for (std::size_t block = 0; block < message.size(); block += 64U) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16U; ++i) {
            const std::size_t offset = block + i * 4U;
            words[i] = (static_cast<std::uint32_t>(message[offset]) << 24U)
                | (static_cast<std::uint32_t>(message[offset + 1U]) << 16U)
                | (static_cast<std::uint32_t>(message[offset + 2U]) << 8U)
                | static_cast<std::uint32_t>(message[offset + 3U]);
        }
        for (std::size_t i = 16U; i < words.size(); ++i) {
            const std::uint32_t s0 = std::rotr(words[i - 15U], 7)
                ^ std::rotr(words[i - 15U], 18) ^ (words[i - 15U] >> 3U);
            const std::uint32_t s1 = std::rotr(words[i - 2U], 17)
                ^ std::rotr(words[i - 2U], 19) ^ (words[i - 2U] >> 10U);
            words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
        }

        auto [a, b, c, d, e, f, g, h] = hash;
        for (std::size_t i = 0; i < words.size(); ++i) {
            const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11)
                ^ std::rotr(e, 25);
            const std::uint32_t temp1 = h + sum1 + Choose(e, f, g) + K[i] + words[i];
            const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13)
                ^ std::rotr(a, 22);
            const std::uint32_t temp2 = sum0 + Majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const std::uint32_t value : hash) {
        output << std::setw(8) << value;
    }
    return output.str();
}

std::string Sha256(std::string_view text)
{
    return Sha256(std::as_bytes(std::span{text.data(), text.size()}));
}

} // namespace hdcodex

