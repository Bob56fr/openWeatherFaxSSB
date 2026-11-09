// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include <cmath>
#include <vector>

#include "owfax/dsp/fir.hpp"
#include "owfax/dsp/fm_demod.hpp"
#include "owfax_test.hpp"

using owfax::dsp::kTwoPi;

namespace {
std::vector<float> sine(double freq, double rate, std::size_t n) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<float>(0.5 * std::sin(kTwoPi * freq * static_cast<double>(i) / rate));
    }
    return v;
}
}  // namespace

OWFAX_TEST(lowpass_has_unity_dc_gain) {
    const auto taps = owfax::dsp::design_lowpass(63, 1000.0, 8000.0);
    double sum = 0.0;
    for (float t : taps) sum += static_cast<double>(t);
    OWFAX_CHECK_NEAR(sum, 1.0, 1e-5);
    OWFAX_CHECK(taps.size() == 63);
}

OWFAX_TEST(tone_ratio_discriminates) {
    const auto tone = sine(300.0, 8000.0, 2000);
    OWFAX_CHECK(owfax::dsp::tone_ratio(tone.data(), tone.size(), 300.0, 8000.0) > 0.9);
    OWFAX_CHECK(owfax::dsp::tone_ratio(tone.data(), tone.size(), 450.0, 8000.0) < 0.05);
}

OWFAX_TEST(fm_demod_measures_tone_frequency) {
    for (double rate : {8000.0, 11025.0, 48000.0}) {
        const auto decim = static_cast<std::size_t>(std::max(1.0, std::floor(rate / 8000.0)));
        owfax::dsp::FmDemodulator demod(rate, 1900.0, 1200.0, decim);
        const auto tone = sine(2123.0, rate, static_cast<std::size_t>(rate));
        std::vector<float> freq;
        demod.process(tone.data(), tone.size(), freq);
        // Skip the filter start-up, then average.
        double mean = 0.0;
        std::size_t n = 0;
        for (std::size_t i = freq.size() / 4; i < freq.size(); ++i, ++n) {
            mean += static_cast<double>(freq[i]);
        }
        mean /= static_cast<double>(n);
        OWFAX_CHECK_NEAR(mean, 2123.0, 0.5);
    }
}
