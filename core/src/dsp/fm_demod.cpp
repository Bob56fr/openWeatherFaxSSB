// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/dsp/fm_demod.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "owfax/dsp/fir.hpp"

namespace owfax::dsp {
namespace {
constexpr double kTransitionHz = 800.0;
constexpr std::size_t kMaxTaps = 511;
}  // namespace

FmDemodulator::FmDemodulator(double input_rate, double center_hz, double cutoff_hz,
                             std::size_t decimation)
    : in_rate_(input_rate),
      center_hz_(center_hz),
      decim_(decimation == 0 ? 1 : decimation),
      osc_step_(kTwoPi * center_hz / input_rate) {
    if (input_rate <= 0.0) throw std::invalid_argument("FmDemodulator: invalid rate");
    if (2.0 * cutoff_hz >= output_rate()) {
        throw std::invalid_argument("FmDemodulator: decimation too high for cutoff");
    }
    auto n = static_cast<std::size_t>(std::ceil(5.5 * input_rate / kTransitionHz)) | 1U;
    n = std::min(n, kMaxTaps);
    taps_ = design_lowpass(n, cutoff_hz, input_rate);
    history_.assign(2 * n, std::complex<float>(0.0F, 0.0F));
}

void FmDemodulator::process(const float* in, std::size_t count, std::vector<float>& out) {
    const std::size_t n = taps_.size();
    const double hz_per_radian = output_rate() / kTwoPi;
    for (std::size_t i = 0; i < count; ++i) {
        const double x = static_cast<double>(in[i]);
        const std::complex<float> mixed(static_cast<float>(x * std::cos(osc_phase_)),
                                        static_cast<float>(-x * std::sin(osc_phase_)));
        osc_phase_ += osc_step_;
        if (osc_phase_ >= kTwoPi) osc_phase_ -= kTwoPi;

        history_[hist_pos_] = mixed;
        history_[hist_pos_ + n] = mixed;
        hist_pos_ = (hist_pos_ + 1) % n;

        if (++phase_count_ < decim_) continue;
        phase_count_ = 0;

        const std::complex<float>* window = &history_[hist_pos_];
        std::complex<float> acc(0.0F, 0.0F);
        for (std::size_t k = 0; k < n; ++k) acc += taps_[k] * window[k];

        const std::complex<float> d = acc * std::conj(prev_);
        prev_ = acc;
        const double angle =
            std::atan2(static_cast<double>(d.imag()), static_cast<double>(d.real()));
        out.push_back(static_cast<float>(center_hz_ + angle * hz_per_radian));
    }
}

}  // namespace owfax::dsp
