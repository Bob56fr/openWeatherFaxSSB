// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// WEFAX signal generator with simulated impairments. Used as a reproducible test
// bench for the decoder (spec TST-001/TST-002) and for on-device self-tests.
// Receive-only project: this produces audio files, never a transmission (NFR-001).

#pragma once

#include <cstdint>
#include <optional>

#include "owfax/audio/wav.hpp"
#include "owfax/image/gray_image.hpp"
#include "owfax/wefax_params.hpp"

namespace owfax::wefax {

struct ModulatorOptions {
    int sample_rate = 11025;
    Ioc ioc = Ioc::k576;
    Lpm lpm = Lpm::k120;
    double start_tone_s = 5.0;     ///< APT start tone; 0 simulates a missed start.
    double phasing_s = 30.0;       ///< Phasing lines (white pulse centred on the line edge).
    double stop_tone_s = 5.0;      ///< APT stop tone.
    double trailer_s = 5.0;        ///< Black after the stop tone.
    double tuning_offset_hz = 0;   ///< Simulated SSB mistuning.
    double shift_hz = 800.0;       ///< Black→white shift (DWD: 850 Hz).
    double clock_ppm = 0.0;        ///< Simulated sound card clock error (causes slant).
    std::optional<double> snr_db;  ///< White noise, SNR measured in a 2.5 kHz bandwidth.
    std::uint32_t noise_seed = 1;
};

/// Modulates `image` (any width, resampled to the line) into WEFAX audio.
audio::AudioBuffer modulate(const GrayImage& image, const ModulatorOptions& options);

/// Synthetic weather-chart-like test pattern: frame, grid, diagonal, checkerboard,
/// gray ramp. Width = pixels_per_line(ioc).
GrayImage make_test_chart(Ioc ioc, std::size_t lines);

}  // namespace owfax::wefax
