// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Disturbances of a received chart (docs/VITRINE-WEB.md § 5, lot 2): stretches of at
// least 15 s where the image is lost, and what caused them.
//
// A *local* disturbance comes from the receiving site: the receiver overloaded by a
// transmitter of the station, or interference from equipment on board (MPPT solar
// regulators, autopilot rams driven by PWM, inverters...). Natural *fading* is not
// local. The decision uses the audio only, so it works the same with an SDR dongle or
// a transceiver; the S-meter is one more clue when there is one.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "owfax/audio/wav.hpp"
#include "owfax/vitrine/telemetry.hpp"
#include "owfax/wefax/decoder.hpp"

namespace owfax::vitrine {

enum class Cause : std::uint8_t {
    kFading = 1,                 ///< Signal faded into the noise: propagation, not local.
    kOverload = 2,               ///< Receiver driven too hard: clipping, level jump, S-meter.
    kImpulsiveInterference = 3,  ///< Impulses or switching buzz (PWM, MPPT, ignition...).
    kBroadbandInterference = 4,  ///< Noise switched on and off abruptly (local hash).
};

const char* cause_name(Cause c);
inline bool is_local(Cause c) {
    return c != Cause::kFading;
}

struct DisturbanceRules {
    double min_run_s = 15.0;  ///< SAT-001: shortest disturbance.
    /// SAT-001: a line below this is lost. Noise alone scores 0.6–0.7 (most of the FM
    /// noise falls inside the black–white range), a clean line 1.
    float bad_line_quality = 0.80F;
    double max_gap_s = 2.0;           ///< Good lines shorter than this do not split a zone.
    double clip_fraction = 1e-3;      ///< Overload: clipped samples.
    double level_jump_db = 6.0;       ///< Overload: audio level above the chart's own.
    double smeter_high_db_s9 = 30.0;  ///< Overload: S-meter at or above S9+30 dB ...
    double smeter_rise_db = 15.0;     ///< ... and that much above the chart's own.
    double kurtosis = 6.0;            ///< Impulsive: Gaussian noise is 3.
    double periodicity = 0.5;         ///< Switching buzz: envelope repeats at 2–200 ms.
    double flatness = 0.5;            ///< Broadband: spectral flatness of noise.
    double edge_s = 2.0;              ///< Abrupt: the image vanishes and returns this fast.
};

struct Zone {
    double start_s = 0.0;  ///< In the chart's audio.
    double end_s = 0.0;
    std::size_t first_line = 0;
    std::size_t last_line = 0;
    Cause cause = Cause::kFading;
    std::vector<std::string> indicators;  ///< Clues found: "clipping", "level_jump", ...
};

struct DisturbanceReport {
    std::vector<Zone> zones;
    double local_fraction = 0.0;           ///< Image lines in local disturbances (SAT-010).
    double fading_fraction = 0.0;          ///< Image lines lost to fading.
    std::vector<std::uint8_t> per_second;  ///< 0 or the Cause value, one per second of audio.
};

/// Finds the disturbances of a chart. `report` times are relative to the audio the
/// telemetry was measured on; `lines` is the height of the image.
DisturbanceReport detect_disturbances(const wefax::DecodeReport& report, std::size_t lines,
                                      const Telemetry& telemetry,
                                      const DisturbanceRules& rules = {});

/// Replaces the audio of the local disturbances with silence of the same length
/// (PUB-031): a receiver overloaded by the station's own transmitter may play its
/// modulation, the operator's voice. Short fades avoid clicks. Fading is kept.
void mute_local_disturbances(audio::AudioBuffer& audio, const DisturbanceReport& report);

}  // namespace owfax::vitrine
