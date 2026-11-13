// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// owfaxd — background reception engine (spec BG-001). Reads an audio input
// continuously, decodes every WEFAX transmission and writes the charts (live, raw,
// enhanced, metadata) to an output directory, with a journal and a status file.
//
// Receive only: owfaxd never talks to the transceiver (CAT comes later, spec CAT-*).
//
// Threads: a capture thread reads the device into a bounded queue; the main thread
// decodes. The final decode of a chart takes seconds on a Pi: the queue absorbs it,
// so the sound card never overruns.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "owfax/build_info.hpp"
#include "owfax/capabilities.hpp"
#include "owfax/dsp/spectrum.hpp"
#include "owfax/engine/receiver.hpp"
#include "owfax/io/audio_input.hpp"
#include "owfax/net/api.hpp"
#include "owfax/net/encoding.hpp"
#include "owfax/net/http_server.hpp"
#include "owfax/net/settings.hpp"

namespace {

namespace fs = std::filesystem;
using owfax::engine::utc_text;

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int /*sig*/) {
    g_stop.store(true);
}

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Options {
    std::string input;
    owfax::io::InputOptions in;
    owfax::engine::ReceiverConfig rx;
    std::string journal;
    std::string config;  // settings file edited from the web interface (skipper)
    std::optional<std::int64_t> start_unix;
    double duration_s = 0.0;  // 0 = until the end of the stream or a signal
    bool force_start = false;
    int http_port = 0;  // 0 = no HTTP API
    std::string http_bind = "0.0.0.0";
    std::string catalog_dir;
    std::string web_dir;
    std::string vitrine_dir;
    std::string publish_queue;
    bool keep_serving = false;
};

class Journal {
public:
    explicit Journal(const std::string& path) {
        if (!path.empty()) file_.open(path, std::ios::app);
    }
    void operator()(const std::string& msg) {
        const std::string line = utc_text(now_unix()) + "  " + msg + "\n";
        std::lock_guard<std::mutex> lock(mutex_);
        std::fputs(line.c_str(), stderr);
        if (file_) {
            file_ << line;
            file_.flush();
        }
    }

private:
    std::mutex mutex_;
    std::ofstream file_;
};

/// Bounded queue of audio blocks between the capture and processing threads.
/// Live sources drop the oldest audio when full (the device cannot wait); files and
/// pipes block the reader instead (`lossless`).
class BlockQueue {
public:
    BlockQueue(std::size_t capacity_samples, bool lossless)
        : capacity_(capacity_samples), lossless_(lossless) {}

    /// Returns the number of samples dropped to make room (oldest first).
    std::size_t push(std::vector<float> block) {
        std::size_t dropped = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (lossless_) {
                space_.wait(lock, [&] {
                    return size_ + block.size() <= capacity_ || size_ == 0 || closed_;
                });
            }
            size_ += block.size();
            blocks_.push_back(std::move(block));
            while (size_ > capacity_ && blocks_.size() > 1) {
                dropped += blocks_.front().size();
                size_ -= blocks_.front().size();
                blocks_.pop_front();
            }
        }
        cv_.notify_one();
        return dropped;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_one();
        space_.notify_one();
    }

    /// Waits up to `timeout` for a block. Empty optional on timeout or when closed and
    /// drained (check closed_and_empty()).
    std::optional<std::vector<float>> pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return !blocks_.empty() || closed_; });
        if (blocks_.empty()) return std::nullopt;
        std::vector<float> b = std::move(blocks_.front());
        blocks_.pop_front();
        size_ -= b.size();
        lock.unlock();
        space_.notify_one();
        return b;
    }

    bool closed_and_empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && blocks_.empty();
    }

    std::size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return size_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable space_;
    std::deque<std::vector<float>> blocks_;
    std::size_t size_ = 0;
    std::size_t capacity_;
    bool lossless_;
    bool closed_ = false;
};

