// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/engine/receiver.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <utility>

#include "owfax/image/gray_image.hpp"
#include "owfax/wefax/stream_decoder.hpp"

namespace owfax::engine {
namespace fs = std::filesystem;
using wefax::StreamEvent;

namespace {

/// Civil date from days since 1970-01-01 (H. Hinnant's algorithm): portable, no
/// gmtime_r / gmtime_s differences between platforms.
void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0));
}

/// Writes through a temporary file and a rename, so that a reader (web UI) never
/// sees a half-written image.
void write_png_atomic(const fs::path& path, const GrayImage& image) {
    fs::path tmp = path;
    tmp += ".tmp";
    write_png(tmp.string(), image);
    fs::rename(tmp, path);
}

double mean_quality(const wefax::DecodeReport& r) {
    if (r.line_quality.empty()) return 0.0;
    double q = 0.0;
    for (float v : r.line_quality) q += static_cast<double>(v);
    return q / static_cast<double>(r.line_quality.size());
}

std::string fmt(const char* format, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

}  // namespace

namespace {
struct Utc {
    int y;
    unsigned mo, d, h, mi, s;
};
Utc split_utc(std::int64_t t) {
    const std::int64_t days = (t >= 0 ? t : t - 86399) / 86400;
    const auto secs = static_cast<unsigned>(t - days * 86400);
    Utc u{};
    civil_from_days(days, u.y, u.mo, u.d);
    u.h = secs / 3600;
    u.mi = secs / 60 % 60;
    u.s = secs % 60;
    return u;
}
}  // namespace

std::string utc_stamp(std::int64_t t) {
    const Utc u = split_utc(t);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02u%02uT%02u%02uZ", u.y, u.mo, u.d, u.h, u.mi);
    return buf;
}

std::string utc_text(std::int64_t t) {
    const Utc u = split_utc(t);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u %02u:%02u:%02uZ", u.y, u.mo, u.d, u.h, u.mi,
                  u.s);
    return buf;
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

struct Receiver::Impl {
    Receiver& owner;
    ReceiverConfig cfg;
    double rate;
    std::int64_t t0;
    wefax::StreamDecoder decoder;
    fs::path dir;

    bool active = false;
    ChartInfo current;
    GrayImage live;
    std::size_t flushed_lines = 0;
    std::vector<ChartInfo> done;

    static std::optional<wefax::DecodeOptions> enhanced_options(const ReceiverConfig& c) {
        if (!c.enhanced) return std::nullopt;
        wefax::DecodeOptions o = c.options;
        o.suppress_clicks = true;  // the validated enhancements (CLI --enhance)
        return o;
    }

    Impl(Receiver& r, int sample_rate, ReceiverConfig c, std::int64_t start)
        : owner(r),
          cfg(std::move(c)),
          rate(sample_rate),
          t0(start),
          decoder(sample_rate, cfg.options, enhanced_options(cfg)),
          dir(cfg.output_dir) {
        fs::create_directories(dir);
    }

    void say(const std::string& msg) const {
        if (owner.log) owner.log(msg);
    }

    std::string file(const std::string& suffix) const { return current.stem + suffix; }

    void flush_live() {
        if (live.height == 0) return;
        write_png_atomic(dir / file("_live.png"), live);
        flushed_lines = live.height;
    }

    void on_start(const StreamEvent& e) {
        active = true;
        current = ChartInfo{};
        current.start_s = e.time_s;
        current.start_utc = t0 + static_cast<std::int64_t>(std::floor(e.time_s));
        current.stem = utc_stamp(current.start_utc) + "_" + cfg.label;
        // Two charts starting in the same minute (forced restart): keep both.
        for (int k = 2; fs::exists(dir / file(".png")) || fs::exists(dir / file("_live.png"));
             ++k) {
            current.stem = utc_stamp(current.start_utc) + "_" + cfg.label + "-" + std::to_string(k);
        }
        current.ioc = e.ioc;
        current.apt_start = e.apt_start;
        current.signal_start = e.signal_start;
        live = GrayImage{};
        flushed_lines = 0;
        say("start " + current.stem + " (IOC " + std::to_string(static_cast<int>(e.ioc)) + ", " +
            start_reason() + ")");
    }

    const char* start_reason() const {
        if (current.apt_start) return "APT tone";
        return current.signal_start ? "fax signal, no APT" : "forced";
    }

    void on_locked(const StreamEvent& e) {
        current.phasing_found = e.phasing_found;
        say("lock " + current.stem + ": " + fmt("%+.1f ppm", e.clock_ppm) + ", " +
            fmt("%+.0f Hz", e.tuning_offset_hz) +
            (e.phasing_found ? ", phasing found" : ", no phasing"));
    }

