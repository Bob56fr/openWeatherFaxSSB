// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Audio input spectrum and level, for the diagnostic page (spec UI-007: spectrum and
// waterfall of the input) and the audio level check of the setup assistant (UI-003).
// A WEFAX signal sits between 1500 Hz (black) and 2300 Hz (white): a mistuned SSB
// receiver shows at once as both peaks shifted.

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace owfax::dsp {

struct SpectrumFrame {
    std::vector<float> db;     ///< Power per band, dB relative to a full-scale sine.
    double max_hz = 0.0;       ///< Bands cover 0 … max_hz evenly.
    double rms_dbfs = -200.0;  ///< Level over the frame, dB relative to full scale.
    double peak_dbfs = -200.0;
    std::size_t clipped = 0;  ///< Samples at or beyond ±0.999 (clipping) in the frame.
};

class SpectrumAnalyzer {
public:
    /// One frame every `hop_s` seconds, `bands` bands from 0 to `max_hz`.
    SpectrumAnalyzer(int sample_rate, std::size_t bands = 256, double max_hz = 3000.0,
                     double hop_s = 0.25);

    /// Feeds samples; returns a frame when one is complete (at most one per call: call
    /// again with count 0 to collect any further frame).
    std::optional<SpectrumFrame> push(const float* samples, std::size_t count);

    std::size_t fft_size() const noexcept { return n_; }

private:
    std::size_t n_;
    std::size_t hop_;
    std::size_t bands_;
    double max_hz_;
    double rate_;
    std::vector<float> window_;
    std::vector<float> ring_;  // last n_ samples
    std::size_t filled_ = 0;
    std::size_t since_ = 0;  // samples since the last frame
    double sum_sq_ = 0.0;
    double peak_ = 0.0;
    std::size_t clipped_ = 0;
    std::size_t level_n_ = 0;
    std::vector<float> pending_;  // input not yet consumed
};

}  // namespace owfax::dsp
