// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// End-to-end: synthetic chart → modulator (with impairments) → decoder → compare.

#include <cmath>
#include <cstdio>

#include "owfax/wefax/decoder.hpp"
#include "owfax/wefax/modulator.hpp"
#include "owfax_test.hpp"

using namespace owfax::wefax;

namespace {

constexpr std::size_t kLines = 150;

/// Best Pearson correlation between the two images over small vertical shifts.
double image_correlation(const owfax::GrayImage& a, const owfax::GrayImage& b) {
    if (a.width != b.width || a.height < 10 || b.height < 10) return 0.0;
    double best = -1.0;
    for (int shift = -2; shift <= 2; ++shift) {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;
        for (std::size_t y = 2; y + 2 < a.height; ++y) {
            const long yb = static_cast<long>(y) + shift;
            if (yb < 0 || yb >= static_cast<long>(b.height)) continue;
            for (std::size_t x = 0; x < a.width; ++x) {
                const double va = a.at(x, y);
                const double vb = b.at(x, static_cast<std::size_t>(yb));
                sa += va;
                sb += vb;
                saa += va * va;
                sbb += vb * vb;
                sab += va * vb;
                n += 1;
            }
        }
        const double cov = sab / n - (sa / n) * (sb / n);
        const double den =
            std::sqrt((saa / n - (sa / n) * (sa / n)) * (sbb / n - (sb / n) * (sb / n)));
        if (den > 0 && cov / den > best) best = cov / den;
    }
    return best;
}

struct Outcome {
    DecodeReport report;
    double correlation;
    std::size_t lines;
};

Outcome run(const ModulatorOptions& mod, const DecodeOptions& dec = {}) {
    const auto chart = make_test_chart(mod.ioc, kLines);
    const auto audio = modulate(chart, mod);
    const auto result = decode(audio, dec);
    const double corr = image_correlation(chart, result.image);
    std::printf(
        "    rate=%d offset=%+.0f ppm=%+.0f snr=%s → offset=%+.1f ppm=%+.1f lines=%zu corr=%.3f\n",
        mod.sample_rate, mod.tuning_offset_hz, mod.clock_ppm,
        mod.snr_db ? std::to_string(*mod.snr_db).substr(0, 4).c_str() : "-",
        result.report.tuning_offset_hz, result.report.clock_ppm, result.image.height, corr);
    return {result.report, corr, result.image.height};
}

}  // namespace

OWFAX_TEST(decode_clean_signal) {
    ModulatorOptions mod;
    const Outcome o = run(mod);
    OWFAX_CHECK(o.report.apt_start_found);
    OWFAX_CHECK(o.report.phasing_found);
    OWFAX_CHECK(o.report.apt_stop_found);
    OWFAX_CHECK(o.report.ioc == Ioc::k576);
    OWFAX_CHECK_NEAR(o.report.tuning_offset_hz, 0.0, 3.0);
    OWFAX_CHECK_NEAR(o.report.clock_ppm, 0.0, 3.0);
    OWFAX_CHECK(o.lines >= kLines - 2 && o.lines <= kLines + 1);
    OWFAX_CHECK(o.correlation > 0.95);
}

OWFAX_TEST(decode_mistuned_with_clock_error_48k) {
    ModulatorOptions mod;
    mod.sample_rate = 48000;
    mod.tuning_offset_hz = 80.0;
    mod.clock_ppm = 250.0;
    const Outcome o = run(mod);
    OWFAX_CHECK_NEAR(o.report.tuning_offset_hz, 80.0, 5.0);
    OWFAX_CHECK_NEAR(o.report.clock_ppm, 250.0, 3.0);
    OWFAX_CHECK(o.correlation > 0.9);
}

OWFAX_TEST(decode_noisy_signal) {
    ModulatorOptions mod;
    mod.sample_rate = 8000;
    mod.tuning_offset_hz = -50.0;
    mod.clock_ppm = -400.0;
    mod.snr_db = 12.0;
    const Outcome o = run(mod);
    OWFAX_CHECK(o.report.apt_start_found);
    OWFAX_CHECK(o.report.apt_stop_found);
    OWFAX_CHECK_NEAR(o.report.tuning_offset_hz, -50.0, 10.0);
    OWFAX_CHECK_NEAR(o.report.clock_ppm, -400.0, 5.0);
    OWFAX_CHECK(o.correlation > 0.7);
}

OWFAX_TEST(decode_missed_start_tone) {
    ModulatorOptions mod;
    mod.start_tone_s = 0.0;
    mod.clock_ppm = 120.0;
    const Outcome o = run(mod);
    OWFAX_CHECK(!o.report.apt_start_found);
    OWFAX_CHECK(o.report.phasing_found);
    OWFAX_CHECK_NEAR(o.report.clock_ppm, 120.0, 3.0);
    OWFAX_CHECK(o.correlation > 0.9);
}

OWFAX_TEST(decode_long_image_refines_clock) {
    // 400 lines (> refinement threshold), noisy, with an awkward clock error.
    ModulatorOptions mod;
    mod.sample_rate = 8000;
    mod.clock_ppm = 333.0;
    mod.snr_db = 15.0;
    const auto chart = make_test_chart(mod.ioc, 400);
    const auto result = decode(modulate(chart, mod));
    std::printf("    long image → ppm=%+.2f lines=%zu\n", result.report.clock_ppm,
                result.image.height);
    OWFAX_CHECK_NEAR(result.report.clock_ppm, 333.0, 1.0);
    OWFAX_CHECK(result.image.height >= 398 && result.image.height <= 401);
}

OWFAX_TEST(decode_detects_ioc_288) {
    ModulatorOptions mod;
    mod.ioc = Ioc::k288;
    const Outcome o = run(mod);
    OWFAX_CHECK(o.report.ioc == Ioc::k288);
    OWFAX_CHECK(o.correlation > 0.9);
}
