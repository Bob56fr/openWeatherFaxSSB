// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/net/crypto.hpp"

#include <cstdio>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
// RtlGenRandom (exported as SystemFunction036 by advapi32), available since XP:
// the documented way to get system randomness without CryptoAPI contexts.
extern "C" BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);
#endif

namespace owfax::net {
namespace {

constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

std::uint32_t rotr(std::uint32_t v, int n) {
    return (v >> n) | (v << (32 - n));
}

void compress(std::uint32_t h[8], const std::uint8_t* block) {
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) |
               (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
               (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) |
               static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = hh + s1 + ch + kK[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

}  // namespace

std::array<std::uint8_t, 32> sha256(std::string_view data) {
    std::uint32_t h[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                          0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t n = data.size();
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) compress(h, p + i);
    std::uint8_t tail[128] = {};
    const std::size_t rest = n - i;
    for (std::size_t k = 0; k < rest; ++k) tail[k] = p[i + k];
    tail[rest] = 0x80;
    const std::size_t total = rest + 9 <= 64 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(n) * 8U;
    for (int k = 0; k < 8; ++k) {
        tail[total - 1 - static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(bits >> (8 * k));
    }
    compress(h, tail);
    if (total == 128) compress(h, tail + 64);
    std::array<std::uint8_t, 32> out{};
    for (std::size_t k = 0; k < 8; ++k) {
        for (std::size_t j = 0; j < 4; ++j) {
            out[4 * k + j] = static_cast<std::uint8_t>(h[k] >> (24 - 8 * j));
        }
    }
    return out;
}

std::array<std::uint8_t, 32> hmac_sha256(std::string_view key, std::string_view data) {
    std::uint8_t k[64] = {};
    if (key.size() > 64) {
        const auto d = sha256(key);
        for (std::size_t i = 0; i < 32; ++i) k[i] = d[i];
    } else {
        for (std::size_t i = 0; i < key.size(); ++i) k[i] = static_cast<std::uint8_t>(key[i]);
    }
    std::string inner(64, '\0');
    std::string outer(64, '\0');
    for (std::size_t i = 0; i < 64; ++i) {
        inner[i] = static_cast<char>(k[i] ^ 0x36U);
        outer[i] = static_cast<char>(k[i] ^ 0x5cU);
    }
    inner.append(data.data(), data.size());
    const auto ih = sha256(inner);
    outer.append(reinterpret_cast<const char*>(ih.data()), ih.size());
    return sha256(outer);
}

std::vector<std::uint8_t> pbkdf2_sha256(std::string_view password, std::string_view salt,
                                        std::uint32_t iterations, std::size_t length) {
    if (iterations == 0) throw std::invalid_argument("pbkdf2: zero iterations");
    std::vector<std::uint8_t> out;
    for (std::uint32_t block = 1; out.size() < length; ++block) {
        std::string s(salt);
        for (int sh = 24; sh >= 0; sh -= 8) s += static_cast<char>((block >> sh) & 0xFFU);
        auto u = hmac_sha256(password, s);
        auto t = u;
        for (std::uint32_t it = 1; it < iterations; ++it) {
            u = hmac_sha256(password,
                            std::string_view(reinterpret_cast<const char*>(u.data()), u.size()));
            for (std::size_t i = 0; i < t.size(); ++i) t[i] ^= u[i];
        }
        for (std::size_t i = 0; i < t.size() && out.size() < length; ++i) out.push_back(t[i]);
    }
    return out;
}

std::vector<std::uint8_t> secure_random(std::size_t count) {
    std::vector<std::uint8_t> out(count);
    if (count == 0) return out;
#if defined(_WIN32)
    if (!SystemFunction036(out.data(), static_cast<ULONG>(count))) {
        throw std::runtime_error("system random source unavailable");
    }
#else
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (f == nullptr) throw std::runtime_error("cannot open /dev/urandom");
    const std::size_t n = std::fread(out.data(), 1, count, f);
    std::fclose(f);
    if (n != count) throw std::runtime_error("short read from /dev/urandom");
#endif
    return out;
}

std::string to_hex(const std::uint8_t* data, std::size_t size) {
    static const char* const k = "0123456789abcdef";
    std::string s;
    s.reserve(2 * size);
    for (std::size_t i = 0; i < size; ++i) {
        s += k[data[i] >> 4];
        s += k[data[i] & 15U];
    }
    return s;
}

std::vector<std::uint8_t> from_hex(std::string_view hex) {
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = val(hex[i]);
        const int lo = val(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
    }
    return out;
}

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
    unsigned diff = a.size() == b.size() ? 0U : 1U;
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^
                                      static_cast<unsigned char>(b[i]));
    }
    return diff == 0;
}

}  // namespace owfax::net