void usage() {
    std::fputs(
        "owfaxd — openWeatherFaxSSB reception engine (receive only)\n"
        "\n"
        "Usage:\n"
        "  owfaxd --input <uri> [options]\n"
        "  owfaxd --list-inputs\n"
        "  owfaxd --version\n"
        "\n"
        "Inputs:\n"
        "  wav:<path>            recording (--realtime to play it at its own pace)\n"
        "  stdin                 raw s16le mono PCM at --rate (e.g. from arecord -t raw)\n"
        "  alsa:<device>         ALSA capture, e.g. alsa:plughw:CARD=Device,DEV=0\n"
        "\n"
        "Options:\n"
        "  --out <dir>           output directory (default: .)\n"
        "  --label <name>        label in file names (default: wefax)\n"
        "  --rate <hz>           device / stdin sample rate (default: 48000)\n"
        "  --realtime            WAV input: deliver at real-time pace\n"
        "  --shift <hz>          black-white shift of the station (default: 800; DWD 850)\n"
        "  --ioc <576|288>       force the IOC (default: from the APT start tone)\n"
        "  --lpm <60|90|120>     lines per minute (default: 120)\n"
        "  --no-enhanced         do not write <stem>_enh.png\n"
        "  --live-every <lines>  rewrite <stem>_live.png every N lines (default: 50)\n"
        "  --force-start         start a chart immediately (APT start missed)\n"
        "  --no-auto-start       start only on an APT tone, not on a fax signal without one\n"
        "  --duration <s>        stop after this many seconds of audio\n"
        "  --start-unix <t>      UTC time of the first sample (default: now)\n"
        "  --journal <file>      also append the journal to this file\n"
        "  --config <file>       settings and skipper password, edited from the web\n"
        "                        interface; its values override the options above\n"
        "                        (created at the first change; keep it outside --out)\n"
        "\n"
        "Local HTTP API (read only, local network clients only):\n"
        "  --http <port>         serve the API and web interface on this port\n"
        "  --http-bind <ipv4>    address to listen on (default: 0.0.0.0, all interfaces)\n"
        "  --catalog <dir>       station catalogue served at /api/v1/stations\n"
        "  --web <dir>           web interface files (default: a built-in page)\n"
        "  --vitrine <dir>       web showcase packages (owfax-vitrine package --out),\n"
        "                        served at /api/v1/vitrine for the replay player\n"
        "  --publish-queue <dir> queue of owfax-vitrine send: the Vitrine page shows\n"
        "                        whether publication to the web showcase is active\n"
        "  --keep-serving        keep the API up after the input ends (until a signal)\n"
        "\n"
        "Files per chart in --out: <stem>_live.png, <stem>.png (raw), <stem>_enh.png,\n"
        "<stem>.json; <stem> = YYYYMMDDTHHMMZ_<label>. status.json is refreshed every 5 s.\n",
        stdout);
}

