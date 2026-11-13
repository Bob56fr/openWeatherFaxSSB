// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Per-second measurements of a chart's audio for the replay player
// (docs/VITRINE-WEB.md, PUB-050 … PUB-053): input level, estimated SNR, and the
// S-meter readings of the capture when there are any.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "owfax/audio/wav.hpp"

namespace owfax::vitrine {

struct SmeterReading {
    std::int64_t unix_time = 0;
    double db_s9 = 0.0;  ///< dB relative to S9 (Hamlib STRENGTH).
};

/// Reads a capture's "<capture>.smeter.csv" ("utc,strength_db", one "HH:MM:SS,dB" row
/// every few seconds). `start_unix` is the start of the capture: times of day are put
/// on its date, rolling over to the next day after midnight. Throws on I/O failure;
/// malformed rows are skipped.
std::vector<SmeterReading> read_smeter_csv(const std::string& path, std::int64_t start_unix);

struct SnrBands {
    double signal_lo_hz = 1400.0;  ///< WEFAX band: black − 100 Hz …
    double signal_hi_hz = 2400.0;  ///< … white + 100 Hz.
};

/// Per-second signatures of the audio used to tell a local disturbance (receiver
/// overload, interference from equipment on board) from natural fading
/// (docs/VITRINE-WEB.md § 5). They need nothing but the audio: any receiver, SDR or
/// transceiver, with or without CAT.
struct AudioFeatures {
    std::vector<float> clip_fraction;  ///< Samples at or beyond ±0.99.
    std::vector<float> kurtosis;  ///< E[x⁴]/E[x²]²: FM fax ≈ 1.5, noise ≈ 3, impulses ≫ 3.
    std::vector<float> flatness;       ///< Spectral flatness 300–2900 Hz: 1 = white noise.
    std::vector<float> band_fraction;  ///< Power in the WEFAX band / power 300–2900 Hz.
    /// Strongest repetition of the signal envelope over 2–200 ms (normalised
    /// autocorrelation, 0 … 1): switching interference (PWM, MPPT) buzzes at its rate.
    std::vector<float> periodicity;
};

struct Telemetry {
    std::vector<float> audio_dbfs;    ///< One value per second of audio.
    std::vector<float> snr_db;        ///< Same length; SNR in a 2.5 kHz bandwidth.
    std::vector<float> smeter_db_s9;  ///< Same length, or empty without S-meter readings.
    AudioFeatures features;           ///< Same length (not published as such).
};

/// Measures `audio` second by second. `audio_start_unix`: Unix time of its first
/// sample, to place the S-meter readings (interpolated linearly, held at the ends).
Telemetry measure(const audio::AudioBuffer& audio, const SnrBands& bands,
                  const std::vector<SmeterReading>& smeter = {},
                  std::optional<double> audio_start_unix = std::nullopt);

}  // namespace owfax::vitrine
