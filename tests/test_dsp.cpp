// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include <algorithm>
#include <cmath>
#include <vector>

#include "owfax/audio/resample.hpp"
#include "owfax/dsp/fir.hpp"
#include "owfax/dsp/fm_demod.hpp"
#include "owfax/wefax/probe.hpp"
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

OWFAX_TEST(resample_keeps_tone_and_duration) {
    owfax::audio::AudioBuffer a;
    a.sample_rate = 48000;
    a.samples = sine(1900.0, 48000.0, 48000 * 2);
    const auto b = owfax::audio::resample(a, 11025);
    OWFAX_CHECK(b.sample_rate == 11025);
    OWFAX_CHECK_NEAR(b.duration_s(), 2.0, 0.01);
    // Frequency preserved: demodulate the resampled tone.
    owfax::dsp::FmDemodulator demod(11025.0, 1900.0, 1200.0, 1);
    std::vector<float> freq;
    demod.process(b.samples.data(), b.samples.size(), freq);
    double mean = 0.0;
    std::size_t n = 0;
    for (std::size_t i = freq.size() / 4; i < freq.size() * 3 / 4; ++i, ++n) {
        mean += static_cast<double>(freq[i]);
    }
    OWFAX_CHECK_NEAR(mean / static_cast<double>(n), 1900.0, 0.5);
    // Amplitude preserved (passband gain ~1).
    double peak = 0.0;
    for (std::size_t i = b.samples.size() / 4; i < b.samples.size() * 3 / 4; ++i) {
        peak = std::max(peak, static_cast<double>(std::fabs(b.samples[i])));
    }
    OWFAX_CHECK_NEAR(peak, 0.5, 0.02);
    const auto s = owfax::audio::slice(a, 0.5, 1.0);
    OWFAX_CHECK(s.samples.size() == 24000);
}

OWFAX_TEST(probe_rejects_idle_carrier) {
    owfax::audio::AudioBuffer carrier;
    carrier.sample_rate = 11025;
    carrier.samples = sine(2325.0, 11025.0, 11025 * 20);  // DWD idle "white"
    const auto idle = owfax::wefax::probe(carrier);
    OWFAX_CHECK(idle.freq_spread_hz < 100.0);
    OWFAX_CHECK(idle.score < 0.05);
}
