// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Streaming decoder: audio fed in random-size chunks, as from a sound card.

#include <cmath>
#include <cstdio>
#include <random>

#include "owfax/wefax/modulator.hpp"
#include "owfax/wefax/stream_decoder.hpp"
#include "owfax_test.hpp"

using namespace owfax::wefax;

namespace {

struct Collected {
    int starts = 0;
    int locks = 0;
    int completes = 0;
    int signal_starts = 0;
    int discarded = 0;
    double locked_ppm = 0.0;
    std::vector<owfax::GrayImage> live;  // one per transmission
    std::vector<std::shared_ptr<const DecodeResult>> finals;
};

void feed(StreamDecoder& dec, const owfax::audio::AudioBuffer& audio, Collected& c,
          std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> chunk(1, 4096);
    std::size_t pos = 0;
    auto drain = [&] {
        for (auto& e : dec.take_events()) {
            switch (e.kind) {
                case StreamEvent::Kind::kStart:
                    ++c.starts;
                    if (e.signal_start) ++c.signal_starts;
                    c.live.emplace_back();
                    break;
                case StreamEvent::Kind::kLocked:
                    ++c.locks;
                    c.locked_ppm = e.clock_ppm;
                    break;
                case StreamEvent::Kind::kLine: {
                    auto& img = c.live.back();
                    if (img.width == 0) img.width = e.pixels.size();
                    img.pixels.insert(img.pixels.end(), e.pixels.begin(), e.pixels.end());
                    img.height = img.pixels.size() / img.width;
                    break;
                }
                case StreamEvent::Kind::kComplete:
                    ++c.completes;
                    if (e.discarded) ++c.discarded;
                    c.finals.push_back(e.final);
                    break;
            }
        }
    };
    while (pos < audio.samples.size()) {
        const std::size_t n = std::min(chunk(rng), audio.samples.size() - pos);
        dec.push(&audio.samples[pos], n);
        pos += n;
        drain();
    }
}

/// Receiver noise between charts: an SSB receiver passes ~300-2700 Hz, so the noise
/// is band-pass around the 1900 Hz WEFAX centre (a low-pass noise would demodulate to
/// a mostly black signal with spikes, which looks like phasing pulses).
std::vector<float> noise(int rate, double seconds, std::uint32_t seed) {
    // mt19937 output is specified by the standard, distributions are not: build the
    // noise from raw draws so that every compiler sees the same samples.
    std::mt19937 rng(seed);
    // RBJ band-pass biquad, centre 1900 Hz, Q 0.8 (about 2.4 kHz wide).
    const double w0 = 2.0 * 3.14159265358979 * 1900.0 / rate;
    const double alpha = std::sin(w0) / (2.0 * 0.8);
    const double a0 = 1.0 + alpha;
    const double b0 = alpha / a0;
    const double a1 = -2.0 * std::cos(w0) / a0;
    const double a2 = (1.0 - alpha) / a0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    std::vector<float> out(static_cast<std::size_t>(rate * seconds));
    for (auto& v : out) {
        double sum = 0.0;
        for (int k = 0; k < 4; ++k) sum += static_cast<double>(rng()) / 4294967296.0;
        const double x = (sum - 2.0) * 0.6;
        const double y = b0 * x - b0 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        v = static_cast<float>(y);
    }
    return out;
}

/// Steady tone (an idle carrier, white or black).
std::vector<float> carrier(int rate, double seconds, double hz) {
    std::vector<float> out(static_cast<std::size_t>(rate * seconds));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = 0.5F * static_cast<float>(
                            std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / rate));
    }
    return out;
}

void append(owfax::audio::AudioBuffer& a, const std::vector<float>& s) {
    a.samples.insert(a.samples.end(), s.begin(), s.end());
}

double correlation(const owfax::GrayImage& a, const owfax::GrayImage& b) {
    if (a.width != b.width || a.height < 10 || b.height < 10) return 0.0;
    double best = -1.0;
    for (int shift = -2; shift <= 2; ++shift) {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;
        for (std::size_t y = 2; y + 2 < a.height; ++y) {
            const long yb = static_cast<long>(y) + shift;
            if (yb < 0 || yb >= static_cast<long>(b.height)) continue;
            for (std::size_t x = 0; x < a.width; ++x) {
                const double va = a.at(x, y);
                const double vb = b.at(x, static_cast<std::size_t>(yb));
                sa += va;
                sb += vb;
                saa += va * va;
                sbb += vb * vb;
                sab += va * vb;
                n += 1;
            }
        }
        const double cov = sab / n - (sa / n) * (sb / n);
        const double den =
            std::sqrt((saa / n - (sa / n) * (sa / n)) * (sbb / n - (sb / n) * (sb / n)));
        if (den > 0 && cov / den > best) best = cov / den;
    }
    return best;
}

}  // namespace

