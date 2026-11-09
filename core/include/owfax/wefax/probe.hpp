// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Quick signal assessment of a short audio excerpt: is there usable WEFAX here?
// Used to rank frequencies (spec CAT-007, CAT-105) and to guide level setting (SRC-007).

#pragma once

#include "owfax/audio/wav.hpp"

namespace owfax::wefax {

struct ProbeResult {
    double rms_dbfs = -120.0;       ///< Input level.
    double clipped_fraction = 0.0;  ///< Samples at or near full scale.
    double band_fraction = 0.0;     ///< Energy in 1400–2400 Hz / energy in 300–3000 Hz.
    double fm_in_range = 0.0;       ///< Demodulated samples inside 1400–2400 Hz.
    double apt_start_ratio = 0.0;   ///< Max 300/675 Hz tone ratio over 0.25 s blocks.
    double apt_stop_ratio = 0.0;    ///< Max 450 Hz tone ratio over 0.25 s blocks.
    double score = 0.0;             ///< 0 … 1 overall WEFAX likelihood × quality.
};

ProbeResult probe(const audio::AudioBuffer& audio);

}  // namespace owfax::wefax
