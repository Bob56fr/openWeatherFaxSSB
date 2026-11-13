// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Disturbances of a received chart (docs/VITRINE-WEB.md § 5, lot 2): receiver overload
// and interference on board must be told from natural fading, from the audio only.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "owfax/vitrine/disturbance.hpp"
#include "owfax/vitrine/package.hpp"
#include "owfax/vitrine/telemetry.hpp"
#include "owfax/wefax/decoder.hpp"
#include "owfax/wefax/modulator.hpp"
#include "owfax_test.hpp"

#ifndef OWFAX_CORPUS_TSV
#error "OWFAX_CORPUS_TSV must be defined by CMake"
#endif

using namespace owfax;
using namespace owfax::vitrine;

namespace {

constexpr double kLeadS = 20.0;     // receiver noise before the chart
constexpr double kImageAtS = 35.0;  // after the start of the chart: APT 5 s + phasing 30 s
constexpr double kSnrDb = 25.0;
constexpr std::size_t kLines = 300;  // 150 s of image

/// Noise standard deviation giving `snr_db` in 2.5 kHz for the modulator's 0.5 sine.
double noise_sigma(double snr_db, int rate) {
    const double in_band = 0.125 / std::pow(10.0, snr_db / 10.0);
    return std::sqrt(in_band * (rate / 2.0) / 2500.0);
}

/// A capture: noise, then a clean chart plus the same receiver noise. `disturb` gets
/// the samples of [from_s, to_s) of the image (seconds after the image start).
struct Capture {
    audio::AudioBuffer audio;
    std::vector<float> noise;  // the receiver noise alone, for the fading case
    double image_s = 0.0;
};

Capture make_capture() {
    wefax::ModulatorOptions m;
    const auto chart = wefax::modulate(wefax::make_test_chart(wefax::Ioc::k576, kLines), m);
    Capture c;
    c.audio.sample_rate = chart.sample_rate;
    const auto lead = static_cast<std::size_t>(kLeadS * chart.sample_rate);
    c.audio.samples.assign(lead, 0.0F);
    c.audio.samples.insert(c.audio.samples.end(), chart.samples.begin(), chart.samples.end());
    std::mt19937 rng(11);
    std::normal_distribution<float> g(0.0F,
                                      static_cast<float>(noise_sigma(kSnrDb, chart.sample_rate)));
    c.noise.resize(c.audio.samples.size());
    for (std::size_t i = 0; i < c.noise.size(); ++i) {
        c.noise[i] = g(rng);
        c.audio.samples[i] += c.noise[i];
    }
    c.image_s = kLeadS + kImageAtS;
    return c;
}

std::size_t sample_at(const Capture& c, double image_offset_s) {
    return static_cast<std::size_t>((c.image_s + image_offset_s) * c.audio.sample_rate);
}

/// Replaces [from, to) seconds of the image with `gen(i)` (i = sample index).
void replace(Capture& c, double from, double to, const std::function<float(std::size_t)>& gen) {
    for (std::size_t i = sample_at(c, from); i < sample_at(c, to); ++i) c.audio.samples[i] = gen(i);
}

struct Result {
    DisturbanceReport report;
    std::size_t lines = 0;
};

Result analyse(const Capture& c) {
    const auto charts = wefax::decode_all(c.audio);
    Result out;
    if (charts.size() != 1) {
        std::printf("    %zu transmissions decoded, expected 1\n", charts.size());
        return out;
    }
    const auto& r = charts[0].report;
    SnrBands bands;
    bands.signal_lo_hz = std::min(r.black_hz, r.white_hz) - 100.0;
    bands.signal_hi_hz = std::max(r.black_hz, r.white_hz) + 100.0;
    const auto t = measure(c.audio, bands);
    out.lines = charts[0].image.height;
    out.report = detect_disturbances(r, out.lines, t);
    for (const Zone& z : out.report.zones) {
        std::string ind;
        for (const auto& s : z.indicators) ind += " " + s;
        std::printf("    zone %.1f-%.1f s, lines %zu-%zu: %s (%s )\n", z.start_s, z.end_s,
                    z.first_line, z.last_line, cause_name(z.cause), ind.c_str());
    }
    return out;
}

bool one_zone(const Result& r, Cause cause, double from, double to) {
    if (r.report.zones.size() != 1) return false;
    const Zone& z = r.report.zones[0];
    const double a = z.start_s - kLeadS - kImageAtS, b = z.end_s - kLeadS - kImageAtS;
    return z.cause == cause && std::fabs(a - from) < 3.0 && std::fabs(b - to) < 3.0;
}

}  // namespace