bool parse(int argc, char** argv, Options& o) {
    auto value = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "owfaxd: %s needs a value\n", argv[i]);
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = nullptr;
        if (a == "--input") {
            if ((v = value(i)) == nullptr) return false;
            o.input = v;
        } else if (a == "--out") {
            if ((v = value(i)) == nullptr) return false;
            o.rx.output_dir = v;
        } else if (a == "--label") {
            if ((v = value(i)) == nullptr) return false;
            o.rx.label = v;
        } else if (a == "--rate") {
            if ((v = value(i)) == nullptr) return false;
            o.in.sample_rate = std::atoi(v);
        } else if (a == "--realtime") {
            o.in.realtime = true;
        } else if (a == "--shift") {
            if ((v = value(i)) == nullptr) return false;
            o.rx.options.shift_hz = std::atof(v);
        } else if (a == "--ioc") {
            if ((v = value(i)) == nullptr) return false;
            const int ioc = std::atoi(v);
            if (ioc != 576 && ioc != 288) {
                std::fputs("owfaxd: --ioc must be 576 or 288\n", stderr);
                return false;
            }
            o.rx.options.ioc = ioc == 576 ? owfax::wefax::Ioc::k576 : owfax::wefax::Ioc::k288;
        } else if (a == "--lpm") {
            if ((v = value(i)) == nullptr) return false;
            const int lpm = std::atoi(v);
            if (lpm == 60) {
                o.rx.options.lpm = owfax::wefax::Lpm::k60;
            } else if (lpm == 90) {
                o.rx.options.lpm = owfax::wefax::Lpm::k90;
            } else if (lpm == 120) {
                o.rx.options.lpm = owfax::wefax::Lpm::k120;
            } else {
                std::fputs("owfaxd: --lpm must be 60, 90 or 120\n", stderr);
                return false;
            }
        } else if (a == "--no-enhanced") {
            o.rx.enhanced = false;
        } else if (a == "--live-every") {
            if ((v = value(i)) == nullptr) return false;
            o.rx.live_flush_lines = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
        } else if (a == "--force-start") {
            o.force_start = true;
        } else if (a == "--no-auto-start") {
            o.rx.options.start_without_apt = false;
        } else if (a == "--duration") {
            if ((v = value(i)) == nullptr) return false;
            o.duration_s = std::atof(v);
        } else if (a == "--start-unix") {
            if ((v = value(i)) == nullptr) return false;
            o.start_unix = std::strtoll(v, nullptr, 10);
        } else if (a == "--config") {
            if ((v = value(i)) == nullptr) return false;
            o.config = v;
        } else if (a == "--journal") {
            if ((v = value(i)) == nullptr) return false;
            o.journal = v;
        } else if (a == "--http") {
            if ((v = value(i)) == nullptr) return false;
            o.http_port = std::atoi(v);
            if (o.http_port <= 0 || o.http_port > 65535) {
                std::fputs("owfaxd: --http needs a port number\n", stderr);
                return false;
            }
        } else if (a == "--http-bind") {
            if ((v = value(i)) == nullptr) return false;
            o.http_bind = v;
        } else if (a == "--catalog") {
            if ((v = value(i)) == nullptr) return false;
            o.catalog_dir = v;
        } else if (a == "--web") {
            if ((v = value(i)) == nullptr) return false;
            o.web_dir = v;
        } else if (a == "--vitrine") {
            if ((v = value(i)) == nullptr) return false;
            o.vitrine_dir = v;
        } else if (a == "--publish-queue") {
            if ((v = value(i)) == nullptr) return false;
            o.publish_queue = v;
        } else if (a == "--keep-serving") {
            o.keep_serving = true;
        } else {
            std::fprintf(stderr, "owfaxd: unknown option '%s' (see --help)\n", a.c_str());
            return false;
        }
    }
    if (o.input.empty() && o.config.empty()) {
        std::fputs("owfaxd: --input is required (see --help)\n", stderr);
        return false;
    }
    if (o.in.sample_rate <= 0) {
        std::fputs("owfaxd: invalid --rate\n", stderr);
        return false;
    }
    return true;
}

std::string fmt(const char* format, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, format, v);
    return buf;
}

/// Where the chart being received stands, for the screens (badge of the web interface).
struct Phase {
    std::string name = "listening";  ///< listening, phasing (start seen), image (decoding)
    std::string start;               ///< apt, signal or forced
    std::size_t lines = 0;           ///< live lines so far
};

/// Engine state: written to status.json and served at /api/v1/status.
std::string status_json(const std::string& input, const std::string& state, const std::string& stem,
                        double audio_s, std::size_t queued, std::size_t overruns,
                        std::size_t dropped, std::size_t charts, std::size_t live_clients,
                        const Phase& phase) {
    using owfax::engine::json_escape;
    return "{\n"
           "  \"phase\": \"" +
           phase.name + "\",\n  \"start\": \"" + phase.start +
           "\",\n  \"lines\": " + std::to_string(phase.lines) +
           ",\n"
           "  \"updated_utc\": \"" +
           utc_text(now_unix()) +
           "\",\n"
           "  \"input\": \"" +
           json_escape(input) +
           "\",\n"
           "  \"state\": \"" +
           state +
           "\",\n"
           "  \"current\": \"" +
           json_escape(stem) +
           "\",\n"
           "  \"audio_s\": " +
           fmt("%.1f", audio_s) +
           ",\n"
           "  \"queued_samples\": " +
           std::to_string(queued) +
           ",\n"
           "  \"overruns\": " +
           std::to_string(overruns) +
           ",\n"
           "  \"dropped_samples\": " +
           std::to_string(dropped) +
           ",\n"
           "  \"charts\": " +
           std::to_string(charts) +
           ",\n"
           "  \"live_clients\": " +
           std::to_string(live_clients) +
           "\n"
           "}";
}

