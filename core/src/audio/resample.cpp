// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/audio/resample.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "owfax/dsp/fir.hpp"

namespace owfax::audio {

AudioBuffer slice(const AudioBuffer& audio, double from_s, double to_s) {
    AudioBuffer out;
    out.sample_rate = audio.sample_rate;
    if (audio.sample_rate <= 0) return out;
    const double rate = audio.sample_rate;
    const auto n = audio.samples.size();
    const auto a = std::min(n, static_cast<std::size_t>(std::max(0.0, from_s) * rate));
    const auto b = std::min(n, static_cast<std::size_t>(std::max(0.0, to_s) * rate));
    if (b > a) {
        out.samples.assign(audio.samples.begin() + static_cast<std::ptrdiff_t>(a),
                           audio.samples.begin() + static_cast<std::ptrdiff_t>(b));
    }
    return out;
}

AudioBuffer resample(const AudioBuffer& audio, int out_rate) {
    if (audio.sample_rate <= 0 || out_rate <= 0) throw std::invalid_argument("resample: rate");
    if (out_rate == audio.sample_rate) return audio;

    const double in_rate = audio.sample_rate;
    const double lower = std::min(in_rate, static_cast<double>(out_rate));
    const double transition = 0.1 * lower;
    auto taps_n = static_cast<std::size_t>(std::ceil(5.5 * in_rate / transition)) | 1U;
    taps_n = std::min<std::size_t>(taps_n, 1023);
    const auto taps = dsp::design_lowpass(taps_n, 0.45 * lower, in_rate);
    const auto half = static_cast<long>(taps_n / 2);
    const auto& x = audio.samples;
    const auto len = static_cast<long>(x.size());

    // Low-pass output at integer input position i (zero padding outside).
    auto filtered = [&](long i) {
        double acc = 0.0;
        for (long k = 0; k < static_cast<long>(taps_n); ++k) {
            const long j = i + k - half;
            if (j >= 0 && j < len) {
                acc += static_cast<double>(taps[static_cast<std::size_t>(k)]) *
                       static_cast<double>(x[static_cast<std::size_t>(j)]);
            }
        }
        return acc;
    };

    AudioBuffer out;
    out.sample_rate = out_rate;
    const double step = in_rate / out_rate;
    const auto n_out = static_cast<std::size_t>(static_cast<double>(x.size()) / step);
    out.samples.resize(n_out);
    for (std::size_t m = 0; m < n_out; ++m) {
        const double pos = static_cast<double>(m) * step;
        const auto i = static_cast<long>(pos);
        const double t = pos - static_cast<double>(i);
        const double y0 = filtered(i);
        const double y1 = t > 0.0 ? filtered(i + 1) : y0;
        out.samples[m] = static_cast<float>(y0 + (y1 - y0) * t);
    }
    return out;
}

}  // namespace owfax::audio
