// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// FM discriminator for audio sub-carriers (spec DSP-001).

#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace owfax::dsp {

/// Converts real audio into instantaneous frequency (Hz):
/// mix down by `center_hz` → complex low-pass (`cutoff_hz`) with decimation →
/// phase differentiator. Streaming: may be fed blocks of any size.
class FmDemodulator {
public:
    FmDemodulator(double input_rate, double center_hz, double cutoff_hz, std::size_t decimation);

    /// Rate of the frequency samples produced (input_rate / decimation).
    double output_rate() const noexcept { return in_rate_ / static_cast<double>(decim_); }

    /// Group delay of the low-pass filter, in input samples.
    double delay_samples() const noexcept { return static_cast<double>(taps_.size() - 1) / 2.0; }

    /// Appends one absolute frequency value (Hz) per `decimation` input samples to `out`.
    void process(const float* in, std::size_t count, std::vector<float>& out);

private:
    double in_rate_;
    double center_hz_;
    std::size_t decim_;
    std::vector<float> taps_;
    std::vector<std::complex<float>> history_;  // doubled delay line
    std::size_t hist_pos_ = 0;
    std::size_t phase_count_ = 0;
    double osc_phase_ = 0.0;
    double osc_step_;
    std::complex<float> prev_{0.0F, 0.0F};
};

}  // namespace owfax::dsp