void write_status(const fs::path& dir, const std::string& json) {
    const fs::path path = dir / "status.json";
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        out << json << "\n";
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
}

/// Live message for the WebSocket /api/v1/live (format: docs/api/openapi.yaml).
/// Diagnostics for /api/v1/diagnostics (spec UI-007): host, tier, inputs, options.
std::string diagnostics_json(const Options& o, const std::string& input, int rate) {
    using owfax::engine::json_escape;
    const auto caps = owfax::detect_host_capabilities();
    std::string j = "{\n  \"host\": {\"os\": \"" + json_escape(caps.os_family) +
                    "\", \"cpu_cores\": " + std::to_string(caps.cpu_cores) +
                    ", \"ram_bytes\": " + std::to_string(caps.ram_bytes) + ", \"tier\": \"" +
                    std::string(owfax::to_string(owfax::classify(caps))) + "\"},\n";
    j += "  \"input\": {\"uri\": \"" + json_escape(o.input) + "\", \"description\": \"" +
         json_escape(input) + "\", \"sample_rate\": " + std::to_string(rate) + "},\n";
    j += "  \"backends\": [";
    bool first = true;
    for (const auto& b : owfax::io::input_backends()) {
        j += (first ? "\"" : ", \"") + json_escape(b) + "\"";
        first = false;
    }
    j += "],\n  \"inputs\": [";
    first = true;
    for (const auto& d : owfax::io::list_inputs()) {
        j += std::string(first ? "\n" : ",\n") + "    {\"uri\": \"" + json_escape(d.uri) +
             "\", \"description\": \"" + json_escape(d.description) + "\"}";
        first = false;
    }
    j += first ? "],\n" : "\n  ],\n";
    j += "  \"options\": {\"label\": \"" + json_escape(o.rx.label) +
         "\", \"shift_hz\": " + fmt("%.0f", o.rx.options.shift_hz) +
         ", \"start_without_apt\": " + (o.rx.options.start_without_apt ? "true" : "false") +
         ", \"enhanced\": " + (o.rx.enhanced ? "true" : "false") + "}\n}";
    return j;
}

/// Input spectrum and level for the live clients (diagnostic page, UI-007).
std::string spectrum_json(const owfax::dsp::SpectrumFrame& f) {
    constexpr double kMin = -110.0;  // dB mapped to 0 … 255
    constexpr double kMax = 0.0;
    std::vector<std::uint8_t> bytes(f.db.size());
    for (std::size_t i = 0; i < f.db.size(); ++i) {
        const double v = (static_cast<double>(f.db[i]) - kMin) / (kMax - kMin) * 255.0;
        bytes[i] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
    }
    return "{\"type\": \"spectrum\", \"max_hz\": " + fmt("%.0f", f.max_hz) +
           ", \"db_min\": -110, \"db_max\": 0, \"bands\": \"" +
           owfax::net::base64_encode(bytes.data(), bytes.size()) +
           "\", \"rms_dbfs\": " + fmt("%.1f", f.rms_dbfs) +
           ", \"peak_dbfs\": " + fmt("%.1f", f.peak_dbfs) +
           ", \"clipped\": " + std::to_string(f.clipped) + "}";
}