    void on_line(const StreamEvent& e) {
        if (live.width == 0) live.width = e.pixels.size();
        if (e.pixels.size() != live.width) return;
        live.pixels.insert(live.pixels.end(), e.pixels.begin(), e.pixels.end());
        live.height = live.pixels.size() / live.width;
        if (cfg.live_flush_lines > 0 && live.height - flushed_lines >= cfg.live_flush_lines) {
            flush_live();
        }
    }

    void on_complete(const StreamEvent& e) {
        current.end_s = e.time_s;
        current.live_lines = live.height;
        if (e.discarded) {
            std::error_code ec;
            fs::remove(dir / file("_live.png"), ec);
            say("discard " + current.stem + ": " + std::to_string(live.height) +
                " lines, a fragment without APT, not a chart");
            active = false;
            live = GrayImage{};
            return;
        }
        if (live.height > 0) {
            flush_live();
            current.files.push_back(file("_live.png"));
        }
        if (e.final) {
            const auto& r = e.final->report;
            write_png_atomic(dir / file(".png"), e.final->image);
            current.files.push_back(file(".png"));
            current.lines = e.final->image.height;
            current.clock_ppm = r.clock_ppm;
            current.tuning_offset_hz = r.tuning_offset_hz;
            current.phasing_found = r.phasing_found;
            current.quality = mean_quality(r);
        }
        if (e.enhanced) {
            write_png_atomic(dir / file("_enh.png"), e.enhanced->image);
            current.files.push_back(file("_enh.png"));
            current.clicks_repaired = e.enhanced->report.clicks_repaired;
        }
        write_json();
        current.files.push_back(file(".json"));
        say("complete " + current.stem + ": " + std::to_string(current.lines) + " lines, " +
            fmt("%+.1f ppm", current.clock_ppm) + ", " +
            fmt("quality %.0f %%", current.quality * 100.0) + (e.final ? "" : " (no final image)"));
        done.push_back(current);
        active = false;
        live = GrayImage{};
    }

    void write_json() const {
        const fs::path path = dir / file(".json");
        fs::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write " + tmp.string());
            out << "{\n"
                << "  \"stem\": \"" << json_escape(current.stem) << "\",\n"
                << "  \"label\": \"" << json_escape(cfg.label) << "\",\n"
                << "  \"start_utc\": \"" << utc_stamp(current.start_utc) << "\",\n"
                << "  \"start_unix\": " << current.start_utc << ",\n"
                << "  \"duration_s\": " << fmt("%.1f", current.end_s - current.start_s) << ",\n"
                << "  \"ioc\": " << static_cast<int>(current.ioc) << ",\n"
                << "  \"apt_start\": " << (current.apt_start ? "true" : "false") << ",\n"
                << "  \"start\": \""
                << (current.apt_start      ? "apt"
                    : current.signal_start ? "signal"
                                           : "forced")
                << "\",\n"
                << "  \"phasing_found\": " << (current.phasing_found ? "true" : "false") << ",\n"
                << "  \"live_lines\": " << current.live_lines << ",\n"
                << "  \"lines\": " << current.lines << ",\n"
                << "  \"clock_ppm\": " << fmt("%.2f", current.clock_ppm) << ",\n"
                << "  \"tuning_offset_hz\": " << fmt("%.1f", current.tuning_offset_hz) << ",\n"
                << "  \"quality\": " << fmt("%.3f", current.quality) << ",\n"
                << "  \"enhancements\": [" << (cfg.enhanced ? "\"clicks\"" : "") << "],\n"
                << "  \"clicks_repaired\": " << current.clicks_repaired << "\n"
                << "}\n";
        }
        fs::rename(tmp, path);
    }

    void drain() {
        for (const auto& e : decoder.take_events()) {
            switch (e.kind) {
                case StreamEvent::Kind::kStart: on_start(e); break;
                case StreamEvent::Kind::kLocked: on_locked(e); break;
                case StreamEvent::Kind::kLine: on_line(e); break;
                case StreamEvent::Kind::kComplete: on_complete(e); break;
            }
        }
    }
};

Receiver::Receiver(int sample_rate, ReceiverConfig config, std::int64_t stream_start_utc)
    : impl_(std::make_unique<Impl>(*this, sample_rate, std::move(config), stream_start_utc)) {}

Receiver::~Receiver() = default;

void Receiver::push(const float* samples, std::size_t count) {
    impl_->decoder.push(samples, count);
    impl_->drain();
}

void Receiver::force_start() {
    impl_->decoder.force_start();
    impl_->drain();
}

void Receiver::finish() {
    if (impl_->decoder.receiving()) impl_->decoder.force_stop();
    impl_->drain();
}

bool Receiver::receiving() const noexcept {
    return impl_->decoder.receiving();
}

std::string Receiver::current_stem() const {
    return impl_->active ? impl_->current.stem : "";
}

std::vector<ChartInfo> Receiver::take_completed() {
    std::vector<ChartInfo> out;
    out.swap(impl_->done);
    return out;
}

}  // namespace owfax::engine
