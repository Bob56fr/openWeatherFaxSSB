// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Live audio input abstraction (spec SRC-001 … SRC-008). A source delivers mono
// samples in [-1, 1] at a fixed rate, in blocks, until the end of the stream.
// Portable sources live here (WAV file, raw PCM stream); hardware back-ends (ALSA,
// WASAPI, Android) are in the owfax_io library so that the core stays dependency-free.

#pragma once

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

#include "owfax/audio/wav.hpp"

namespace owfax::audio {

class AudioSource {
public:
    virtual ~AudioSource() = default;

    /// Samples per second delivered by read().
    virtual int sample_rate() const = 0;

    /// Blocks until samples are available and copies at most `max` of them to `out`.
    /// Returns 0 at the end of the stream or after interrupt(). Throws
    /// std::runtime_error on an unrecoverable device error.
    virtual std::size_t read(float* out, std::size_t max) = 0;

    /// Thread-safe: makes a pending or future read() return 0 quickly.
    virtual void interrupt() = 0;

    /// Human-readable description for logs ("ALSA plughw:CARD=Device,DEV=0 @ 48000 Hz").
    virtual std::string description() const = 0;

    /// True for a device that keeps producing whether or not it is read (sound card,
    /// real-time playback): a slow consumer loses samples. False for files and pipes,
    /// which simply wait for the reader.
    virtual bool is_live() const { return false; }

    /// Number of capture overruns (samples lost by the device) since the start.
    virtual std::size_t overruns() const { return 0; }
};

/// Plays a buffer as a stream. `realtime` paces read() at the sample rate (useful to
/// test the live chain with a recording); otherwise it is delivered as fast as asked.
std::unique_ptr<AudioSource> make_buffer_source(AudioBuffer audio, std::string description,
                                                bool realtime = false);

/// Raw signed 16-bit little-endian mono PCM read from `file` (e.g. stdin fed by
/// `arecord -t raw` or rtl_fm). The file is not closed by the source.
std::unique_ptr<AudioSource> make_pcm16_source(std::FILE* file, int sample_rate,
                                               std::string description);

}  // namespace owfax::audio
