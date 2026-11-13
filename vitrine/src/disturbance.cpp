// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/vitrine/disturbance.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace owfax::vitrine {
namespace {

constexpr double kFadeS = 0.02;       // mute ramps
constexpr double kEdgeSearchS = 6.0;  // where to look for a zone's edges

double median(std::vector<float> v) {
    if (v.empty()) return std::nan("");
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return static_cast<double>(*mid);
}

std::vector<float> pick(const std::vector<float>& v, const std::vector<std::size_t>& idx) {
    std::vector<float> out;
    for (std::size_t i : idx) {
        if (i < v.size()) out.push_back(v[i]);
    }
    return out;
}

double mean(const std::vector<float>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (float x : v) s += static_cast<double>(x);
    return s / static_cast<double>(v.size());
}

struct Run {
    std::size_t first;
    std::size_t last;
};

/// Runs of lost lines, short good gaps bridged.
std::vector<Run> lost_runs(const std::vector<float>& q, float threshold, std::size_t max_gap) {
    std::vector<Run> runs;
    std::optional<Run> cur;
    std::size_t gap = 0;
    for (std::size_t i = 0; i < q.size(); ++i) {
        if (q[i] < threshold) {
            if (cur) {
                cur->last = i;
            } else {
                cur = Run{i, i};
            }
            gap = 0;
        } else if (cur && ++gap > max_gap) {
            runs.push_back(*cur);
            cur.reset();
            gap = 0;
        }
    }
    if (cur) runs.push_back(*cur);
    return runs;
}

/// How many seconds the fax takes to vanish (falling) or to come back (rising) around
/// `edge_s`, judged on the WEFAX band fraction; empty if it cannot be told.
std::optional<double> transition_s(const std::vector<float>& band, double edge_s, double high,
                                   double low, bool falling) {
    const double d = high - low;
    if (d < 0.15) return std::nullopt;  // fax and disturbance look alike: undecidable
    const double hi_thr = high - 0.25 * d;
    const double lo_thr = low + 0.25 * d;
    const auto n = static_cast<std::ptrdiff_t>(band.size());
    const auto from =
        std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(edge_s - kEdgeSearchS));
    const auto to =
        std::min<std::ptrdiff_t>(n - 1, static_cast<std::ptrdiff_t>(edge_s + kEdgeSearchS));
    auto at = [&](std::ptrdiff_t i) {
        return static_cast<double>(band[static_cast<std::size_t>(i)]);
    };
    if (falling) {
        // First second already disturbed, then the last one still clean before it.
        std::ptrdiff_t low_i = -1;
        for (std::ptrdiff_t i = from; i <= to; ++i) {
            if (at(i) <= lo_thr) {
                low_i = i;
                break;
            }
        }
        if (low_i < 0) return std::nullopt;
        for (std::ptrdiff_t j = low_i - 1; j >= from; --j) {
            if (at(j) >= hi_thr) return static_cast<double>(low_i - j);
        }
        return std::nullopt;
    }
    std::ptrdiff_t low_i = -1;
    for (std::ptrdiff_t i = to; i >= from; --i) {
        if (at(i) <= lo_thr) {
            low_i = i;
            break;
        }
    }
    if (low_i < 0) return std::nullopt;
    for (std::ptrdiff_t j = low_i + 1; j <= to; ++j) {
        if (at(j) >= hi_thr) return static_cast<double>(j - low_i);
    }
    return std::nullopt;
}

}  // namespace

const char* cause_name(Cause c) {
    switch (c) {
        case Cause::kFading: return "fading";
        case Cause::kOverload: return "overload";
        case Cause::kImpulsiveInterference: return "impulsive_interference";
        case Cause::kBroadbandInterference: return "broadband_interference";
    }
    return "fading";
}

