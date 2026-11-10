// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// ALSA capture back-end (Linux: Debian, Raspberry Pi OS). Internal header.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "owfax/io/audio_input.hpp"

namespace owfax::io {

#if defined(OWFAX_HAVE_ALSA)
std::unique_ptr<audio::AudioSource> open_alsa(const std::string& device, int sample_rate);
std::vector<InputDevice> list_alsa();
#endif

}  // namespace owfax::io