OWFAX_TEST(stream_single_transmission) {
    ModulatorOptions mod;
    mod.clock_ppm = 150.0;
    mod.tuning_offset_hz = 30.0;
    const auto chart = make_test_chart(mod.ioc, 150);
    StreamDecoder dec(mod.sample_rate);
    Collected c;
    feed(dec, modulate(chart, mod), c, 1);
    OWFAX_CHECK(c.starts == 1);
    OWFAX_CHECK(c.locks == 1);
    OWFAX_CHECK(c.completes == 1);
    OWFAX_CHECK(!dec.receiving());
    OWFAX_CHECK_NEAR(c.locked_ppm, 150.0, 5.0);
    if (c.live.size() == 1 && c.finals.size() == 1 && c.finals[0]) {
        const double live = correlation(chart, c.live[0]);
        const double final = correlation(chart, c.finals[0]->image);
        std::printf("    live: %zu lines corr %.3f | final: %zu lines corr %.3f ppm %.1f\n",
                    c.live[0].height, live, c.finals[0]->image.height, final,
                    c.finals[0]->report.clock_ppm);
        OWFAX_CHECK(c.live[0].height >= 148 && c.live[0].height <= 151);
        OWFAX_CHECK(live > 0.95);
        OWFAX_CHECK(final > 0.95);
    } else {
        OWFAX_CHECK(false);
    }
}

OWFAX_TEST(stream_two_transmissions) {
    ModulatorOptions mod;
    mod.sample_rate = 8000;
    auto audio = modulate(make_test_chart(mod.ioc, 100), mod);
    mod.ioc = Ioc::k288;
    const auto second = modulate(make_test_chart(mod.ioc, 100), mod);
    audio.samples.insert(audio.samples.end(), second.samples.begin(), second.samples.end());
    StreamDecoder dec(mod.sample_rate);
    Collected c;
    feed(dec, audio, c, 2);
    OWFAX_CHECK(c.starts == 2);
    OWFAX_CHECK(c.completes == 2);
    if (c.finals.size() == 2 && c.finals[0] && c.finals[1]) {
        OWFAX_CHECK(c.finals[0]->report.ioc == Ioc::k576);
        OWFAX_CHECK(c.finals[1]->report.ioc == Ioc::k288);
    }
    if (c.live.size() == 2) {
        OWFAX_CHECK(c.live[0].width == 1810 && c.live[1].width == 905);
    }
}

OWFAX_TEST(stream_forced_start_without_apt) {
    ModulatorOptions mod;
    mod.sample_rate = 8000;
    mod.start_tone_s = 0.0;
    const auto chart = make_test_chart(mod.ioc, 100);
    StreamDecoder dec(mod.sample_rate);
    dec.force_start();
    OWFAX_CHECK(dec.receiving());
    Collected c;
    c.live.emplace_back();  // forced start: kStart is emitted before feeding
    for (auto& e : dec.take_events()) OWFAX_CHECK(e.kind == StreamEvent::Kind::kStart);
    feed(dec, modulate(chart, mod), c, 3);
    OWFAX_CHECK(c.locks == 1);
    OWFAX_CHECK(c.completes == 1);
    if (!c.live.empty()) {
        std::printf("    forced start: %zu live lines, corr %.3f\n", c.live[0].height,
                    correlation(chart, c.live[0]));
        OWFAX_CHECK(correlation(chart, c.live[0]) > 0.9);
    }
}

OWFAX_TEST(stream_starts_on_fax_signal_when_apt_is_missed) {
    // Real cases: GYA 4610 kHz, start tones chopped by QRM; capture started mid-chart.
    ModulatorOptions mod;
    mod.sample_rate = 8000;
    mod.start_tone_s = 0.0;
    mod.clock_ppm = -40.0;
    const auto chart = make_test_chart(mod.ioc, 200);
    owfax::audio::AudioBuffer audio;
    audio.sample_rate = mod.sample_rate;
    append(audio, noise(mod.sample_rate, 30.0, 7));
    append(audio, modulate(chart, mod).samples);
    StreamDecoder dec(mod.sample_rate);
    Collected c;
    feed(dec, audio, c, 4);
    OWFAX_CHECK(c.starts == 1);
    OWFAX_CHECK(c.signal_starts == 1);
    OWFAX_CHECK(c.completes == 1);
    OWFAX_CHECK(c.discarded == 0);
    if (c.finals.size() == 1 && c.finals[0]) {
        const auto& r = c.finals[0]->report;
        const double corr = correlation(chart, c.finals[0]->image);
        std::printf("    signal start: %zu lines, phasing %d, %.1f ppm, corr %.3f\n",
                    c.finals[0]->image.height, r.phasing_found ? 1 : 0, r.clock_ppm, corr);
        OWFAX_CHECK(r.phasing_found);  // started early enough to catch the phasing
        OWFAX_CHECK(corr > 0.95);
    } else {
        OWFAX_CHECK(false);
    }
}

