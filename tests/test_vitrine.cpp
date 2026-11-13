// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Web showcase chart packages (docs/VITRINE-WEB.md, lot 1).

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "owfax/net/json.hpp"
#include "owfax/vitrine/catalog.hpp"
#include "owfax/vitrine/geo.hpp"
#include "owfax/vitrine/package.hpp"
#include "owfax/vitrine/telemetry.hpp"
#include "owfax/wefax/decoder.hpp"
#include "owfax/wefax/modulator.hpp"
#include "owfax_test.hpp"

using namespace owfax;
using namespace owfax::vitrine;
using net::Json;

namespace {

double median_of(std::vector<float> v, std::size_t from, std::size_t to) {
    std::vector<float> s(v.begin() + static_cast<std::ptrdiff_t>(from),
                         v.begin() + static_cast<std::ptrdiff_t>(std::min(to, v.size())));
    std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() / 2), s.end());
    return s[s.size() / 2];
}

/// Test chart with `lead_s` seconds of receiver noise before it, as in a capture.
audio::AudioBuffer capture_like(std::size_t lines, double snr_db, double lead_s) {
    wefax::ModulatorOptions m;
    m.snr_db = snr_db;
    auto chart = wefax::modulate(wefax::make_test_chart(wefax::Ioc::k576, lines), m);
    audio::AudioBuffer rec;
    rec.sample_rate = chart.sample_rate;
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0F, 0.01F);
    rec.samples.resize(static_cast<std::size_t>(lead_s * rec.sample_rate));
    for (float& s : rec.samples) s = noise(rng);
    rec.samples.insert(rec.samples.end(), chart.samples.begin(), chart.samples.end());
    return rec;
}

StationRef test_station(bool allowed) {
    StationRef s;
    s.id = "dwd-pinneberg";
    s.name = "Deutscher Wetterdienst";
    s.callsign = "DDK6";
    s.assigned_khz = 13882.5;
    s.power_kw = 10.0;
    s.position = {53.67, 9.81};
    s.publish_allowed = allowed;
    s.attribution = "Quelle: Deutscher Wetterdienst";
    return s;
}

}  // namespace

OWFAX_TEST(vitrine_locator_round_trip) {
    const auto jn18 = from_locator("JN18");
    OWFAX_CHECK(jn18.has_value());
    OWFAX_CHECK_NEAR(jn18->lat_deg, 48.5, 1e-9);
    OWFAX_CHECK_NEAR(jn18->lon_deg, 3.0, 1e-9);
    OWFAX_CHECK(to_locator(*jn18, 4) == "JN18");
    for (const char* loc : {"IN88gs", "JO43ab", "AA00aa", "RR99xx", "JN18eu52"}) {
        const auto p = from_locator(loc);
        OWFAX_CHECK(p.has_value());
        if (p) OWFAX_CHECK(to_locator(*p, static_cast<int>(std::string(loc).size())) == loc);
    }
    OWFAX_CHECK(from_locator("in88GS").has_value());  // case-insensitive
    for (const char* bad : {"", "JN1", "ZZ00", "JN18zz", "JN18ab5", "JNAB"}) {
        OWFAX_CHECK(!from_locator(bad).has_value());
    }
}

OWFAX_TEST(vitrine_great_circle) {
    // Paris → London: 344 km, initial bearing 330°.
    const auto g = great_circle({48.8566, 2.3522}, {51.5074, -0.1278});
    OWFAX_CHECK_NEAR(g.distance_km, 344.0, 3.0);
    OWFAX_CHECK_NEAR(g.distance_nm, g.distance_km / 1.852, 1e-9);
    OWFAX_CHECK_NEAR(g.azimuth_deg, 330.0, 3.0);
    // Due east along the equator: a quarter of the circumference, bearing 90°.
    const auto e = great_circle({0.0, 0.0}, {0.0, 90.0});
    OWFAX_CHECK_NEAR(e.distance_km, 10007.5, 1.0);
    OWFAX_CHECK_NEAR(e.azimuth_deg, 90.0, 1e-6);
}

OWFAX_TEST(vitrine_sun_and_daylight) {
    // Near the March 2026 equinox, solar noon on the equator: sun almost overhead.
    OWFAX_CHECK(sun_elevation_deg({0.0, 0.0}, 1774008000) > 86.0);
    OWFAX_CHECK(sun_elevation_deg({0.0, 0.0}, 1773964800) < -80.0);  // midnight
    // Paris, 21 June 2026 12:00 UTC: 90 − 48.86 + 23.44 ≈ 64.5°.
    OWFAX_CHECK_NEAR(sun_elevation_deg({48.8566, 2.3522}, 1782043200), 64.5, 1.5);
    OWFAX_CHECK(path_daylight({48.0, -4.0}, {53.67, 9.81}, 1782043200) == "day");
    OWFAX_CHECK(path_daylight({48.0, -4.0}, {53.67, 9.81}, 1782043200 - 12 * 3600) == "night");
}

