// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Offline WEFAX decoder: whole recording in, image out (spec §6).
// A streaming, line-by-line variant (DSP-007) will build on the same stages.

#pragma once

#include <optional>
#include <vector>

#include "owfax/audio/wav.hpp"
#include "owfax/image/gray_image.hpp"
#include "owfax/wefax_params.hpp"

namespace owfax::wefax {

struct DecodeOptions {
    std::optional<Ioc> ioc;  ///< Forced IOC; otherwise from the APT start tone (576 if none).
    Lpm lpm = Lpm::k120;     ///< Lines per minute.
    std::optional<double> clock_ppm;  ///< Forced sound card clock error; estimated otherwise.
    double max_clock_ppm = 1000.0;    ///< Search range for the clock error estimate.
    double shift_hz = 800.0;          ///< Nominal black→white shift of the station (DWD: 850 Hz).
    /// FM click suppression (DSP-020): repairs isolated demodulator spikes and samples far
    /// outside the black–white range. Off by default: the raw image is the reference (DSP-030).
    bool suppress_clicks = false;
};

struct DecodeReport {
    bool apt_start_found = false;
    bool phasing_found = false;
    bool apt_stop_found = false;
    Ioc ioc = Ioc::k576;
    double black_hz = kBlackHz;       ///< Measured black frequency.
    double white_hz = kWhiteHz;       ///< Measured white frequency.
    double tuning_offset_hz = 0.0;    ///< Measured centre − 1900 Hz: SSB mistuning (DSP-004).
    double clock_ppm = 0.0;           ///< Clock error used for slant correction (DSP-005).
    double apt_start_s = -1.0;        ///< Start of the APT start tone, −1 if not found.
    double image_start_s = 0.0;       ///< Start of the first image line.
    double image_end_s = 0.0;         ///< End of the last image line.
    std::vector<float> line_quality;  ///< Per image line: 1 = clean … 0 = unusable (DSP-008).
    std::size_t clicks_repaired = 0;  ///< Samples repaired by click suppression (0 if off).
};

struct DecodeResult {
    GrayImage image;
    DecodeReport report;
};

/// Decodes one WEFAX transmission contained in `audio`.
/// Handles a missing APT start (manual start, DSP-006): the image then begins at the
/// first sample and is aligned on the phasing signal if present.
/// Throws std::invalid_argument / std::runtime_error on unusable input.
DecodeResult decode(const audio::AudioBuffer& audio, const DecodeOptions& options = {});

/// Same as decode(), from the instantaneous frequency (Hz) already demodulated at
/// `rate` samples/s (used by the streaming decoder and by SDR paths).
DecodeResult decode_frequency(std::vector<float> freq_hz, double rate,
                              const DecodeOptions& options = {});

/// Splits a long recording into transmissions at each APT start tone and decodes
/// each one. A leading partial transmission (recording started mid-chart) is decoded
/// too when long enough. Report times are relative to the start of the recording.
std::vector<DecodeResult> decode_all(const audio::AudioBuffer& audio,
                                     const DecodeOptions& options = {});

}  // namespace owfax::wefax
