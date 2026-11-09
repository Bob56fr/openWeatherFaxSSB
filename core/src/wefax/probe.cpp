// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/wefax/probe.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "owfax/dsp/fir.hpp"
#include "owfax/dsp/fm_demod.hpp"
#include "owfax/wefax_params.hpp"

namespace owfax::wefax {

ProbeResult probe(const audio::AudioBuffer& audio) {
    ProbeResult r;
    const std::size_t n = audio.samples.size();
    if (audio.sample_rate <= 0 || n == 0) return r;
    const auto rate = static_cast<double>(audio.sample_rate);

    double energy = 0.0;
    std::size_t clipped = 0;
    for (float s : audio.samples) {
        energy += static_cast<double>(s) * static_cast<double>(s);
        if (std::fabs(s) >= 0.98F) ++clipped;
    }
    const double nd = static_cast<double>(n);
    r.rms_dbfs = 10.0 * std::log10(std::max(energy / nd, 1e-12));
    r.clipped_fraction = static_cast<double>(clipped) / nd;

    // Coarse spectrum with Goertzel bins every 50 Hz, on 0.5 s blocks.
    const auto block = static_cast<std::size_t>(rate * 0.5);
    double in_band = 0.0;
    double total = 0.0;
    for (std::size_t pos = 0; block > 0 && pos + block <= n; pos += block) {
        for (double f = 300.0; f <= 3000.0; f += 50.0) {
            const double p = dsp::tone_ratio(&audio.samples[pos], block, f, rate);
            total += p;
            if (f >= 1400.0 && f <= 2400.0) in_band += p;
        }
    }
    r.band_fraction = total > 0.0 ? in_band / total : 0.0;

    // FM consistency and APT tones on the demodulated signal.
    const auto decim = std::max<std::size_t>(1, static_cast<std::size_t>(rate / 8000.0));
    dsp::FmDemodulator demod(rate, kCenterHz, 1200.0, decim);
    std::vector<float> freq;
    demod.process(audio.samples.data(), n, freq);
    std::size_t in_range = 0;
    for (float f : freq) {
        if (f >= 1400.0F && f <= 2400.0F) ++in_range;
    }
    r.fm_in_range =
        freq.empty() ? 0.0 : static_cast<double>(in_range) / static_cast<double>(freq.size());

    const double frate = demod.output_rate();
    const auto tblock = static_cast<std::size_t>(frate * 0.25);
    for (std::size_t pos = 0; tblock > 0 && pos + tblock <= freq.size(); pos += tblock) {
        r.apt_start_ratio =
            std::max({r.apt_start_ratio, dsp::tone_ratio(&freq[pos], tblock, 300.0, frate),
                      dsp::tone_ratio(&freq[pos], tblock, 675.0, frate)});
        r.apt_stop_ratio =
            std::max(r.apt_stop_ratio, dsp::tone_ratio(&freq[pos], tblock, kAptStopHz, frate));
    }

    // White noise gives fm_in_range ≈ 0.6 and band_fraction ≈ 0.4; clean WEFAX ≈ 1.0 for both.
    const double fm_score = std::clamp((r.fm_in_range - 0.6) / 0.35, 0.0, 1.0);
    const double band_score = std::clamp((r.band_fraction - 0.4) / 0.4, 0.0, 1.0);
    const double level_ok = (r.rms_dbfs > -50.0 && r.clipped_fraction < 0.001) ? 1.0 : 0.5;
    r.score = fm_score * band_score * level_ok;
    return r;
}

}  // namespace owfax::wefax
