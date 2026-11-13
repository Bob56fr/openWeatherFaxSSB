// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/vitrine/package.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

#include "owfax/build_info.hpp"
#include "owfax/engine/receiver.hpp"

namespace owfax::vitrine {
namespace {

constexpr double kAptStartToneS = 5.0;   // APT start tone, then phasing (ITU-R / WMO)
constexpr double kPhasingGuessS = 35.0;  // audio kept before the image without APT
constexpr double kLeadS = 2.0;           // audio before the APT start tone
constexpr double kTailS = 12.0;          // stop tone (5 s) + trailer, after the image

std::string num(double v, int decimals) {
    if (!std::isfinite(v)) return "null";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string str(const std::string& s) {
    return "\"" + engine::json_escape(s) + "\"";
}

std::string iso_utc(std::int64_t t) {
    std::string s = engine::utc_text(t);  // "2026-11-10 07:04:12Z"
    if (s.size() > 10) s[10] = 'T';
    return s;
}

/// "13882.5", "3855": the frequency as people write it.
std::string khz_text(double khz) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", khz);
    std::string s = buf;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s;
}

template <typename T>
std::string array(const std::vector<T>& v, int decimals) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ',';
        out += num(static_cast<double>(v[i]), decimals);
    }
    return out + "]";
}

double median(std::vector<float> v) {
    if (v.empty()) return std::nan("");
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return static_cast<double>(*mid);
}

}  // namespace

double mean_quality(const wefax::DecodeReport& r) {
    if (r.line_quality.empty()) return 0.0;
    double q = 0.0;
    for (float v : r.line_quality) q += static_cast<double>(v);
    return q / static_cast<double>(r.line_quality.size());
}

Selection select(const wefax::DecodeReport& r, std::size_t lines, double saturated_fraction,
                 const StationRef& station, const SelectionRules& rules) {
    Selection s;
    if (lines == 0) {
        s.reasons.emplace_back("no_image");
    } else if (!(r.apt_start_found || r.phasing_found) ||
               (!r.apt_stop_found && lines < rules.min_lines)) {
        s.reasons.emplace_back("incomplete");  // PUB-020
    }
    if (mean_quality(r) < rules.min_quality) s.reasons.emplace_back("low_quality");  // PUB-021
    if (saturated_fraction > rules.max_saturated_fraction) {
        s.reasons.emplace_back("saturated");  // PUB-022, SAT-010
    }
    if (!station.publish_allowed) s.reasons.emplace_back("station_not_allowed");  // PUB-024
    s.publish = s.reasons.empty();
    return s;
}