OWFAX_TEST(vitrine_smeter_csv_crosses_midnight) {
    const auto path = std::filesystem::temp_directory_path() / "owfax_vitrine_smeter.csv";
    {
        std::ofstream out(path);
        out << "utc,strength_db\n23:59:50,-3\n23:59:60,bad\n00:00:00,-8\nnonsense\n00:00:10,12\n";
    }
    // Capture started 2026-11-10 23:59:45Z.
    const std::int64_t start = 1794320645 - 14 * 3600 - 24 * 60 - 5 + 23 * 3600 + 59 * 60 + 45;
    const auto r = read_smeter_csv(path.string(), start);
    std::filesystem::remove(path);
    OWFAX_CHECK(r.size() == 3);
    if (r.size() == 3) {
        OWFAX_CHECK(r[0].unix_time == start + 5);
        OWFAX_CHECK(r[1].unix_time == start + 15);  // next day
        OWFAX_CHECK(r[2].unix_time == start + 25);
        OWFAX_CHECK_NEAR(r[2].db_s9, 12.0, 1e-9);
    }
}

OWFAX_TEST(vitrine_telemetry_snr_follows_the_signal) {
    // SNR measured in the same 2.5 kHz bandwidth as the test modulator.
    double prev = -100.0;
    for (double snr : {0.0, 10.0, 20.0, 30.0}) {
        wefax::ModulatorOptions m;
        m.snr_db = snr;
        const auto a = wefax::modulate(wefax::make_test_chart(wefax::Ioc::k576, 120), m);
        const auto t = measure(a, SnrBands{});
        OWFAX_CHECK(t.audio_dbfs.size() == t.snr_db.size());
        OWFAX_CHECK(t.audio_dbfs.size() == static_cast<std::size_t>(a.duration_s() + 0.999));
        OWFAX_CHECK(t.smeter_db_s9.empty());
        // Image lines start after the 5 s start tone and the 30 s of phasing.
        const double got = median_of(t.snr_db, 40, 90);
        std::printf("    modulated SNR %.0f dB -> measured %.1f dB\n", snr, got);
        if (snr <= 20.0) {
            OWFAX_CHECK_NEAR(got, snr, 2.0);
            OWFAX_CHECK(got > prev + 8.0);
        } else {
            // Above ~25 dB the FM sidebands reach the noise reference bands and the
            // estimate levels off: still clearly above the 20 dB case.
            OWFAX_CHECK(got > 23.0 && got > prev + 4.0);
        }
        prev = got;
    }
}

OWFAX_TEST(vitrine_telemetry_smeter_interpolation) {
    audio::AudioBuffer a;
    a.sample_rate = 8000;
    a.samples.assign(8000 * 30, 0.0F);
    const std::vector<SmeterReading> s = {{1000, -10.0}, {1010, 10.0}, {1020, 10.0}};
    const auto t = measure(a, SnrBands{}, s, 1000.0);
    OWFAX_CHECK(t.smeter_db_s9.size() == 30);
    OWFAX_CHECK_NEAR(t.smeter_db_s9[0], -9.0, 1e-4);   // 1000.5 s
    OWFAX_CHECK_NEAR(t.smeter_db_s9[4], -1.0, 1e-4);   // 1004.5 s
    OWFAX_CHECK_NEAR(t.smeter_db_s9[25], 10.0, 1e-4);  // held after the last reading
    OWFAX_CHECK(t.audio_dbfs[0] < -150.0);             // digital silence
}

OWFAX_TEST(vitrine_station_from_catalogue_json) {
    const auto doc = net::parse_json(R"({
      "id": "dwd-pinneberg", "name": "DWD", "position": {"lat": 53.67, "lon": 9.81},
      "wefax": {"shift_hz": 850, "frequencies": [
        {"callsign": "DDH3", "assigned_khz": 3855.0, "power_kw": 10.0},
        {"callsign": "DDK6", "assigned_khz": 13882.5}]},
      "publication": {"allowed": true, "attribution": "Quelle: DWD", "terms_url": "https://x"}})");
    OWFAX_CHECK(doc.has_value());
    const auto s = station_from_json(*doc, 13882.0);
    OWFAX_CHECK(s.has_value());
    if (s) {
        OWFAX_CHECK(s->callsign == "DDK6");
        OWFAX_CHECK(!s->power_kw.has_value());
        OWFAX_CHECK_NEAR(s->shift_hz, 850.0, 1e-9);
        OWFAX_CHECK(s->publish_allowed);
        OWFAX_CHECK(s->attribution == "Quelle: DWD");
    }
    OWFAX_CHECK(!station_from_json(*doc, 7880.0).has_value());
    // Without a publication block (or without attribution) a station is never published.
    const auto bare = net::parse_json(R"({"id": "x", "position": {"lat": 1, "lon": 2},
      "wefax": {"frequencies": [{"callsign": "X", "assigned_khz": 4610}]},
      "publication": {"allowed": true}})");
    const auto b = station_from_json(*bare, 4610.0);
    OWFAX_CHECK(b.has_value() && !b->publish_allowed);
}