std::string event_json(const owfax::wefax::StreamEvent& e, const std::string& stem) {
    using Kind = owfax::wefax::StreamEvent::Kind;
    using owfax::engine::json_escape;
    const std::string head = "{\"type\": \"";
    const std::string s = "\", \"stem\": \"" + json_escape(stem) + "\"";
    switch (e.kind) {
        case Kind::kStart:
            return head + "start" + s + ", \"ioc\": " + std::to_string(static_cast<int>(e.ioc)) +
                   ", \"start\": \"" +
                   (e.apt_start      ? "apt"
                    : e.signal_start ? "signal"
                                     : "forced") +
                   "\"}";
        case Kind::kLocked:
            return head + "lock" + s + ", \"clock_ppm\": " + fmt("%.2f", e.clock_ppm) +
                   ", \"tuning_offset_hz\": " + fmt("%.1f", e.tuning_offset_hz) +
                   ", \"phasing_found\": " + (e.phasing_found ? "true" : "false") + "}";
        case Kind::kLine:
            return head + "line" + s + ", \"index\": " + std::to_string(e.line_index) +
                   ", \"width\": " + std::to_string(e.pixels.size()) +
                   ", \"quality\": " + fmt("%.3f", static_cast<double>(e.quality)) +
                   ", \"pixels\": \"" +
                   owfax::net::base64_encode(e.pixels.data(), e.pixels.size()) + "\"}";
        case Kind::kComplete:
            return head + "complete" + s + ", \"discarded\": " + (e.discarded ? "true" : "false") +
                   ", \"lines\": " + std::to_string(e.final ? e.final->image.height : 0) + "}";
    }
    return "{}";
}

/// Settings chosen in the web interface (--config file) override the command line;
/// the audio input only at start-up, and only if one was chosen there.
void apply_settings(Options& o, const owfax::net::StationSettings& s, bool with_input) {
    o.rx.label = s.label;
    o.rx.options.shift_hz = s.shift_hz;
    o.rx.options.start_without_apt = s.start_without_apt;
    o.rx.enhanced = s.enhanced;
    if (with_input && !s.input.empty()) o.input = s.input;
}

