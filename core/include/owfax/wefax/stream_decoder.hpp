// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Streaming WEFAX decoder (spec DSP-007, BG-001): audio is pushed in blocks of any
// size, as it arrives from the sound card or the SDR; events report the start of a
// transmission, the phasing lock, each image line (live preview) and the end of the
// transmission with the final image.
//
// The live lines use the clock estimated on the phasing. The final image is computed
// at the end by the full offline pipeline (decode_frequency) on the transmission's
// demodulated signal, which is kept at ~8 kHz (≈ 38 MB for 20 minutes).

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "owfax/wefax/decoder.hpp"

namespace owfax::wefax {

struct StreamEvent {
    enum class Kind {
        kStart,     ///< APT start tone, WEFAX signal without APT, or forced start.
        kLocked,    ///< Phasing analysed: line period and origin known, lines follow.
        kLine,      ///< One live image line.
        kComplete,  ///< Transmission ended (APT stop, forced stop, or length limit).
    };
    Kind kind = Kind::kStart;
    double time_s = 0.0;               ///< Stream time of the event (s since the first sample).
    Ioc ioc = Ioc::k576;               ///< kStart.
    bool apt_start = false;            ///< kStart: true if from a detected tone.
    bool signal_start = false;         ///< kStart: WEFAX signal found without APT (DSP-006).
    double clock_ppm = 0.0;            ///< kLocked.
    double tuning_offset_hz = 0.0;     ///< kLocked.
    bool phasing_found = false;        ///< kLocked.
    std::size_t line_index = 0;        ///< kLine.
    std::vector<std::uint8_t> pixels;  ///< kLine: pixels_per_line(ioc) values.
    float quality = 0.0F;              ///< kLine: 1 = clean … 0 = unusable.
    std::shared_ptr<const DecodeResult> final;  ///< kComplete: final image (may be null).
    /// kComplete: same transmission decoded with the enhancement options (may be null;
    /// only when enhanced options were given to the decoder). Spec UI-008.
    std::shared_ptr<const DecodeResult> enhanced;
    /// kComplete: the transmission was a fragment, not a chart (started without APT and
    /// shorter than one minute); no final image. Spec DSP-006.
    bool discarded = false;
};

class StreamDecoder {
public:
    /// `enhanced`: if set, each transmission is also decoded with these options at the
    /// end (before/after comparison, spec UI-009). Costs one more offline decode.
    StreamDecoder(int sample_rate, DecodeOptions options = {},
                  std::optional<DecodeOptions> enhanced = std::nullopt);
    ~StreamDecoder();
    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    /// Feeds audio samples (mono, [-1, 1]).
    void push(const float* samples, std::size_t count);

    /// Returns and clears the events produced since the last call.
    std::vector<StreamEvent> take_events();

    /// Manual start when the APT start was missed (spec DSP-006).
    void force_start();

    /// Ends the current transmission now and produces its final image.
    void force_stop();

    /// True between kStart and kComplete.
    bool receiving() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace owfax::wefax
