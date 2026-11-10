// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Reception engine (spec BG-001, DSP-007, UI-008): feeds a continuous audio stream to
// the streaming decoder and writes every received chart to an output directory:
//
//   <stem>_live.png   live preview, rewritten while the chart is received
//   <stem>.png        raw final image (reference, DSP-030)
//   <stem>_enh.png    final image with the validated enhancements (UI-009)
//   <stem>.json       metadata (times, clock, tuning, quality, treatments)
//
// <stem> = <YYYYMMDDTHHMMZ>_<label>, from the UTC time of the start of the chart.
// Single-threaded: call push() from one thread (the daemon's processing thread).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "owfax/wefax/decoder.hpp"

namespace owfax::engine {

struct ReceiverConfig {
    std::string output_dir = ".";
    std::string label = "wefax";        ///< Station or frequency label, used in file names.
    wefax::DecodeOptions options;       ///< Raw decode (reference image).
    bool enhanced = true;               ///< Also write <stem>_enh.png.
    std::size_t live_flush_lines = 50;  ///< Rewrite <stem>_live.png every N lines (0 = never).
};

/// Summary of one received chart, also written as <stem>.json.
struct ChartInfo {
    std::string stem;
    std::int64_t start_utc = 0;  ///< Unix time of the start of the transmission.
    double start_s = 0.0;        ///< Stream time of the start.
    double end_s = 0.0;          ///< Stream time of the end.
    wefax::Ioc ioc = wefax::Ioc::k576;
    bool apt_start = false;           ///< Started on a detected APT tone.
    bool signal_start = false;        ///< Started on a WEFAX signal without APT (DSP-006).
    bool phasing_found = false;       ///< Phasing pulses locked the line origin.
    std::size_t live_lines = 0;       ///< Lines shown in the live preview.
    std::size_t lines = 0;            ///< Lines of the final image (0 = no final image).
    double clock_ppm = 0.0;           ///< Final clock correction.
    double tuning_offset_hz = 0.0;    ///< Measured mistuning.
    double quality = 0.0;             ///< Mean line quality of the final image, 0 … 1.
    std::size_t clicks_repaired = 0;  ///< Enhanced image: clicks suppressed.
    std::vector<std::string> files;   ///< Files written (names, relative to output_dir).
};

class Receiver {
public:
    /// `stream_start_utc`: Unix time of the first sample; stream times are added to it.
    Receiver(int sample_rate, ReceiverConfig config, std::int64_t stream_start_utc);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    /// Feeds audio (mono, [-1, 1]).
    void push(const float* samples, std::size_t count);

    /// Manual start when the APT start tone was missed (spec DSP-006).
    void force_start();

    /// Ends the stream: a chart in progress is finalised and written.
    void finish();

    bool receiving() const noexcept;

    /// Stem of the chart being received, empty when idle.
    std::string current_stem() const;

    /// Charts completed since the last call.
    std::vector<ChartInfo> take_completed();

    /// Progress messages (start, lock, files written), for the journal. Optional.
    std::function<void(const std::string&)> log;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// "20261110T0704Z" for a Unix time.
std::string utc_stamp(std::int64_t unix_time);

/// "2026-11-10 07:04:12Z" for a Unix time (journal lines).
std::string utc_text(std::int64_t unix_time);

/// Escapes a string for a JSON document.
std::string json_escape(const std::string& s);

}  // namespace owfax::engine
