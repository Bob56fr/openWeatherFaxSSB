// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include <algorithm>
#include <cmath>
#include <vector>

#include "owfax/audio/resample.hpp"
#include "owfax/dsp/fir.hpp"
#include "owfax/dsp/fm_demod.hpp"
#include "owfax/dsp/spectrum.hpp"
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

OWFAX_TEST(spectrum_locates_tones_and_measures_level) {
    // WEFAX black and white tones of a mistuned receiver (+100 Hz), half scale, 48 kHz.
    const double rate = 48000.0;
    std::vector<float> s(48000);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        s[i] = static_cast<float>(0.25 * std::sin(kTwoPi * 1600.0 * t) +
                                  0.25 * std::sin(kTwoPi * 2400.0 * t));
    }
    owfax::dsp::SpectrumAnalyzer an(48000, 300, 3000.0, 0.25);  // 10 Hz bands
    std::vector<owfax::dsp::SpectrumFrame> frames;
    for (std::size_t pos = 0; pos < s.size(); pos += 4800) {  // 0.1 s blocks, as owfaxd
        auto f = an.push(&s[pos], 4800);
        while (f) {
            frames.push_back(*f);
            f = an.push(nullptr, 0);
        }
    }
    OWFAX_CHECK(frames.size() == 4);  // one per 0.25 s
    if (frames.empty()) return;
    const auto& f = frames.back();
    OWFAX_CHECK(f.db.size() == 300 && f.max_hz == 3000.0);
    // Strongest band around each expected tone (bands are 10 Hz wide).
    auto argmax = [&](std::size_t from, std::size_t to) {
        std::size_t best = from;
        for (std::size_t i = from; i < to; ++i) {
            if (f.db[i] > f.db[best]) best = i;
        }
        return best;
    };
    const std::size_t lo = argmax(130, 200);
    const std::size_t hi = argmax(210, 280);
    OWFAX_CHECK(lo >= 159 && lo <= 160);  // 1600 Hz
    OWFAX_CHECK(hi >= 239 && hi <= 240);  // 2400 Hz
    // A 0.25-amplitude sine reads about -12 dB; far from the tones, much lower.
    OWFAX_CHECK_NEAR(f.db[lo], -12.0, 1.6);
    OWFAX_CHECK(f.db[50] < -60.0);
    // Two 0.25 sines: RMS level of a 0.35 sine, -9 dBFS; peak at most 0.5 (-6 dBFS).
    OWFAX_CHECK_NEAR(f.rms_dbfs, -9.0, 0.5);
    OWFAX_CHECK(f.peak_dbfs <= -6.0 + 0.1 && f.peak_dbfs > -7.0);
    OWFAX_CHECK(f.clipped == 0);
}

OWFAX_TEST(spectrum_counts_clipping) {
    std::vector<float> s(12000, 1.0F);
    owfax::dsp::SpectrumAnalyzer an(11025);
    auto f = an.push(s.data(), s.size());
    OWFAX_CHECK(f.has_value());
    if (f) {
        OWFAX_CHECK(f->clipped > 2000);
        OWFAX_CHECK_NEAR(f->peak_dbfs, 0.0, 0.01);
    }
}