Package build_package(const ChartSource& src, const StationRef& station,
                      const ReceiverRef& receiver, const std::string& label,
                      const std::vector<SmeterReading>& smeter, const SelectionRules& rules,
                      const std::optional<wefax::DecodeOptions>& enhanced_options) {
    if (!src.recording || !src.raw || src.recording->sample_rate <= 0) {
        throw std::invalid_argument("build_package: missing recording or decode");
    }
    const audio::AudioBuffer& rec = *src.recording;
    const wefax::DecodeReport& r = src.raw->report;
    const double rate = rec.sample_rate;
    const double rec_end = rec.duration_s();
    const double lo = std::max(0.0, src.min_s);
    const double hi = std::min(rec_end, src.max_s > 0.0 ? src.max_s : rec_end);

    const bool apt = r.apt_start_found && r.apt_start_s >= 0.0;
    const double chart_start = apt ? r.apt_start_s : std::max(lo, r.image_start_s - kPhasingGuessS);
    const double a0 = std::clamp(chart_start - kLeadS, lo, hi);
    const double a1 = std::clamp(r.image_end_s + kTailS, a0, hi);
    if (a1 - a0 < 1.0) throw std::invalid_argument("build_package: empty chart");

    Package p;
    p.audio_start_s = a0;
    p.audio.sample_rate = rec.sample_rate;
    const auto s0 = static_cast<std::size_t>(a0 * rate);
    const auto s1 = std::min(rec.samples.size(), static_cast<std::size_t>(a1 * rate));
    p.audio.samples.assign(rec.samples.begin() + static_cast<std::ptrdiff_t>(s0),
                           rec.samples.begin() + static_cast<std::ptrdiff_t>(s1));
    // Times relative to the cut, exact to the sample (a0 is rounded down to one).
    const double shift = static_cast<double>(s0) / rate;
    p.report = r;
    if (apt) p.report.apt_start_s -= shift;
    p.report.image_start_s -= shift;
    p.report.image_end_s -= shift;
    p.raw = src.raw->image;

    if (enhanced_options) {
        try {
            auto e = wefax::decode(p.audio, *enhanced_options);
            if (e.image.width == p.raw.width && e.image.height > 0) p.enhanced = std::move(e.image);
        } catch (const std::exception&) {
            // No enhanced image: the raw one is the reference anyway (DSP-030).
        }
    }

    SnrBands bands;
    bands.signal_lo_hz = std::min(r.black_hz, r.white_hz) - 100.0;
    bands.signal_hi_hz = std::max(r.black_hz, r.white_hz) + 100.0;
    p.telemetry =
        measure(p.audio, bands, smeter, static_cast<double>(src.recording_start_unix) + shift);

    p.chart_start_s = chart_start - shift;
    p.chart_start_unix =
        src.recording_start_unix + static_cast<std::int64_t>(std::lround(chart_start));
    p.chart_id = engine::utc_stamp(p.chart_start_unix) + "_" + label + "_" +
                 khz_text(station.assigned_khz) + "kHz";
    p.path = great_circle(receiver.position, station.position);
    const auto mid = src.recording_start_unix +
                     static_cast<std::int64_t>((r.image_start_s + r.image_end_s) / 2.0);
    p.daylight = path_daylight(receiver.position, station.position, mid);
    p.selection = select(p.report, p.raw.height, 0.0, station, rules);
    return p;
}

