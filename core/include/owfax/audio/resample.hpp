// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#pragma once

#include "owfax/audio/wav.hpp"

namespace owfax::audio {

/// Copy of audio[from_s, to_s) (clamped to the buffer).
AudioBuffer slice(const AudioBuffer& audio, double from_s, double to_s);

/// Sample-rate conversion: windowed-sinc anti-aliasing low-pass at 0.45 × the lower
/// rate, then interpolation. Intended for archiving (e.g. 48 kHz → 11025 Hz for the
/// test corpus), not real time.
AudioBuffer resample(const AudioBuffer& audio, int out_rate);

}  // namespace owfax::audio
