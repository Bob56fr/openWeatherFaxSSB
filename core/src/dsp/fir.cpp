// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/dsp/fir.hpp"

#include <cmath>
#include <stdexcept>

namespace owfax::dsp {

std::vector<float> design_lowpass(std::size_t num_taps, double cutoff_hz, double sample_rate) {
    if (num_taps < 3 || sample_rate <= 0.0 || cutoff_hz <= 0.0 || cutoff_hz >= sample_rate / 2.0) {
        throw std::invalid_argument("design_lowpass: invalid parameters");
    }
    const double fc = cutoff_hz / sample_rate;
    const double m = static_cast<double>(num_taps - 1);
    std::vector<double> h(num_taps);
    double sum = 0.0;
    for (std::size_t i = 0; i < num_taps; ++i) {
        const double n = static_cast<double>(i);
        const double x = n - m / 2.0;
        const double sinc = x == 0.0 ? 2.0 * fc : std::sin(kTwoPi * fc * x) / (kTwoPi / 2.0 * x);
        const double window =
            0.42 - 0.5 * std::cos(kTwoPi * n / m) + 0.08 * std::cos(2.0 * kTwoPi * n / m);
        h[i] = sinc * window;
        sum += h[i];
    }
    std::vector<float> taps(num_taps);
    for (std::size_t i = 0; i < num_taps; ++i) taps[i] = static_cast<float>(h[i] / sum);
    return taps;
}

double tone_ratio(const float* x, std::size_t n, double freq_hz, double sample_rate) {
    if (n < 2 || sample_rate <= 0.0) return 0.0;
    double mean = 0.0;
    for (std::size_t i = 0; i < n; ++i) mean += static_cast<double>(x[i]);
    mean /= static_cast<double>(n);

    const double coeff = 2.0 * std::cos(kTwoPi * freq_hz / sample_rate);
    double s1 = 0.0;
    double s2 = 0.0;
    double energy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(x[i]) - mean;
        energy += v * v;
        const double s0 = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    if (energy <= 0.0) return 0.0;
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * power / (static_cast<double>(n) * energy);
}

}  // namespace owfax::dsp
