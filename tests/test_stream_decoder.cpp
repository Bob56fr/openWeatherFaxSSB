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
