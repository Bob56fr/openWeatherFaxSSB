// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include <cstring>
#include <filesystem>
#include <fstream>

#include "owfax/audio/wav.hpp"
#include "owfax/image/gray_image.hpp"
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
    owfax::GrayImage img(70000, 2, 128);  // > 65535 bytes: several deflate blocks
    const auto path = (fs::temp_directory_path() / "owfax_test.png").string();
    owfax::write_png(path, img);
    std::ifstream in(path, std::ios::binary);
    char sig[8] = {};
    in.read(sig, 8);
    const auto size = fs::file_size(path);
    in.close();
    fs::remove(path);
    OWFAX_CHECK(std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) == 0);
    OWFAX_CHECK(size > 140000);
}
