// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Minimal WAV file reader/writer (spec SRC-006, SRC-008).

#pragma once

#include <string>
#include <vector>

namespace owfax::audio {

/// Mono audio, samples normalised to [-1, 1].
struct AudioBuffer {
    int sample_rate = 0;
    std::vector<float> samples;

    double duration_s() const noexcept {
        return sample_rate > 0 ? static_cast<double>(samples.size()) / sample_rate : 0.0;
    }
};

/// Reads a RIFF/WAVE file (PCM 8/16/24/32-bit or IEEE float 32/64-bit, any channel
/// count, WAVE_FORMAT_EXTENSIBLE included). Channels are averaged to mono.
/// Throws std::runtime_error on unreadable or unsupported files.
AudioBuffer read_wav(const std::string& path);

/// Writes a mono 16-bit PCM WAV file. Samples are clamped to [-1, 1].
/// Throws std::runtime_error on I/O failure.
void write_wav(const std::string& path, const AudioBuffer& audio);

}  // namespace owfax::audio