OWFAX_TEST(vitrine_selection_rules) {
    wefax::DecodeReport r;
    r.apt_start_found = true;
    r.apt_stop_found = true;
    r.line_quality.assign(800, 0.9F);
    SelectionRules rules;
    OWFAX_CHECK(select(r, 800, 0.0, test_station(true), rules).publish);
    const auto blocked = select(r, 800, 0.0, test_station(false), rules);
    OWFAX_CHECK(!blocked.publish &&
                blocked.reasons == std::vector<std::string>{"station_not_allowed"});
    OWFAX_CHECK(select(r, 800, 0.2, test_station(true), rules).reasons ==
                std::vector<std::string>{"local_disturbance"});
    r.apt_stop_found = false;  // no stop tone: complete only if long enough
    OWFAX_CHECK(select(r, 800, 0.0, test_station(true), rules).publish);
    OWFAX_CHECK(select(r, 200, 0.0, test_station(true), rules).reasons ==
                std::vector<std::string>{"incomplete"});
    r.line_quality.assign(800, 0.3F);
    OWFAX_CHECK(select(r, 800, 0.0, test_station(true), rules).reasons ==
                std::vector<std::string>{"low_quality"});
    OWFAX_CHECK(select(r, 0, 0.0, test_station(true), rules).reasons.front() == "no_image");
}

OWFAX_TEST(vitrine_package_from_a_capture) {
    const double lead = 20.0;
    const auto rec = capture_like(240, 25.0, lead);
    const auto charts = wefax::decode_all(rec);
    OWFAX_CHECK(charts.size() == 1);
    if (charts.size() != 1) return;

    ChartSource src;
    src.recording = &rec;
    src.recording_start_unix = 1794320645;  // 2026-11-10 14:24:05Z
    src.raw = &charts[0];
    src.max_s = rec.duration_s();
    ReceiverRef rx;
    rx.position = *from_locator("IN88gs");
    rx.locator = "IN88gs";
    wefax::DecodeOptions enh;
    enh.suppress_clicks = true;
    SelectionRules rules;
    rules.min_lines = 200;
    const auto p = build_package(src, test_station(true), rx, "DDK6", {}, rules, enh);

    // The chart starts with its APT tone, 20 s into the capture: 14:24:25Z.
    OWFAX_CHECK(p.chart_id == "20261110T1424Z_DDK6_13882.5kHz");
    OWFAX_CHECK_NEAR(p.audio_start_s, lead - 2.0, 0.3);
    OWFAX_CHECK_NEAR(p.report.apt_start_s, 2.0, 0.3);
    OWFAX_CHECK(p.raw.height >= 235 && p.raw.height <= 245);
    OWFAX_CHECK(p.enhanced.has_value());
    OWFAX_CHECK(p.selection.publish);
    OWFAX_CHECK(p.telemetry.snr_db.size() ==
                static_cast<std::size_t>(p.audio.duration_s() + 0.999));
    OWFAX_CHECK(p.path.distance_km > 900.0 && p.path.distance_km < 1300.0);

    const auto meta = net::parse_json(meta_json(p, test_station(true), rx, PackageFiles{}));
    OWFAX_CHECK(meta.has_value());
    if (meta) {
        OWFAX_CHECK(meta->get("schema")->string == "owfax-vitrine/0.1");
        OWFAX_CHECK(meta->get("replay")->boolean);
        OWFAX_CHECK(meta->get("receiver")->get("locator")->string == "IN88gs");
        OWFAX_CHECK(meta->get("wefax")->get("lpm")->number == 120.0);
        OWFAX_CHECK(meta->get("time")->get("start_utc")->string == "2026-11-10T14:24:25Z");
        const Json* sync = meta->get("sync");
        OWFAX_CHECK(sync->get("image_s")->number > sync->get("apt_start_s")->number + 30.0);
        OWFAX_CHECK_NEAR(sync->get("line_period_s")->number, 0.5, 0.001);
        OWFAX_CHECK(meta->get("selection")->get("publish")->boolean);
        OWFAX_CHECK(meta->get("files")->get("enhanced")->string == "enh.webp");
        OWFAX_CHECK(meta->get("quality")->get("snr_db_median")->number > 20.0);
    }
    const auto tel = net::parse_json(telemetry_json(p));
    OWFAX_CHECK(tel.has_value());
    if (tel) {
        OWFAX_CHECK(tel->get("line_quality")->array.size() == p.raw.height);
        OWFAX_CHECK(tel->get("snr_db")->array.size() == p.telemetry.snr_db.size());
        OWFAX_CHECK(tel->get("smeter_db_s9") == nullptr);
    }
}
