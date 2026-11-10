// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "owfax/audio/wav.hpp"
#include "owfax/image/gray_image.hpp"
#include "owfax/image/zlib.hpp"
#include "owfax_test.hpp"

namespace fs = std::filesystem;

OWFAX_TEST(crc32_and_adler32_reference_values) {
    const char* s = "123456789";
    const auto* p = reinterpret_cast<const std::uint8_t*>(s);
    OWFAX_CHECK(owfax::detail::crc32(p, 9) == 0xCBF43926U);
    const char* w = "Wikipedia";
    OWFAX_CHECK(owfax::detail::adler32(reinterpret_cast<const std::uint8_t*>(w), 9) == 0x11E60398U);
}

OWFAX_TEST(wav_roundtrip) {
    owfax::audio::AudioBuffer a;
    a.sample_rate = 11025;
    a.samples = {0.0F, 0.5F, -0.5F, 1.0F, -1.0F, 0.25F};
    const auto path = (fs::temp_directory_path() / "owfax_test_roundtrip.wav").string();
    owfax::audio::write_wav(path, a);
    const auto b = owfax::audio::read_wav(path);
    fs::remove(path);
    OWFAX_CHECK(b.sample_rate == 11025);
    OWFAX_CHECK(b.samples.size() == a.samples.size());
    for (std::size_t i = 0; i < a.samples.size() && i < b.samples.size(); ++i) {
        OWFAX_CHECK_NEAR(b.samples[i], a.samples[i], 1e-4);
    }
}

OWFAX_TEST(png_has_valid_structure) {
    owfax::GrayImage img(70000, 2, 128);
    const auto path = (fs::temp_directory_path() / "owfax_test.png").string();
    owfax::write_png(path, img);
    std::ifstream in(path, std::ios::binary);
    char sig[8] = {};
    in.read(sig, 8);
    const auto size = fs::file_size(path);
    in.close();
    const auto back = owfax::read_png(path);
    fs::remove(path);
    OWFAX_CHECK(std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) == 0);
    OWFAX_CHECK(size < 2000);  // uniform image: compressed
    OWFAX_CHECK(back.width == img.width && back.height == img.height);
    OWFAX_CHECK(back.pixels == img.pixels);
}

namespace {

/// Deterministic pseudo-random bytes (xorshift), identical on every platform.
std::vector<std::uint8_t> noise_bytes(std::size_t n, std::uint32_t seed) {
    std::vector<std::uint8_t> v(n);
    std::uint32_t s = seed;
    for (auto& b : v) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        b = static_cast<std::uint8_t>(s >> 24);
    }
    return v;
}

}  // namespace

OWFAX_TEST(zlib_round_trip) {
    std::vector<std::vector<std::uint8_t>> cases;
    cases.emplace_back();                                     // empty
    cases.push_back({42});                                    // one byte
    cases.push_back(std::vector<std::uint8_t>(300000, 255));  // long runs, many blocks
    cases.push_back(noise_bytes(100000, 7));                  // incompressible
    std::vector<std::uint8_t> text;
    for (int i = 0; i < 5000; ++i) {
        const std::string s = "line " + std::to_string(i % 97) + " of a chart; ";
        text.insert(text.end(), s.begin(), s.end());
    }
    cases.push_back(text);  // repeats at every distance
    auto mixed = noise_bytes(70000, 9);
    for (std::size_t i = 0; i < mixed.size(); i += 3) mixed[i] = 0;
    cases.push_back(mixed);
    for (const auto& c : cases) {
        const auto z = owfax::zlib_compress(c.data(), c.size());
        const auto back = owfax::zlib_decompress(z.data(), z.size(), c.size() + 1);
        OWFAX_CHECK(back.has_value() && *back == c);
        if (c.size() == 300000) OWFAX_CHECK(z.size() < 3000);
        if (c.size() == 100000) OWFAX_CHECK(z.size() < c.size() + c.size() / 50);
    }
    // Corruption is detected, and the output limit holds.
    const auto z = owfax::zlib_compress(text.data(), text.size());
    auto bad = z;
    bad[bad.size() / 2] ^= 0x55;
    const auto r = owfax::zlib_decompress(bad.data(), bad.size(), text.size() + 1);
    OWFAX_CHECK(!r.has_value() || *r != text);
    OWFAX_CHECK(!owfax::zlib_decompress(z.data(), z.size(), text.size() / 2).has_value());
    // A stored (uncompressed) zlib stream, as the previous writer produced.
    const std::vector<std::uint8_t> stored = {0x78, 0x01, 0x01, 0x03, 0x00, 0xFC, 0xFF,
                                              'a',  'b',  'c',  0x02, 0x4D, 0x01, 0x27};
    const auto s = owfax::zlib_decompress(stored.data(), stored.size(), 10);
    OWFAX_CHECK(s.has_value() && s->size() == 3 && (*s)[0] == 'a' && (*s)[2] == 'c');
}

OWFAX_TEST(png_round_trip_chart_like_image) {
    // White chart with frame, grid, a diagonal, text-like checkerboard and noise.
    owfax::GrayImage img(1810, 400, 255);
    const auto speckle = noise_bytes(img.pixels.size(), 3);
    for (std::size_t y = 0; y < img.height; ++y) {
        for (std::size_t x = 0; x < img.width; ++x) {
            const bool line = x % 226 < 2 || y % 50 < 2 || (x / 4 == y) ||
                              (y > 300 && y < 340 && x > 900 && x < 1300 && ((x / 6 + y / 6) % 2));
            std::uint8_t v = line ? 0 : 255;
            if (speckle[y * img.width + x] < 8)
                v = static_cast<std::uint8_t>(255 - v);  // FM clicks
            img.at(x, y) = v;
        }
    }
    const auto png = owfax::encode_png(img);
    const auto back = owfax::decode_png(png.data(), png.size());
    OWFAX_CHECK(back.width == img.width && back.height == img.height && back.pixels == img.pixels);
    std::printf("    chart-like PNG: %zu bytes for %zu pixels (%.1fx)\n", png.size(),
                img.pixels.size(),
                static_cast<double>(img.pixels.size()) / static_cast<double>(png.size()));
    OWFAX_CHECK(png.size() * 4 < img.pixels.size());
}

OWFAX_TEST(png_16_levels_display_version) {
    // Odd width: the last byte of each 4-bit scanline holds a single pixel.
    owfax::GrayImage img(1809, 120, 255);
    const auto speckle = noise_bytes(img.pixels.size(), 5);
    for (std::size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = speckle[i];
    const auto full = owfax::encode_png(img, 8);
    const auto light = owfax::encode_png(img, 4);
    OWFAX_CHECK(full[24] == 8 && light[24] == 4);  // IHDR bit depth
    const auto back = owfax::decode_png(light.data(), light.size());
    OWFAX_CHECK(back.width == img.width && back.height == img.height);
    bool nearest = true;
    for (std::size_t i = 0; i < img.pixels.size(); ++i) {
        const int expected = ((img.pixels[i] * 15 + 127) / 255) * 17;
        if (back.pixels[i] != expected) nearest = false;
    }
    OWFAX_CHECK(nearest);  // nearest of 0, 17, ... 255, never further than 8
    OWFAX_CHECK(light.size() * 3 < full.size() * 2);
    OWFAX_CHECK(owfax::decode_png(full.data(), full.size()).pixels == img.pixels);
}
