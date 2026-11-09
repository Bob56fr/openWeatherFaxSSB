// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// TODO(IMG-003): switch to real deflate compression (e.g. miniz, MIT) to shrink files.

#include <algorithm>
#include <fstream>
#include <stdexcept>

#include "owfax/image/gray_image.hpp"

namespace owfax {
namespace detail {

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc) {
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16) | a;
}

}  // namespace detail

namespace {

void put_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

void put_chunk(std::vector<std::uint8_t>& out, const char* type,
               const std::vector<std::uint8_t>& body) {
    put_be32(out, static_cast<std::uint32_t>(body.size()));
    const std::size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    put_be32(out, detail::crc32(out.data() + crc_start, out.size() - crc_start));
}

}  // namespace

void write_png(const std::string& path, const GrayImage& image) {
    if (image.width == 0 || image.height == 0) throw std::runtime_error("write_png: empty image");

    // Raw scanlines, each prefixed by filter type 0 (none).
    std::vector<std::uint8_t> raw;
    raw.reserve((image.width + 1) * image.height);
    for (std::size_t y = 0; y < image.height; ++y) {
        raw.push_back(0);
        const auto row = image.pixels.begin() + static_cast<std::ptrdiff_t>(y * image.width);
        raw.insert(raw.end(), row, row + static_cast<std::ptrdiff_t>(image.width));
    }

    // zlib stream made of stored deflate blocks (max 65535 bytes each).
    std::vector<std::uint8_t> zlib = {0x78, 0x01};
    std::size_t pos = 0;
    do {
        const std::size_t len = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = pos + len == raw.size();
        zlib.push_back(last ? 1 : 0);
        const auto len16 = static_cast<std::uint16_t>(len);
        const auto nlen16 = static_cast<std::uint16_t>(~len16);
        zlib.push_back(static_cast<std::uint8_t>(len16 & 0xFF));
        zlib.push_back(static_cast<std::uint8_t>(len16 >> 8));
        zlib.push_back(static_cast<std::uint8_t>(nlen16 & 0xFF));
        zlib.push_back(static_cast<std::uint8_t>(nlen16 >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                    raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
    } while (pos < raw.size());
    put_be32(zlib, detail::adler32(raw.data(), raw.size()));

    std::vector<std::uint8_t> ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(image.width));
    put_be32(ihdr, static_cast<std::uint32_t>(image.height));
    ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0});  // 8-bit, grayscale, deflate, no filter, no interlace

    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    put_chunk(out, "IHDR", ihdr);
    put_chunk(out, "IDAT", zlib);
    put_chunk(out, "IEND", {});

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) throw std::runtime_error("cannot write " + path);
}

}  // namespace owfax
