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

struct Telemetry {
    std::vector<float> audio_dbfs;    ///< One value per second of audio.
    std::vector<float> snr_db;        ///< Same length; SNR in a 2.5 kHz bandwidth.
    std::vector<float> smeter_db_s9;  ///< Same length, or empty without S-meter readings.
};

/// Measures `audio` second by second. `audio_start_unix`: Unix time of its first
/// sample, to place the S-meter readings (interpolated linearly, held at the ends).
Telemetry measure(const audio::AudioBuffer& audio, const SnrBands& bands,
                  const std::vector<SmeterReading>& smeter = {},
                  std::optional<double> audio_start_unix = std::nullopt);

}  // namespace owfax::vitrine
