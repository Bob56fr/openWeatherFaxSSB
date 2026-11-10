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
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "owfax/build_info.hpp"
#include "owfax/engine/receiver.hpp"
#include "owfax/io/audio_input.hpp"

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
    std::optional<std::int64_t> start_unix;
    double duration_s = 0.0;  // 0 = until the end of the stream or a signal
    bool force_start = false;
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
        "  --duration <s>        stop after this many seconds of audio\n"
        "  --start-unix <t>      UTC time of the first sample (default: now)\n"
        "  --journal <file>      also append the journal to this file\n"
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
        } else if (a == "--duration") {
            if ((v = value(i)) == nullptr) return false;
            o.duration_s = std::atof(v);
        } else if (a == "--start-unix") {
            if ((v = value(i)) == nullptr) return false;
            o.start_unix = std::strtoll(v, nullptr, 10);
        } else if (a == "--journal") {
            if ((v = value(i)) == nullptr) return false;
            o.journal = v;
        } else {
            std::fprintf(stderr, "owfaxd: unknown option '%s' (see --help)\n", a.c_str());
            return false;
        }
    }
    if (o.input.empty()) {
        std::fputs("owfaxd: --input is required (see --help)\n", stderr);
        return false;
    }
    if (o.in.sample_rate <= 0) {
        std::fputs("owfaxd: invalid --rate\n", stderr);
        return false;
    }
    return true;
}

void write_status(const fs::path& dir, const std::string& input, const std::string& state,
                  const std::string& stem, double audio_s, std::size_t queued, std::size_t overruns,
                  std::size_t dropped, std::size_t charts) {
    const fs::path path = dir / "status.json";
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        char audio[32];
        std::snprintf(audio, sizeof audio, "%.1f", audio_s);
        out << "{\n"
            << "  \"updated_utc\": \"" << utc_text(now_unix()) << "\",\n"
            << "  \"input\": \"" << owfax::engine::json_escape(input) << "\",\n"
            << "  \"state\": \"" << state << "\",\n"
            << "  \"current\": \"" << owfax::engine::json_escape(stem) << "\",\n"
            << "  \"audio_s\": " << audio << ",\n"
            << "  \"queued_samples\": " << queued << ",\n"
            << "  \"overruns\": " << overruns << ",\n"
            << "  \"dropped_samples\": " << dropped << ",\n"
            << "  \"charts\": " << charts << "\n"
            << "}\n";
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
}

int run(const Options& o) {
    Journal journal(o.journal);
    auto source = owfax::io::open_input(o.input, o.in);
    const int rate = source->sample_rate();
    const std::string input = source->description();
    const std::int64_t t0 = o.start_unix.value_or(now_unix());
    journal("owfaxd " + std::string(owfax::build_info().version) + " - input " + input +
            ", output " + o.rx.output_dir);

    owfax::engine::Receiver rx(rate, o.rx, t0);
    rx.log = [&](const std::string& m) { journal(m); };
    if (o.force_start) rx.force_start();

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

    std::size_t processed = 0;
    std::size_t charts = 0;
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
                rx.push(block->data(), block->size());
                processed += block->size();
            }
        } else if (queue.closed_and_empty()) {
            break;
        }
        for (const auto& c : rx.take_completed()) {
            ++charts;
            (void)c;
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
        if (now - last_status >= std::chrono::seconds(5)) {
            last_status = now;
            write_status(o.rx.output_dir, input, rx.receiving() ? "receiving" : "listening",
                         rx.current_stem(), static_cast<double>(processed) / rate, queue.size(),
                         source->overruns(), dropped.load(), charts);
        }
    }
    capture.join();
    rx.finish();
    charts += rx.take_completed().size();
    write_status(o.rx.output_dir, input, "stopped", "", static_cast<double>(processed) / rate, 0,
                 source->overruns(), dropped.load(), charts);
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
