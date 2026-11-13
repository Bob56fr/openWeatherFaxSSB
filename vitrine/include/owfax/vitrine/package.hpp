// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Chart package of the web showcase (docs/VITRINE-WEB.md § 4): one received chart with
// its real audio, its measurements and its metadata, ready to be encoded and sent to
// the WordPress plugin. Building a package never sends anything; the selection rules
// (PUB-020 … PUB-024) only say whether it may be published.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "owfax/audio/wav.hpp"
#include "owfax/image/gray_image.hpp"
#include "owfax/vitrine/geo.hpp"
#include "owfax/vitrine/telemetry.hpp"
#include "owfax/wefax/decoder.hpp"

namespace owfax::vitrine {

/// Transmitting station, from the catalogue (catalog/stations/*.json).
struct StationRef {
    std::string id;
    std::string name;
    std::string callsign;
    double assigned_khz = 0.0;
    std::optional<double> power_kw;
    GeoPoint position;
    double shift_hz = 800.0;
    bool publish_allowed = false;  ///< Catalogue "publication.allowed" (PUB-024).
    std::string attribution;       ///< Text to show with every chart (PUB-024).
    std::string terms_url;
};

/// Receiving station (PUB-001).
struct ReceiverRef {
    std::string locator;  ///< Shown on the site (6 characters).
    GeoPoint position;    ///< Used for distances only, never published.
    std::string rig;
    std::string input;
    std::optional<double> dial_khz;
    std::string mode;
};

struct SelectionRules {
    double min_quality = 0.60;             ///< PUB-021.
    std::size_t min_lines = 600;           ///< PUB-020 without APT stop: 5 min at 120 lpm.
    double max_saturated_fraction = 0.15;  ///< SAT-010 (lot 2: no detector yet).
};

struct Selection {
    bool publish = false;
    std::vector<std::string> reasons;  ///< Why not, one short code per rule failed.
};

/// Inputs for one chart found in a recording.
struct ChartSource {
    const audio::AudioBuffer* recording = nullptr;  ///< The whole capture.
    std::int64_t recording_start_unix = 0;          ///< Unix time of its first sample.
    const wefax::DecodeResult* raw = nullptr;       ///< From decode_all on `recording`.
    /// Recording time limits for the audio cut: the neighbouring charts (or the ends of
    /// the recording). The cut never crosses them.
    double min_s = 0.0;
    double max_s = 0.0;
};

struct Package {
    std::string chart_id;               ///< <YYYYMMDDTHHMMZ>_<label>_<kHz>kHz
    std::int64_t chart_start_unix = 0;  ///< APT start tone, or phasing estimate without APT.
    double chart_start_s = 0.0;         ///< Same instant, in `audio`.
    audio::AudioBuffer audio;    ///< The chart's audio, from before the APT to after the stop.
    double audio_start_s = 0.0;  ///< Where `audio` starts in the recording.
    GrayImage raw;
    std::optional<GrayImage> enhanced;
    wefax::DecodeReport report;  ///< Times relative to `audio` (not to the recording).
    Telemetry telemetry;
    GreatCircle path;
    std::string daylight;  ///< "day", "night" or "mixed" at the middle of the chart.
    Selection selection;
};

/// Cuts the chart's audio, measures it and applies the selection rules.
/// `enhanced_options`: if set, the cut audio is decoded again with them (UI-008).
/// Throws std::invalid_argument on inconsistent input.
Package build_package(const ChartSource& src, const StationRef& station,
                      const ReceiverRef& receiver, const std::string& label,
                      const std::vector<SmeterReading>& smeter, const SelectionRules& rules,
                      const std::optional<wefax::DecodeOptions>& enhanced_options);

/// Selection rules PUB-020 … PUB-024 on a decoded chart.
Selection select(const wefax::DecodeReport& report, std::size_t lines, double saturated_fraction,
                 const StationRef& station, const SelectionRules& rules);

/// Mean of the per-line quality, 0 … 1.
double mean_quality(const wefax::DecodeReport& report);

/// File names inside the package directory, as written by the packaging tool.
struct PackageFiles {
    std::string raw = "raw.webp";
    std::string enhanced = "enh.webp";  ///< Empty if no enhanced image.
    std::string thumb = "thumb.webp";
    std::string audio = "audio.m4a";
    std::string telemetry = "telemetry.json";
};

/// meta.json (schema owfax-vitrine/0.1, docs/schemas/owfax-vitrine-0.1.schema.json).
std::string meta_json(const Package& p, const StationRef& station, const ReceiverRef& receiver,
                      const PackageFiles& files);

/// telemetry.json (schema owfax-vitrine-telemetry/0.1): columns, one value per second.
std::string telemetry_json(const Package& p);

}  // namespace owfax::vitrine
