// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/image/gray_image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace owfax {

GrayImage shrink_to_width(const GrayImage& image, std::size_t width) {
    if (width == 0 || width >= image.width || image.height == 0) return image;
    const double scale = static_cast<double>(image.width) / static_cast<double>(width);
    const auto height = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::lround(static_cast<double>(image.height) / scale)));
    GrayImage out(width, height, 0);
    // Source area of each output pixel: [x * scale, (x + 1) * scale), same vertically.
    std::vector<std::size_t> x0(width + 1);
    for (std::size_t x = 0; x <= width; ++x) {
        x0[x] = std::min(image.width,
                         static_cast<std::size_t>(std::lround(static_cast<double>(x) * scale)));
    }
    const double yscale = static_cast<double>(image.height) / static_cast<double>(height);
    for (std::size_t y = 0; y < height; ++y) {
        const auto ya = static_cast<std::size_t>(std::lround(static_cast<double>(y) * yscale));
        const auto yb = std::max(
            ya + 1, std::min(image.height, static_cast<std::size_t>(
                                               std::lround(static_cast<double>(y + 1) * yscale))));
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t xb = std::max(x0[x] + 1, x0[x + 1]);
            std::uint64_t sum = 0;
            for (std::size_t sy = ya; sy < yb; ++sy) {
                for (std::size_t sx = x0[x]; sx < xb; ++sx) sum += image.at(sx, sy);
            }
            const std::uint64_t n = (yb - ya) * (xb - x0[x]);
            out.at(x, y) = static_cast<std::uint8_t>((sum + n / 2) / n);
        }
    }
    return out;
}

}  // namespace owfax
