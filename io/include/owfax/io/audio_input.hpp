// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Opens an audio input from a URI (spec SRC-001 … SRC-008):
//
//   wav:<path>          a recording (also any path ending in .wav)
//   stdin               raw s16le mono PCM on standard input, at InputOptions::sample_rate
//   alsa:<device>       ALSA capture device, e.g. alsa:plughw:CARD=Device,DEV=0 (Linux)
//
// Further back-ends (WASAPI, Android AAudio/OpenSL) plug in here.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "owfax/audio/source.hpp"

namespace owfax::io {

struct InputOptions {
    int sample_rate = 48000;  ///< Requested rate (devices, stdin). WAV files keep theirs.
    bool realtime = false;    ///< WAV only: deliver at the recording's own pace.
};

struct InputDevice {
    std::string uri;          ///< Value for open_input().
    std::string description;  ///< As reported by the system.
};

/// Throws std::runtime_error (unknown scheme, back-end not built, device error).
std::unique_ptr<audio::AudioSource> open_input(const std::string& uri,
                                               const InputOptions& options = {});

/// Capture devices visible to the back-ends built in. Never throws.
std::vector<InputDevice> list_inputs();

/// Back-ends compiled in, e.g. {"wav", "stdin", "alsa"}.
std::vector<std::string> input_backends();

}  // namespace owfax::io