DisturbanceReport detect_disturbances(const wefax::DecodeReport& report, std::size_t lines,
                                      const Telemetry& t, const DisturbanceRules& rules) {
    DisturbanceReport out;
    const std::size_t seconds = t.audio_dbfs.size();
    out.per_second.assign(seconds, 0);
    const auto& q = report.line_quality;
    if (lines == 0 || q.empty() || report.image_end_s <= report.image_start_s) return out;
    const double lp = (report.image_end_s - report.image_start_s) / static_cast<double>(lines);
    const auto max_gap = static_cast<std::size_t>(rules.max_gap_s / lp);
    const auto min_lines = static_cast<std::size_t>(std::ceil(rules.min_run_s / lp));

    std::vector<Run> runs;
    for (const Run& r : lost_runs(q, rules.bad_line_quality, max_gap)) {
        if (r.last - r.first + 1 >= min_lines) runs.push_back(r);
    }

    // Reference: the image seconds outside every lost run.
    auto line_time = [&](double line) { return report.image_start_s + line * lp; };
    std::vector<bool> in_run(seconds, false);
    for (const Run& r : runs) {
        const double a = line_time(static_cast<double>(r.first));
        const double b = line_time(static_cast<double>(r.last + 1));
        for (std::size_t s = 0; s < seconds; ++s) {
            const double c = static_cast<double>(s) + 0.5;
            if (c >= a && c <= b) in_run[s] = true;
        }
    }
    std::vector<std::size_t> ref, image;
    for (std::size_t s = 0; s < seconds; ++s) {
        const double c = static_cast<double>(s) + 0.5;
        if (c < report.image_start_s || c > report.image_end_s) continue;
        image.push_back(s);
        if (!in_run[s]) ref.push_back(s);
    }
    if (ref.empty()) ref = image;
    const AudioFeatures& f = t.features;
    const double ref_level = median(pick(t.audio_dbfs, ref));
    const double ref_band = median(pick(f.band_fraction, ref));
    const bool have_smeter = !t.smeter_db_s9.empty();
    const double ref_smeter = have_smeter ? median(pick(t.smeter_db_s9, ref)) : 0.0;

    std::size_t local_lines = 0, fading_lines = 0;
    for (const Run& r : runs) {
        Zone z;
        z.first_line = r.first;
        z.last_line = r.last;
        z.start_s = line_time(static_cast<double>(r.first));
        z.end_s = line_time(static_cast<double>(r.last + 1));
        // Inner seconds: the edges mix fax and disturbance.
        std::vector<std::size_t> inner, all;
        for (std::size_t s = 0; s < seconds; ++s) {
            const double c = static_cast<double>(s) + 0.5;
            if (c >= z.start_s && c <= z.end_s) all.push_back(s);
            if (c >= z.start_s + 1.0 && c <= z.end_s - 1.0) inner.push_back(s);
        }
        if (inner.empty()) inner = all;

        auto& ind = z.indicators;
        if (mean(pick(f.clip_fraction, inner)) >= rules.clip_fraction) ind.emplace_back("clipping");
        if (median(pick(t.audio_dbfs, inner)) - ref_level >= rules.level_jump_db) {
            ind.emplace_back("level_jump");
        }
        if (have_smeter) {
            const double sm = median(pick(t.smeter_db_s9, inner));
            if (sm >= rules.smeter_high_db_s9 && sm - ref_smeter >= rules.smeter_rise_db) {
                ind.emplace_back("smeter_high");
            }
        }
        if (median(pick(f.kurtosis, inner)) >= rules.kurtosis) ind.emplace_back("impulsive");
        if (median(pick(f.periodicity, inner)) >= rules.periodicity) ind.emplace_back("periodic");
        const bool broadband = median(pick(f.flatness, inner)) >= rules.flatness;
        if (broadband) ind.emplace_back("broadband");
        const double zone_band = median(pick(f.band_fraction, inner));
        // An edge at the very start or end of the image cannot be judged: only the
        // edges that exist count, and at least one must.
        const bool has_fall = r.first > 0;
        const bool has_rise = r.last + 1 < lines;
        const auto fall = has_fall
                              ? transition_s(f.band_fraction, z.start_s, ref_band, zone_band, true)
                              : std::nullopt;
        const auto rise = has_rise
                              ? transition_s(f.band_fraction, z.end_s, ref_band, zone_band, false)
                              : std::nullopt;
        const bool abrupt = (has_fall || has_rise) &&
                            (!has_fall || (fall && *fall <= rules.edge_s)) &&
                            (!has_rise || (rise && *rise <= rules.edge_s));
        if (abrupt) ind.emplace_back("abrupt_edges");

        auto has = [&](const char* s) { return std::find(ind.begin(), ind.end(), s) != ind.end(); };
        // A periodic envelope is the signature of switching interference, which may
        // clip too; clipping alone (or with the irregular impulses of speech) is overload.
        if (has("periodic")) {
            z.cause = Cause::kImpulsiveInterference;
        } else if (has("clipping") || has("level_jump") || has("smeter_high")) {
            z.cause = Cause::kOverload;
        } else if (has("impulsive")) {
            z.cause = Cause::kImpulsiveInterference;
        } else if (broadband && abrupt) {
            z.cause = Cause::kBroadbandInterference;
        } else {
            z.cause = Cause::kFading;
        }
        const std::size_t n = r.last - r.first + 1;
        (is_local(z.cause) ? local_lines : fading_lines) += n;
        for (std::size_t s : all) out.per_second[s] = static_cast<std::uint8_t>(z.cause);
        out.zones.push_back(std::move(z));
    }
    out.local_fraction = static_cast<double>(local_lines) / static_cast<double>(lines);
    out.fading_fraction = static_cast<double>(fading_lines) / static_cast<double>(lines);
    return out;
}

void mute_local_disturbances(audio::AudioBuffer& audio, const DisturbanceReport& report) {
    const double rate = audio.sample_rate;
    const auto fade = std::max<std::size_t>(1, static_cast<std::size_t>(kFadeS * rate));
    for (const Zone& z : report.zones) {
        if (!is_local(z.cause)) continue;
        const auto a = std::min(audio.samples.size(), static_cast<std::size_t>(z.start_s * rate));
        const auto b = std::min(audio.samples.size(), static_cast<std::size_t>(z.end_s * rate));
        for (std::size_t i = a; i < b; ++i) {
            const std::size_t from_a = i - a, to_b = b - 1 - i;
            const std::size_t k = std::min(from_a, to_b);
            const float g =
                k < fade ? 1.0F - static_cast<float>(k) / static_cast<float>(fade) : 0.0F;
            audio.samples[i] *= g;
        }
    }
}

}  // namespace owfax::vitrine
