// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Audio sources, input URIs and the reception engine (files written per chart).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "owfax/audio/source.hpp"
#include "owfax/engine/receiver.hpp"
#include "owfax/io/audio_input.hpp"
#include "owfax/wefax/modulator.hpp"
#include "owfax_test.hpp"

namespace fs = std::filesystem;
using namespace owfax;

namespace {

fs::path fresh_dir(const char* name) {
    const fs::path d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

OWFAX_TEST(utc_stamps) {
    OWFAX_CHECK(engine::utc_stamp(0) == "19700101T0000Z");
    OWFAX_CHECK(engine::utc_stamp(1794812640) == "20261116T0704Z");
    OWFAX_CHECK(engine::utc_text(1794812652) == "2026-11-16 07:04:12Z");
    OWFAX_CHECK(engine::utc_stamp(951782400) == "20000229T0000Z");  // leap day
    OWFAX_CHECK(engine::json_escape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
}

OWFAX_TEST(buffer_source_delivers_everything_then_ends) {
    audio::AudioBuffer a;
    a.sample_rate = 8000;
    for (int i = 0; i < 10000; ++i) a.samples.push_back(static_cast<float>(i % 100) / 100.0F);
    auto src = audio::make_buffer_source(a, "test");
    std::vector<float> got;
    float buf[777];
    std::size_t n = 0;
    while ((n = src->read(buf, 777)) > 0) got.insert(got.end(), buf, buf + n);
    OWFAX_CHECK(got == a.samples);
    OWFAX_CHECK(src->read(buf, 777) == 0);

    auto again = audio::make_buffer_source(a, "test");
    again->interrupt();
    OWFAX_CHECK(again->read(buf, 777) == 0);
}

OWFAX_TEST(pcm16_source_decodes_little_endian) {
    const fs::path p = fresh_dir("owfax_pcm") / "in.raw";
    {
        std::ofstream out(p, std::ios::binary);
        const unsigned char bytes[] = {0x00, 0x40, 0x00, 0xC0, 0xFF, 0x7F, 0x00, 0x80, 0x01};
        out.write(reinterpret_cast<const char*>(bytes), sizeof bytes);  // odd byte: dropped
    }
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    OWFAX_CHECK(f != nullptr);
    if (f == nullptr) return;
    auto src = audio::make_pcm16_source(f, 8000, "raw");
    float buf[16];
    const std::size_t n = src->read(buf, 16);
    OWFAX_CHECK(n == 4);
    OWFAX_CHECK_NEAR(buf[0], 0.5, 1e-6);
    OWFAX_CHECK_NEAR(buf[1], -0.5, 1e-6);
    OWFAX_CHECK_NEAR(buf[2], 32767.0 / 32768.0, 1e-6);
    OWFAX_CHECK_NEAR(buf[3], -1.0, 1e-6);
    OWFAX_CHECK(src->read(buf, 16) == 0);
    std::fclose(f);
}

OWFAX_TEST(open_input_uris) {
    const fs::path p = fresh_dir("owfax_uri") / "x.wav";
    audio::AudioBuffer a;
    a.sample_rate = 11025;
    a.samples.assign(1000, 0.25F);
    audio::write_wav(p.string(), a);
    auto src = io::open_input("wav:" + p.string());
    OWFAX_CHECK(src->sample_rate() == 11025);
    auto bare = io::open_input(p.string());
    OWFAX_CHECK(bare->sample_rate() == 11025);
    bool threw = false;
    try {
        io::open_input("nope:thing");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    OWFAX_CHECK(threw);
    const auto b = io::input_backends();
    OWFAX_CHECK(b.size() >= 2 && b[0] == "wav" && b[1] == "stdin");
}

OWFAX_TEST(receiver_writes_live_raw_enhanced_and_metadata) {
    const fs::path dir = fresh_dir("owfax_rx");
    wefax::ModulatorOptions mod;
    mod.sample_rate = 8000;
    mod.clock_ppm = 40.0;
    auto audio = wefax::modulate(wefax::make_test_chart(mod.ioc, 120), mod);
    mod.ioc = wefax::Ioc::k288;
    const auto second = wefax::modulate(wefax::make_test_chart(mod.ioc, 80), mod);
    audio.samples.insert(audio.samples.end(), second.samples.begin(), second.samples.end());

    engine::ReceiverConfig cfg;
    cfg.output_dir = dir.string();
    cfg.label = "TEST";
    cfg.live_flush_lines = 30;
    const std::int64_t t0 = 1794812400;  // 2026-11-16 07:00:00Z
    engine::Receiver rx(mod.sample_rate, cfg, t0);
    std::vector<std::string> log;
    rx.log = [&](const std::string& m) { log.push_back(m); };

    std::vector<engine::ChartInfo> charts;
    bool saw_live_while_receiving = false;
    for (std::size_t pos = 0; pos < audio.samples.size(); pos += 800) {
        const std::size_t n = std::min<std::size_t>(800, audio.samples.size() - pos);
        rx.push(&audio.samples[pos], n);
        if (rx.receiving() && !rx.current_stem().empty() &&
            fs::exists(dir / (rx.current_stem() + "_live.png"))) {
            saw_live_while_receiving = true;
        }
        for (auto& c : rx.take_completed()) charts.push_back(c);
    }
    rx.finish();
    for (auto& c : rx.take_completed()) charts.push_back(c);

    OWFAX_CHECK(saw_live_while_receiving);
    OWFAX_CHECK(charts.size() == 2);
    if (charts.size() != 2) return;
    const auto& c = charts[0];
    std::printf("    %s: %zu live / %zu final lines, %+.1f ppm, quality %.2f, %zu files\n",
                c.stem.c_str(), c.live_lines, c.lines, c.clock_ppm, c.quality, c.files.size());
    OWFAX_CHECK(c.stem.rfind("20261116T07", 0) == 0);
    OWFAX_CHECK(c.stem.size() > 5 && c.stem.substr(c.stem.size() - 5) == "_TEST");
    OWFAX_CHECK(c.ioc == wefax::Ioc::k576 && charts[1].ioc == wefax::Ioc::k288);
    OWFAX_CHECK(c.apt_start && c.phasing_found);
    OWFAX_CHECK(c.lines >= 115 && c.lines <= 125);
    OWFAX_CHECK(c.live_lines >= 115 && c.live_lines <= 125);
    OWFAX_CHECK_NEAR(c.clock_ppm, 40.0, 5.0);
    OWFAX_CHECK(c.quality > 0.9);
    for (const char* suffix : {"_live.png", ".png", "_enh.png", ".json"}) {
        OWFAX_CHECK(fs::exists(dir / (c.stem + suffix)));
        OWFAX_CHECK(!fs::exists(dir / (c.stem + suffix + ".tmp")));
    }
    OWFAX_CHECK(c.files.size() == 4);
    const std::string json = slurp(dir / (c.stem + ".json"));
    OWFAX_CHECK(json.find("\"label\": \"TEST\"") != std::string::npos);
    OWFAX_CHECK(json.find("\"ioc\": 576") != std::string::npos);
    OWFAX_CHECK(json.find("\"start\": \"apt\"") != std::string::npos);
    OWFAX_CHECK(json.find("\"enhancements\": [\"clicks\"]") != std::string::npos);
    OWFAX_CHECK(log.size() >= 6);  // start, lock, complete per chart
}

OWFAX_TEST(receiver_finish_finalises_a_chart_in_progress) {
    const fs::path dir = fresh_dir("owfax_rx_cut");
    wefax::ModulatorOptions mod;
    mod.sample_rate = 8000;
    auto audio = wefax::modulate(wefax::make_test_chart(mod.ioc, 200), mod);
    audio.samples.resize(audio.samples.size() / 2);  // signal lost mid-chart
    engine::ReceiverConfig cfg;
    cfg.output_dir = dir.string();
    cfg.enhanced = false;
    engine::Receiver rx(mod.sample_rate, cfg, 0);
    rx.push(audio.samples.data(), audio.samples.size());
    OWFAX_CHECK(rx.receiving());
    rx.finish();
    OWFAX_CHECK(!rx.receiving());
    const auto charts = rx.take_completed();
    OWFAX_CHECK(charts.size() == 1);
    if (charts.size() == 1) {
        OWFAX_CHECK(charts[0].lines > 50);
        OWFAX_CHECK(fs::exists(dir / (charts[0].stem + ".png")));
        OWFAX_CHECK(!fs::exists(dir / (charts[0].stem + "_enh.png")));
    }
}
