// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#pragma once

#include <cstddef>
#include <vector>

namespace owfax::dsp {

inline constexpr double kTwoPi = 6.28318530717958647692;

/// Windowed-sinc low-pass FIR (Blackman window), unity DC gain.
/// `num_taps` should be odd; `cutoff_hz` must be below sample_rate / 2.
/// Transition width is roughly 5.5 × sample_rate / num_taps.
std::vector<float> design_lowpass(std::size_t num_taps, double cutoff_hz, double sample_rate);

/// Goertzel tone detector. Returns the fraction of the (mean-removed) signal energy
/// found at `freq_hz`: about 1 for a pure sine, 0.81 for a square wave, ~0 otherwise.
double tone_ratio(const float* x, std::size_t n, double freq_hz, double sample_rate);

}  // namespace owfax::dsp
