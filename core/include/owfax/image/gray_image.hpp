// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace owfax {

/// 8-bit grayscale image, row-major, 0 = black, 255 = white.
struct GrayImage {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<std::uint8_t> pixels;

    GrayImage() = default;
    GrayImage(std::size_t w, std::size_t h, std::uint8_t fill = 255)
        : width(w), height(h), pixels(w * h, fill) {}

    std::uint8_t& at(std::size_t x, std::size_t y) { return pixels[y * width + x]; }
    std::uint8_t at(std::size_t x, std::size_t y) const { return pixels[y * width + x]; }
};

/// Writes a grayscale PNG (spec IMG-003): per-scanline PNG filters and deflate
/// compression, dependency-free. `bit_depth` 8 is lossless (the reference image);
/// 4 keeps the nearest of 16 grey levels, about 4x smaller, for display over a slow
/// link. Throws std::runtime_error on I/O failure.
void write_png(const std::string& path, const GrayImage& image, int bit_depth = 8);
/// Same PNG, in memory.
std::vector<std::uint8_t> encode_png(const GrayImage& image, int bit_depth = 8);

/// Reads an 8- or 4-bit grayscale non-interlaced PNG (as written by write_png; 4-bit
/// levels are expanded to 0, 17, ... 255). Throws std::runtime_error on I/O failure or
/// on any other PNG flavour.
GrayImage read_png(const std::string& path);
GrayImage decode_png(const std::uint8_t* data, std::size_t size);

namespace detail {
std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0);
std::uint32_t adler32(const std::uint8_t* data, std::size_t size);
}  // namespace detail

}  // namespace owfax