OWFAX_TEST(stream_does_not_start_on_noise_or_carrier) {
    const int rate = 8000;
    owfax::audio::AudioBuffer audio;
    audio.sample_rate = rate;
    append(audio, noise(rate, 120.0, 8));
    append(audio, carrier(rate, 60.0, kWhiteHz));  // DWD keeps a white carrier between charts
    append(audio, carrier(rate, 60.0, kBlackHz));
    append(audio, noise(rate, 60.0, 9));
    StreamDecoder dec(rate);
    Collected c;
    feed(dec, audio, c, 5);
    OWFAX_CHECK(c.starts == 0);
    OWFAX_CHECK(!dec.receiving());
}

OWFAX_TEST(stream_ends_when_signal_turns_to_noise) {
    // Real case: GYA 2026-11-10 08:22, no stop tone, then 25 min of noise.
    ModulatorOptions mod;
    mod.sample_rate = 8000;
    mod.stop_tone_s = 0.0;
    mod.trailer_s = 0.0;
    const std::size_t lines = 200;
    const auto chart = make_test_chart(mod.ioc, lines);
    auto audio = modulate(chart, mod);
    append(audio, noise(mod.sample_rate, 120.0, 10));
    StreamDecoder dec(mod.sample_rate);
    Collected c;
    feed(dec, audio, c, 6);
    OWFAX_CHECK(c.starts == 1);
    OWFAX_CHECK(c.completes == 1);
    OWFAX_CHECK(!dec.receiving());
    if (c.finals.size() == 1 && c.finals[0]) {
        const std::size_t h = c.finals[0]->image.height;
        std::printf("    ended on noise: %zu lines for a %zu-line chart\n", h, lines);
        OWFAX_CHECK(h >= lines - 2);
        OWFAX_CHECK(h <= lines + 30);  // at most ~15 s of noise kept, not 120 s
    } else {
        OWFAX_CHECK(false);
    }
}

OWFAX_TEST(stream_discards_short_fragment_without_apt) {
    // Real case: GYA 07:21, the noisy end of a chart came out as two short pieces.
    ModulatorOptions mod;
    mod.sample_rate = 8000;
    mod.start_tone_s = 0.0;
    mod.phasing_s = 0.0;
    mod.stop_tone_s = 0.0;
    mod.trailer_s = 0.0;
    owfax::audio::AudioBuffer audio;
    audio.sample_rate = mod.sample_rate;
    append(audio, noise(mod.sample_rate, 20.0, 11));
    // Dense content like a real chart (spread >= 0.15); the sparse test chart is not.
    owfax::GrayImage dense(static_cast<std::size_t>(pixels_per_line(mod.ioc)), 80, 255);
    for (std::size_t y = 0; y < dense.height; ++y) {
        for (std::size_t x = 0; x < dense.width; ++x) {
            dense.at(x, y) = ((x / 24) * 7919 + y / 40) % 5 < 2 ? 0 : 255;
        }
    }
    append(audio, modulate(dense, mod).samples);  // 40 s
    append(audio, noise(mod.sample_rate, 60.0, 12));
    StreamDecoder dec(mod.sample_rate);
    Collected c;
    feed(dec, audio, c, 7);
    std::printf("    fragment: %d start(s), %d discarded\n", c.starts, c.discarded);
    OWFAX_CHECK(c.starts >= 1);
    OWFAX_CHECK(c.starts == c.discarded);
    for (const auto& f : c.finals) OWFAX_CHECK(!f);
}

OWFAX_TEST(stream_auto_start_can_be_disabled) {
    ModulatorOptions mod;
    mod.sample_rate = 8000;
    mod.start_tone_s = 0.0;
    DecodeOptions opt;
    opt.start_without_apt = false;
    StreamDecoder dec(mod.sample_rate, opt);
    Collected c;
    feed(dec, modulate(make_test_chart(mod.ioc, 100), mod), c, 8);
    OWFAX_CHECK(c.starts == 0);
}

OWFAX_TEST(stream_splits_charts_whose_apt_tones_were_lost) {
    // Real case: GYA 2026-11-10 07:35, stop and start tones lost in QRM, 41 s of noise
    // between two charts. The second one is recognised by its phasing.
    ModulatorOptions mod;
    mod.sample_rate = 8000;
    mod.stop_tone_s = 0.0;
    mod.trailer_s = 0.0;
    const auto chart = make_test_chart(mod.ioc, 200);
    auto audio = modulate(chart, mod);
    append(audio, noise(mod.sample_rate, 40.0, 13));
    mod.start_tone_s = 0.0;
    mod.stop_tone_s = 5.0;
    append(audio, modulate(chart, mod).samples);
    StreamDecoder dec(mod.sample_rate);
    Collected c;
    feed(dec, audio, c, 9);
    OWFAX_CHECK(c.starts == 2);
    OWFAX_CHECK(c.completes == 2);
    if (c.finals.size() == 2 && c.finals[0] && c.finals[1]) {
        for (const auto& f : c.finals) {
            std::printf("    %zu lines, phasing %d, corr %.3f\n", f->image.height,
                        f->report.phasing_found ? 1 : 0, correlation(chart, f->image));
            OWFAX_CHECK(f->report.phasing_found);
            OWFAX_CHECK(correlation(chart, f->image) > 0.95);
        }
    } else {
        OWFAX_CHECK(false);
    }
}