int run(Options o) {
    Journal journal(o.journal);
    std::unique_ptr<owfax::net::SettingsStore> store;
    if (!o.config.empty()) {
        owfax::net::StationSettings seed;  // first start: the command line seeds the file
        seed.label = o.rx.label;
        seed.shift_hz = o.rx.options.shift_hz;
        seed.start_without_apt = o.rx.options.start_without_apt;
        seed.enhanced = o.rx.enhanced;
        seed.input = o.input;
        store = std::make_unique<owfax::net::SettingsStore>(o.config, seed);
        std::string err;
        if (!store->load(err)) throw std::runtime_error(err);
        apply_settings(o, store->settings(), true);
        journal("settings: " + o.config +
                (store->password_set() ? ""
                                       : " (no skipper password yet: create it in the web "
                                         "interface, Settings tab)"));
    }
    if (o.input.empty()) {
        throw std::runtime_error("no audio input: give --input, or choose one in the settings");
    }
    auto source = owfax::io::open_input(o.input, o.in);
    const int rate = source->sample_rate();
    const std::string input = source->description();
    const std::int64_t t0 = o.start_unix.value_or(now_unix());
    journal("owfaxd " + std::string(owfax::build_info().version) + " - input " + input +
            ", output " + o.rx.output_dir);

    // o is also read by the HTTP threads (diagnostics) while settings are applied.
    std::mutex options_mutex;
    std::mutex pending_mutex;
    std::optional<owfax::net::StationSettings> pending;  // changed, not applied yet
    std::atomic<bool> restart_requested{false};          // from the web interface
    std::function<void(const owfax::wefax::StreamEvent&, const std::string&)> on_event;
    auto make_receiver = [&](std::int64_t start_utc) {
        auto r = std::make_unique<owfax::engine::Receiver>(rate, o.rx, start_utc);
        r->log = [&](const std::string& m) { journal(m); };
        r->on_event = [&](const owfax::wefax::StreamEvent& e, const std::string& stem) {
            on_event(e, stem);
        };
        return r;
    };
    auto rx = make_receiver(t0);

    std::unique_ptr<owfax::net::Api> api;
    std::unique_ptr<owfax::net::HttpServer> server;
    bool status_dirty = true;
    if (o.http_port > 0) {
        std::error_code ec;
        if (!o.web_dir.empty() && !fs::exists(fs::path(o.web_dir) / "index.html", ec)) {
            throw std::runtime_error("--web " + o.web_dir +
                                     ": no index.html there (run from the repository root, or "
                                     "give the full path of the web directory)");
        }
        if (!o.catalog_dir.empty() && !fs::is_directory(o.catalog_dir, ec)) {
            throw std::runtime_error("--catalog " + o.catalog_dir + ": not a directory");
        }
        owfax::net::ApiConfig ac;
        ac.charts_dir = o.rx.output_dir;
        ac.catalog_dir = o.catalog_dir;
        ac.web_dir = o.web_dir;
        ac.vitrine_dir = o.vitrine_dir;
        ac.publish_queue = o.publish_queue;
        ac.input = input;
        ac.journal_path = o.journal;
        ac.diagnostics = [&o, &options_mutex, input, rate] {
            std::lock_guard<std::mutex> lock(options_mutex);
            return diagnostics_json(o, input, rate);
        };
        ac.settings = store.get();
        ac.running_input = o.input;
        ac.on_settings = [&](const owfax::net::StationSettings& s) {
            std::lock_guard<std::mutex> lock(pending_mutex);
            pending = s;  // applied by the decoding loop between two charts
        };
        // A service manager (systemd, BG-005) starts owfaxd again after it stops.
        ac.on_restart = [&] { restart_requested.store(true); };
        api = std::make_unique<owfax::net::Api>(ac);
        owfax::net::HttpServer::Options so;
        so.bind = o.http_bind;
        so.port = o.http_port;
        server = std::make_unique<owfax::net::HttpServer>(so, api->handler());
        journal("http: API and web interface on port " + std::to_string(server->port()) + " (" +
                o.http_bind + "), local network clients only");
    }
    Phase phase;
    on_event = [&](const owfax::wefax::StreamEvent& e, const std::string& stem) {
        using Kind = owfax::wefax::StreamEvent::Kind;
        switch (e.kind) {
            case Kind::kStart:
                phase.name = "phasing";
                phase.start = e.apt_start ? "apt" : e.signal_start ? "signal" : "forced";
                phase.lines = 0;
                break;
            case Kind::kLocked: phase.name = "image"; break;
            case Kind::kLine: phase.lines = e.line_index + 1; break;
            case Kind::kComplete: phase = Phase{}; break;
        }
        if (e.kind != Kind::kLine) status_dirty = true;
        if (api) api->publish(event_json(e, stem));
    };
    if (o.force_start) rx->force_start();

    // Up to 10 minutes of audio waiting for the decoder before the oldest is dropped.
    BlockQueue queue(static_cast<std::size_t>(rate) * 600U, !source->is_live());
    std::atomic<std::size_t> dropped{0};
    std::string capture_error;
    const auto limit = o.duration_s > 0.0 ? static_cast<std::size_t>(o.duration_s * rate)
                                          : static_cast<std::size_t>(-1);
    std::thread capture([&] {
        try {
            std::size_t total = 0;
            const auto block = static_cast<std::size_t>(rate / 10);  // 0.1 s
            while (!g_stop.load() && total < limit) {
                std::vector<float> buf(std::min(block, limit - total));
                const std::size_t n = source->read(buf.data(), buf.size());
                if (n == 0) break;
                buf.resize(n);
                total += n;
                dropped += queue.push(std::move(buf));
            }
        } catch (const std::exception& e) {
            capture_error = e.what();
        }
        queue.close();
    });

    owfax::dsp::SpectrumAnalyzer spectrum(rate);
    std::size_t processed = 0;
    std::size_t charts = 0;
    auto update_status = [&](const std::string& state, const std::string& stem) {
        const std::string json = status_json(
            input, state, stem, static_cast<double>(processed) / rate, queue.size(),
            source->overruns(), dropped.load(), charts, api ? api->live_clients() : 0, phase);
        write_status(o.rx.output_dir, json);
        if (api) {
            api->set_status(json);
            api->publish("{\"type\": \"status\", \"status\": " + json + "}");
        }
    };
    std::size_t reported_drop = 0;
    std::size_t reported_overruns = 0;
    auto last_status = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    bool interrupted = false;
    while (true) {
        if (g_stop.load() && !interrupted) {
            interrupted = true;
            journal("stop requested");
            source->interrupt();
        }
        if (auto block = queue.pop(std::chrono::milliseconds(200))) {
            // After a stop request, audio still queued is discarded: stop promptly.
            if (!interrupted) {
                rx->push(block->data(), block->size());
                if (api && api->live_clients() > 0) {
                    auto frame = spectrum.push(block->data(), block->size());
                    while (frame) {
                        api->publish(spectrum_json(*frame));
                        frame = spectrum.push(nullptr, 0);
                    }
                }
                processed += block->size();
            }
        } else if (queue.closed_and_empty()) {
            break;
        }
        for (const auto& c : rx->take_completed()) {
            ++charts;
            (void)c;
        }
        // Restart asked from the web interface (new audio input): stop cleanly, but
        // never in the middle of a chart.
        if (restart_requested.load() && !rx->receiving() && !g_stop.load()) {
            journal("restart requested from the web interface: stopping between two charts");
            g_stop.store(true);
        }
        // Settings changed in the web interface: applied between two charts, never
        // during a reception (a new receiver, its clock continuing the stream's).
        if (!rx->receiving()) {
            std::optional<owfax::net::StationSettings> p;
            {
                std::lock_guard<std::mutex> lock(pending_mutex);
                p.swap(pending);
            }
            if (p) {
                {
                    std::lock_guard<std::mutex> lock(options_mutex);
                    apply_settings(o, *p, false);
                }
                rx->finish();
                charts += rx->take_completed().size();
                rx = make_receiver(
                    t0 + static_cast<std::int64_t>(processed / static_cast<std::size_t>(rate)));
                journal("settings applied: label " + o.rx.label + ", shift " +
                        fmt("%.0f", o.rx.options.shift_hz) + " Hz, start without APT " +
                        (o.rx.options.start_without_apt ? "on" : "off") + ", enhanced image " +
                        (o.rx.enhanced ? "on" : "off") +
                        (!p->input.empty() && p->input != o.input
                             ? "; new audio input " + p->input + " at the next start"
                             : ""));
                status_dirty = true;
            }
        }
        if (dropped.load() > reported_drop) {
            reported_drop = dropped.load();
            journal("warning: decoder too slow, " + std::to_string(reported_drop) +
                    " samples dropped");
        }
        if (source->overruns() > reported_overruns) {
            reported_overruns = source->overruns();
            journal("warning: audio overrun (" + std::to_string(reported_overruns) + ")");
        }
        const auto now = std::chrono::steady_clock::now();
        if (status_dirty || now - last_status >= std::chrono::seconds(5)) {
            last_status = now;
            status_dirty = false;
            update_status(rx->receiving() ? "receiving" : "listening", rx->current_stem());
        }
    }
    capture.join();
    rx->finish();
    charts += rx->take_completed().size();
    update_status("stopped", "");
    if (server && o.keep_serving && !g_stop.load()) {
        journal("end of input: " + std::to_string(charts) +
                " chart(s); API still served until stopped");
        while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (server) server->stop();
    if (!capture_error.empty()) {
        journal("error: " + capture_error);
        return 1;
    }
    journal("end of input: " + std::to_string(charts) + " chart(s)");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        usage();
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        const auto& b = owfax::build_info();
        std::printf("owfaxd %s (%s), %s\nSource: %s\n", std::string(b.version).c_str(),
                    std::string(b.git_revision).c_str(), std::string(b.license).c_str(),
                    std::string(b.source_url).c_str());
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--list-inputs") == 0) {
        std::printf("Back-ends:");
        for (const auto& b : owfax::io::input_backends()) std::printf(" %s", b.c_str());
        std::printf("\n");
        for (const auto& d : owfax::io::list_inputs()) {
            std::printf("%-45s %s\n", d.uri.c_str(), d.description.c_str());
        }
        return 0;
    }
    Options o;
    if (!parse(argc, argv, o)) return 2;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    try {
        return run(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "owfaxd: %s\n", e.what());
        return 1;
    }
}