OWFAX_TEST(disturbance_none_on_a_clean_chart) {
    const auto r = analyse(make_capture());
    OWFAX_CHECK(r.lines > 0);
    OWFAX_CHECK(r.report.zones.empty());
    OWFAX_CHECK(r.report.local_fraction == 0.0);
}

OWFAX_TEST(disturbance_overload_clipping) {
    // Own transmitter: the receiver is blocked, its audio pinned at full scale.
    Capture c = make_capture();
    std::mt19937 rng(3);
    std::normal_distribution<float> loud(0.0F, 1.5F);
    replace(c, 40.0, 70.0, [&](std::size_t) { return std::clamp(loud(rng), -1.0F, 1.0F); });
    const auto r = analyse(c);
    OWFAX_CHECK(one_zone(r, Cause::kOverload, 40.0, 70.0));
    OWFAX_CHECK_NEAR(r.report.local_fraction, 30.0 / 150.0, 0.03);
}

OWFAX_TEST(disturbance_switching_interference) {
    // PWM hash (MPPT regulator, autopilot ram): sharp impulses every 2.5 ms (400 Hz)
    // drowning the fax.
    Capture c = make_capture();
    std::mt19937 rng(5);
    std::normal_distribution<float> hiss(0.0F, 0.05F);
    const auto period = static_cast<std::size_t>(c.audio.sample_rate / 400);
    replace(c, 50.0, 80.0, [&](std::size_t i) {
        const std::size_t k = i % period;
        const float spike = k < 4 ? 0.9F * std::exp(-static_cast<float>(k)) : 0.0F;
        return hiss(rng) + spike;
    });
    const auto r = analyse(c);
    OWFAX_CHECK(one_zone(r, Cause::kImpulsiveInterference, 50.0, 80.0));
}

OWFAX_TEST(disturbance_broadband_switched_on_and_off) {
    // An SDR overloaded behind its software AGC: plain noise at an ordinary level, but
    // switched on and off within a second.
    Capture c = make_capture();
    std::mt19937 rng(9);
    std::normal_distribution<float> hash(0.0F, 0.2F);
    replace(c, 60.0, 90.0, [&](std::size_t) { return hash(rng); });
    const auto r = analyse(c);
    OWFAX_CHECK(one_zone(r, Cause::kBroadbandInterference, 60.0, 90.0));
}

OWFAX_TEST(disturbance_fading_is_not_local) {
    // The fax fades at −2 dB/s down to −50 dB, stays 25 s, and comes back at +2 dB/s;
    // the receiver noise goes on. Natural: published, never muted.
    Capture c = make_capture();
    const double fall = 25.0, hold = 25.0;  // 50 dB at 2 dB/s = 25 s
    const double t0 = 30.0, t1 = t0 + fall, t2 = t1 + hold, t3 = t2 + fall;
    const double rate = c.audio.sample_rate;
    for (std::size_t i = sample_at(c, t0); i < sample_at(c, t3); ++i) {
        const double t = static_cast<double>(i) / rate - c.image_s;
        double db = 0.0;
        if (t < t1)
            db = -2.0 * (t - t0);
        else if (t < t2)
            db = -50.0;
        else
            db = -50.0 + 2.0 * (t - t2);
        const float signal = c.audio.samples[i] - c.noise[i];
        c.audio.samples[i] = signal * static_cast<float>(std::pow(10.0, db / 20.0)) + c.noise[i];
    }
    const auto r = analyse(c);
    OWFAX_CHECK(!r.report.zones.empty());
    for (const Zone& z : r.report.zones) OWFAX_CHECK(z.cause == Cause::kFading);
    OWFAX_CHECK(r.report.local_fraction == 0.0);
    OWFAX_CHECK(r.report.fading_fraction > 0.1);
}

OWFAX_TEST(disturbance_shorter_than_15_s_is_ignored) {
    Capture c = make_capture();
    std::mt19937 rng(4);
    std::normal_distribution<float> loud(0.0F, 1.5F);
    replace(c, 40.0, 50.0, [&](std::size_t) { return std::clamp(loud(rng), -1.0F, 1.0F); });
    const auto r = analyse(c);
    OWFAX_CHECK(r.report.zones.empty());
}

