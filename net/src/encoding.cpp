// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/net/encoding.hpp"

#include <vector>

namespace owfax::net {
namespace {

std::uint32_t rotl(std::uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::array<std::uint8_t, 20> sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    std::vector<std::uint8_t> msg(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8U;
    msg.push_back(0x80U);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));

    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::uint32_t w[80];
        for (std::size_t i = 0; i < 16; ++i) {
            const std::uint8_t* p = &msg[chunk + 4 * i];
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) |
                   (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            out[4 * i + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
        }
    }
    return out;
}

std::string base64_encode(const std::uint8_t* data, std::size_t size) {
    static const char* const kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < size; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += kAlphabet[(v >> 18) & 63U];
        out += kAlphabet[(v >> 12) & 63U];
        out += kAlphabet[(v >> 6) & 63U];
        out += kAlphabet[v & 63U];
    }
    if (i < size) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < size) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        out += kAlphabet[(v >> 18) & 63U];
        out += kAlphabet[(v >> 12) & 63U];
        out += i + 1 < size ? kAlphabet[(v >> 6) & 63U] : '=';
        out += '=';
    }
    return out;
}

std::string base64_encode(std::string_view data) {
    return base64_encode(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::string websocket_accept(std::string_view key) {
    std::string s(key);
    s += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto digest = sha1(s);
    return base64_encode(digest.data(), digest.size());
}

std::optional<std::string> url_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) return std::nullopt;
            const int hi = hex_value(in[i + 1]);
            const int lo = hex_value(in[i + 2]);
            if (hi < 0 || lo < 0) return std::nullopt;
            c = static_cast<char>(hi * 16 + lo);
            i += 2;
        }
        if (c == '\0') return std::nullopt;
        out += c;
    }
    return out;
}

}  // namespace owfax::net