std::string meta_json(const Package& p, const StationRef& st, const ReceiverRef& rx,
                      const PackageFiles& files) {
    const auto& r = p.report;
    const bool apt = r.apt_start_found && r.apt_start_s >= 0.0;
    const double chart_start = p.chart_start_s;
    const std::int64_t start_unix = p.chart_start_unix;
    const std::int64_t end_unix =
        p.chart_start_unix + static_cast<std::int64_t>(std::lround(r.image_end_s - chart_start));
    const std::size_t lines = p.raw.height;
    const double line_period =
        lines > 0 ? (r.image_end_s - r.image_start_s) / static_cast<double>(lines) : 0.0;
    const int lpm = line_period > 0.0 ? static_cast<int>(std::lround(60.0 / line_period)) : 0;

    // SNR over the image only (the tones and the gaps would bias it).
    std::vector<float> snr_img;
    for (std::size_t s = 0; s < p.telemetry.snr_db.size(); ++s) {
        const double t = static_cast<double>(s) + 0.5;
        if (t >= r.image_start_s && t <= r.image_end_s) snr_img.push_back(p.telemetry.snr_db[s]);
    }
    std::size_t bad = 0;
    for (float q : r.line_quality) bad += q < 0.5F ? 1 : 0;
    const double bad_fraction =
        r.line_quality.empty()
            ? 1.0
            : static_cast<double>(bad) / static_cast<double>(r.line_quality.size());

    const BuildInfo& b = build_info();
    std::ostringstream o;
    o << "{\n"
      << "  \"schema\": \"owfax-vitrine/0.1\",\n"
      << "  \"chart_id\": " << str(p.chart_id) << ",\n"
      << "  \"replay\": true,\n"
      << "  \"software\": {\"name\": \"openWeatherFaxSSB\", \"version\": "
      << str(std::string(b.version)) << ", \"commit\": " << str(std::string(b.git_revision))
      << ", \"source_url\": " << str(std::string(b.source_url)) << "},\n"
      << "  \"station\": {\"id\": " << str(st.id) << ", \"name\": " << str(st.name)
      << ", \"callsign\": " << str(st.callsign) << ", \"assigned_khz\": " << num(st.assigned_khz, 1)
      << ", \"power_kw\": " << (st.power_kw ? num(*st.power_kw, 1) : "null")
      << ", \"attribution\": " << str(st.attribution) << ", \"terms_url\": " << str(st.terms_url)
      << "},\n"
      << "  \"receiver\": {\"locator\": " << str(rx.locator) << ", \"rig\": " << str(rx.rig)
      << ", \"input\": " << str(rx.input)
      << ", \"dial_khz\": " << (rx.dial_khz ? num(*rx.dial_khz, 2) : "null")
      << ", \"mode\": " << str(rx.mode) << "},\n"
      << "  \"path\": {\"distance_km\": " << num(p.path.distance_km, 0)
      << ", \"distance_nm\": " << num(p.path.distance_nm, 0)
      << ", \"azimuth_deg\": " << num(p.path.azimuth_deg, 0)
      << ", \"daylight\": " << str(p.daylight) << "},\n"
      << "  \"time\": {\"start_utc\": " << str(iso_utc(start_unix))
      << ", \"end_utc\": " << str(iso_utc(end_unix))
      << ", \"duration_s\": " << num(r.image_end_s - chart_start, 1) << "},\n"
      << "  \"wefax\": {\"lpm\": " << lpm << ", \"ioc\": " << static_cast<int>(r.ioc)
      << ", \"lines\": " << lines << ", \"width_px\": " << p.raw.width
      << ", \"black_hz\": " << num(r.black_hz, 0) << ", \"white_hz\": " << num(r.white_hz, 0)
      << ", \"clock_ppm\": " << num(r.clock_ppm, 2)
      << ", \"tuning_offset_hz\": " << num(r.tuning_offset_hz, 1)
      << ", \"apt_start\": " << (r.apt_start_found ? "true" : "false")
      << ", \"phasing\": " << (r.phasing_found ? "true" : "false")
      << ", \"apt_stop\": " << (r.apt_stop_found ? "true" : "false") << "},\n"
      << "  \"quality\": {\"mean\": " << num(mean_quality(r), 3)
      << ", \"snr_db_median\": " << num(median(snr_img), 1)
      << ", \"lines_bad_fraction\": " << num(bad_fraction, 3) << "},\n"
      << "  \"diversity\": {\"enabled\": false, \"sources\": []},\n"
      << "  \"saturation\": {\"detector\": \"none\", \"zones\": [], \"fraction\": 0},\n"
      << "  \"sync\": {\"audio_duration_s\": " << num(p.audio.duration_s(), 3)
      << ", \"apt_start_s\": " << (apt ? num(r.apt_start_s, 3) : "null")
      << ", \"phasing_s\": " << (apt ? num(r.apt_start_s + kAptStartToneS, 3) : "null")
      << ", \"image_s\": " << num(r.image_start_s, 3)
      << ", \"image_end_s\": " << num(r.image_end_s, 3)
      << ", \"line_period_s\": " << num(line_period, 6) << "},\n"
      << "  \"treatments\": {\"raw\": [], \"enhanced\": " << (p.enhanced ? "[\"clicks\"]" : "[]")
      << "},\n"
      << "  \"selection\": {\"publish\": " << (p.selection.publish ? "true" : "false")
      << ", \"reasons\": [";
    for (std::size_t i = 0; i < p.selection.reasons.size(); ++i) {
        o << (i ? ", " : "") << str(p.selection.reasons[i]);
    }
    o << "]},\n"
      << "  \"files\": {\"raw\": " << str(files.raw) << ", \"enhanced\": "
      << (p.enhanced && !files.enhanced.empty() ? str(files.enhanced) : "null")
      << ", \"thumb\": " << str(files.thumb) << ", \"audio\": " << str(files.audio)
      << ", \"telemetry\": " << str(files.telemetry) << "}\n"
      << "}\n";
    return o.str();
}

std::string telemetry_json(const Package& p) {
    std::string o = "{\n  \"schema\": \"owfax-vitrine-telemetry/0.1\",\n  \"interval_s\": 1,\n";
    o += "  \"audio_dbfs\": " + array(p.telemetry.audio_dbfs, 1) + ",\n";
    o += "  \"snr_db\": " + array(p.telemetry.snr_db, 1) + ",\n";
    if (!p.telemetry.smeter_db_s9.empty()) {
        o += "  \"smeter_db_s9\": " + array(p.telemetry.smeter_db_s9, 1) + ",\n";
    }
    o += "  \"line_quality\": " + array(p.report.line_quality, 2) + "\n}\n";
    return o;
}

}  // namespace owfax::vitrine