OWFAX_TEST(disturbance_long_overload_rejects_and_mutes) {
    // 90 s of the 150 s image lost to the station's own transmitter.
    Capture c = make_capture();
    std::mt19937 rng(8);
    std::normal_distribution<float> loud(0.0F, 1.5F);
    replace(c, 30.0, 120.0, [&](std::size_t) { return std::clamp(loud(rng), -1.0F, 1.0F); });
    const auto charts = wefax::decode_all(c.audio);
    OWFAX_CHECK(charts.size() == 1);
    if (charts.size() != 1) return;
    ChartSource src;
    src.recording = &c.audio;
    src.recording_start_unix = 1794320645;
    src.raw = &charts[0];
    src.max_s = c.audio.duration_s();
    ReceiverRef rx;
    StationRef st;
    st.assigned_khz = 13882.5;
    st.publish_allowed = true;
    st.attribution = "test";
    SelectionRules rules;
    rules.min_lines = 200;
    rules.min_quality = 0.0;  // judge the disturbance rule alone
    const auto p = build_package(src, st, rx, "TEST", {}, rules, std::nullopt);
    OWFAX_CHECK(!p.selection.publish);
    OWFAX_CHECK(p.selection.reasons == std::vector<std::string>{"local_disturbance"});
    OWFAX_CHECK(p.disturbances.local_fraction > 0.5);
    // The audio of the zone is silent, the rest untouched.
    OWFAX_CHECK(p.disturbances.zones.size() == 1);
    if (p.disturbances.zones.size() == 1) {
        const Zone& z = p.disturbances.zones[0];
        const double rate = p.audio.sample_rate;
        double inside = 0.0, before = 0.0;
        for (auto i = static_cast<std::size_t>((z.start_s + 1) * rate);
             i < static_cast<std::size_t>((z.end_s - 1) * rate); ++i) {
            inside = std::max(inside, static_cast<double>(std::fabs(p.audio.samples[i])));
        }
        for (auto i = static_cast<std::size_t>((z.start_s - 5) * rate);
             i < static_cast<std::size_t>((z.start_s - 1) * rate); ++i) {
            before = std::max(before, static_cast<double>(std::fabs(p.audio.samples[i])));
        }
        OWFAX_CHECK(inside == 0.0);
        OWFAX_CHECK(before > 0.3);
        // The measurements keep the real levels (taken before muting).
        OWFAX_CHECK(p.telemetry.audio_dbfs[static_cast<std::size_t>(z.start_s + 10)] > -3.0F);
    }
}

OWFAX_TEST(disturbance_no_local_zone_on_real_receptions) {
    // False alarms: none of the real receptions of the corpus was disturbed locally.
    const char* dir = std::getenv("OWFAX_CORPUS_DIR");
    if (dir == nullptr) {
        std::printf("    skipped: OWFAX_CORPUS_DIR not set (see tools/corpus/fetch.sh)\n");
        return;
    }
    std::ifstream tsv(OWFAX_CORPUS_TSV);
    std::string line;
    std::getline(tsv, line);  // header
    int charts = 0;
    while (std::getline(tsv, line)) {
        std::stringstream ss(line);
        std::string field;
        std::vector<std::string> f;
        while (std::getline(ss, field, '\t')) f.push_back(field);
        if (f.size() < 10) continue;
        const auto audio = audio::read_wav(std::string(dir) + "/" + f[0]);
        wefax::DecodeOptions opt;
        opt.shift_hz = std::stod(f[9]);
        const auto d = wefax::decode(audio, opt);
        SnrBands bands;
        bands.signal_lo_hz = std::min(d.report.black_hz, d.report.white_hz) - 100.0;
        bands.signal_hi_hz = std::max(d.report.black_hz, d.report.white_hz) + 100.0;
        const auto rep = detect_disturbances(d.report, d.image.height, measure(audio, bands));
        for (const Zone& z : rep.zones) {
            std::printf("    %s: %.0f-%.0f s %s\n", f[0].c_str(), z.start_s, z.end_s,
                        cause_name(z.cause));
        }
        OWFAX_CHECK(rep.local_fraction == 0.0);
        ++charts;
    }
    std::printf("    %d real receptions checked\n", charts);
}
