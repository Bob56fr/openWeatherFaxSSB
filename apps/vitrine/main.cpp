// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// owfax-vitrine: builds the chart packages of the web showcase (docs/VITRINE-WEB.md,
// lot 1) from a capture recording. Sends nothing: publishing is a later step.
//
//   owfax-vitrine package <capture.wav> --catalog <dir> --locator <QTH> [options]
//
// Every chart found in the recording becomes a directory <out>/<chart_id>/ with
// raw.webp, enh.webp, thumb.webp, audio.m4a (encoded with ffmpeg), meta.json and
// telemetry.json. The capture's sidecar files are used when present:
//   <capture>.json         start_utc, assigned_khz, receiver, rig_settings
//   <capture>.smeter.csv   S-meter readings (tools/capture/ic7300_capture.sh)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "owfax/audio/wav.hpp"
#include "owfax/build_info.hpp"
#include "owfax/image/gray_image.hpp"
#include "owfax/net/json.hpp"
#include "owfax/vitrine/catalog.hpp"
#include "owfax/vitrine/package.hpp"
#include "owfax/wefax/decoder.hpp"

namespace fs = std::filesystem;
using namespace owfax;

namespace {

constexpr std::size_t kThumbWidth = 400;

void usage() {
    std::fputs(
        "Usage:\n"
        "  owfax-vitrine package <capture.wav> --catalog <dir> --locator <QTH> [options]\n"
        "\n"
        "Builds one showcase package per chart found in the recording (nothing is sent).\n"
        "\n"
        "Options:\n"
        "  --out <dir>             output directory (default: vitrine-out)\n"
        "  --khz <kHz>             assigned frequency (default: <capture>.json or file name)\n"
        "  --start-utc <ISO>       start of the recording, e.g. 2026-11-10T14:24:05Z\n"
        "                          (default: <capture>.json or file name)\n"
        "  --smeter <csv>          S-meter readings (default: <capture>.smeter.csv if present)\n"
        "  --label <text>          label in the chart id (default: the station callsign)\n"
        "  --rig <text> --input <text> --mode <text> --dial-khz <kHz>\n"
        "                          receiver details shown on the site\n"
        "  --min-quality <0..1>    publication threshold (default 0.60)\n"
        "  --min-lines <n>         complete chart without APT stop (default 600)\n"
        "  --only-publishable      write only the charts that pass the selection rules\n"
        "  --ffmpeg <path>         ffmpeg executable (default: ffmpeg)\n"
        "  --no-encode             keep PNG and WAV, do not run ffmpeg\n"
        "  --keep-intermediate     keep the PNG and WAV next to the encoded files\n",
        stderr);
}

struct Options {
    std::string wav;
    std::string catalog;
    std::string locator;
    std::string out = "vitrine-out";
    std::optional<double> khz;
    std::optional<std::int64_t> start_unix;
    std::string smeter;
    std::string label;
    std::string rig, input, mode;
    std::optional<double> dial_khz;
    vitrine::SelectionRules rules;
    bool only_publishable = false;
    std::string ffmpeg = "ffmpeg";
    bool encode = true;
    bool keep_intermediate = false;
};

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/// "2026-11-10T14:24:05Z", "2026-11-10T14:24Z" or "20261110T1424Z".
std::optional<std::int64_t> parse_utc(const std::string& s) {
    static const std::regex iso(R"((\d{4})-?(\d{2})-?(\d{2})T(\d{2}):?(\d{2})(?::?(\d{2}))?Z?)");
    std::smatch m;
    if (!std::regex_match(s, m, iso)) return std::nullopt;
    const int y = std::stoi(m[1]);
    const auto mo = static_cast<unsigned>(std::stoi(m[2]));
    const auto d = static_cast<unsigned>(std::stoi(m[3]));
    const int h = std::stoi(m[4]), mi = std::stoi(m[5]);
    const int sec = m[6].matched ? std::stoi(m[6]) : 0;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return std::nullopt;
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec;
}

std::optional<double> parse_double(const std::string& s) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return std::nullopt;
    return v;
}

bool parse(int argc, char** argv, Options& o) {
    if (argc < 3 || std::strcmp(argv[1], "package") != 0) return false;
    o.wav = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](std::string& dst) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "owfax-vitrine: %s needs a value\n", a.c_str());
                return false;
            }
            dst = argv[++i];
            return true;
        };
        auto number = [&](std::optional<double>& dst) {
            std::string v;
            if (!value(v)) return false;
            dst = parse_double(v);
            if (!dst)
                std::fprintf(stderr, "owfax-vitrine: %s: not a number: %s\n", a.c_str(), v.c_str());
            return dst.has_value();
        };
        std::string v;
        std::optional<double> n;
        bool ok = true;
        if (a == "--catalog")
            ok = value(o.catalog);
        else if (a == "--locator")
            ok = value(o.locator);
        else if (a == "--out")
            ok = value(o.out);
        else if (a == "--khz")
            ok = number(o.khz);
        else if (a == "--start-utc") {
            ok = value(v);
            if (ok) {
                o.start_unix = parse_utc(v);
                ok = o.start_unix.has_value();
                if (!ok) std::fprintf(stderr, "owfax-vitrine: bad --start-utc: %s\n", v.c_str());
            }
        } else if (a == "--smeter")
            ok = value(o.smeter);
        else if (a == "--label")
            ok = value(o.label);
        else if (a == "--rig")
            ok = value(o.rig);
        else if (a == "--input")
            ok = value(o.input);
        else if (a == "--mode")
            ok = value(o.mode);
        else if (a == "--dial-khz")
            ok = number(o.dial_khz);
        else if (a == "--min-quality") {
            ok = number(n);
            if (ok) o.rules.min_quality = *n;
        } else if (a == "--min-lines") {
            ok = number(n);
            if (ok) o.rules.min_lines = static_cast<std::size_t>(*n);
        } else if (a == "--only-publishable")
            o.only_publishable = true;
        else if (a == "--ffmpeg")
            ok = value(o.ffmpeg);
        else if (a == "--no-encode")
            o.encode = false;
        else if (a == "--keep-intermediate")
            o.keep_intermediate = true;
        else {
            std::fprintf(stderr, "owfax-vitrine: unknown option %s\n", a.c_str());
            return false;
        }
        if (!ok) return false;
    }
    if (o.catalog.empty() || o.locator.empty()) {
        std::fputs("owfax-vitrine: --catalog and --locator are required\n", stderr);
        return false;
    }
    return true;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& p, const std::string& text) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out << text;
    }
    fs::rename(tmp, p);
}

/// Sidecar <capture>.json written by tools/capture/ic7300_capture.sh.
void apply_sidecar(const fs::path& wav, Options& o) {
    fs::path side = wav;
    side.replace_extension(".json");
    if (fs::exists(side)) {
        if (const auto j = net::parse_json(read_file(side))) {
            const net::Json* v = nullptr;
            if (!o.start_unix && (v = j->get("start_utc")) && v->is(net::Json::Type::kString)) {
                o.start_unix = parse_utc(v->string);
            }
            if (!o.khz && (v = j->get("assigned_khz")) && v->is(net::Json::Type::kNumber)) {
                o.khz = v->number;
            }
            if (o.rig.empty() && (v = j->get("receiver")) && v->is(net::Json::Type::kString)) {
                o.rig = v->string;
            }
            if ((v = j->get("rig_settings")) && v->is(net::Json::Type::kString)) {
                // "Tuned 4610 kHz (dial 4608100 Hz PKTUSB), AGC slow, ..."
                static const std::regex dial(R"(dial (\d+) Hz (\w+))");
                std::smatch m;
                if (std::regex_search(v->string, m, dial)) {
                    if (!o.dial_khz) o.dial_khz = std::stod(m[1]) / 1000.0;
                    if (o.mode.empty()) o.mode = m[2];
                }
            }
            if (o.input.empty()) o.input = "usb-audio";
        }
    }
    // File name "20261110T1424Z_DWD_13882.5kHz.wav" (corpus, captures).
    static const std::regex name(R"((\d{8}T\d{4}Z)_.*_([0-9.]+)kHz)");
    std::smatch m;
    const std::string stem = wav.stem().string();
    if (std::regex_search(stem, m, name)) {
        if (!o.start_unix) o.start_unix = parse_utc(m[1]);
        if (!o.khz) o.khz = parse_double(m[2]);
    }
    if (o.smeter.empty()) {
        fs::path csv = wav;
        csv.replace_extension(".smeter.csv");
        if (fs::exists(csv)) o.smeter = csv.string();
    }
}

std::string quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
#endif
}

bool run(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    if (rc != 0) std::fprintf(stderr, "owfax-vitrine: command failed (%d): %s\n", rc, cmd.c_str());
    return rc == 0;
}

bool encode_webp(const Options& o, const fs::path& png, const fs::path& webp, int quality) {
    return run(quote(o.ffmpeg) + " -nostdin -y -v error -i " + quote(png.string()) +
               " -c:v libwebp -quality " + std::to_string(quality) + " " + quote(webp.string()));
}

/// AAC-LC mono 32 kbit/s at 16 kHz in MP4: plays everywhere, Safari iOS 15 included (PUB-030).
bool encode_audio(const Options& o, const fs::path& wav, const fs::path& m4a) {
    return run(quote(o.ffmpeg) + " -nostdin -y -v error -i " + quote(wav.string()) +
               " -ac 1 -ar 16000 -c:a aac -b:a 32k -movflags +faststart " + quote(m4a.string()));
}

/// File size in MB (0 if missing).
double mb_of(const fs::path& p) {
    std::error_code ec;
    const auto s = fs::file_size(p, ec);
    return ec ? 0.0 : static_cast<double>(s) / 1e6;
}

int package(Options& o) {
    apply_sidecar(o.wav, o);
    if (!o.khz || !o.start_unix) {
        std::fputs("owfax-vitrine: frequency or start time unknown: give --khz and --start-utc\n",
                   stderr);
        return 2;
    }
    vitrine::ReceiverRef rx;
    const auto qth = vitrine::from_locator(o.locator);
    if (!qth) {
        std::fprintf(stderr, "owfax-vitrine: bad locator %s\n", o.locator.c_str());
        return 2;
    }
    rx.position = *qth;
    rx.locator = vitrine::to_locator(*qth, 6);  // never more precise on the site (PUB-001)
    rx.rig = o.rig;
    rx.input = o.input;
    rx.mode = o.mode;
    rx.dial_khz = o.dial_khz;

    std::string error;
    const auto station = vitrine::find_station(o.catalog, *o.khz, error);
    if (!station) {
        std::fprintf(stderr, "owfax-vitrine: %s\n", error.c_str());
        return 2;
    }
    const std::string label = o.label.empty() ? station->callsign : o.label;

    std::vector<vitrine::SmeterReading> smeter;
    if (!o.smeter.empty()) smeter = vitrine::read_smeter_csv(o.smeter, *o.start_unix);

    const audio::AudioBuffer rec = audio::read_wav(o.wav);
    wefax::DecodeOptions opt;
    opt.shift_hz = station->shift_hz;
    wefax::DecodeOptions enh = opt;
    enh.suppress_clicks = true;  // the validated enhancements (UI-008)

    const auto charts = wefax::decode_all(rec, opt);
    std::printf("%s: %zu transmission(s), station %s (%s), %zu S-meter readings\n", o.wav.c_str(),
                charts.size(), station->id.c_str(), station->callsign.c_str(), smeter.size());
    int written = 0;
    for (std::size_t i = 0; i < charts.size(); ++i) {
        vitrine::ChartSource src;
        src.recording = &rec;
        src.recording_start_unix = *o.start_unix;
        src.raw = &charts[i];
        src.min_s = i > 0 ? charts[i - 1].report.image_end_s : 0.0;
        if (i + 1 < charts.size()) {
            const auto& next = charts[i + 1].report;
            src.max_s = next.apt_start_found && next.apt_start_s >= 0.0 ? next.apt_start_s
                                                                        : next.image_start_s;
        } else {
            src.max_s = rec.duration_s();
        }
        const auto p = vitrine::build_package(src, *station, rx, label, smeter, o.rules, enh);

        std::string verdict = "publishable";
        if (!p.selection.publish) {
            verdict = "not published:";
            for (const auto& r : p.selection.reasons) verdict += " " + r;
        }
        std::printf("  %s  %4zu lines  quality %3.0f %%  -> %s\n", p.chart_id.c_str(), p.raw.height,
                    vitrine::mean_quality(p.report) * 100.0, verdict.c_str());
        if (o.only_publishable && !p.selection.publish) continue;

        const fs::path dir = fs::path(o.out) / p.chart_id;
        fs::create_directories(dir);
        vitrine::PackageFiles files;
        write_png((dir / "raw.png").string(), p.raw);
        if (p.enhanced) write_png((dir / "enh.png").string(), *p.enhanced);
        write_png((dir / "thumb.png").string(),
                  shrink_to_width(p.enhanced ? *p.enhanced : p.raw, kThumbWidth));
        audio::write_wav((dir / "audio.wav").string(), p.audio);
        if (o.encode) {
            bool ok = encode_webp(o, dir / "raw.png", dir / "raw.webp", 85) &&
                      encode_webp(o, dir / "thumb.png", dir / "thumb.webp", 80) &&
                      encode_audio(o, dir / "audio.wav", dir / "audio.m4a");
            if (ok && p.enhanced) ok = encode_webp(o, dir / "enh.png", dir / "enh.webp", 85);
            if (!ok) {
                std::fputs(
                    "owfax-vitrine: encoding failed (is ffmpeg installed with libwebp? "
                    "--no-encode keeps PNG and WAV)\n",
                    stderr);
                return 1;
            }
            if (!o.keep_intermediate) {
                for (const char* f : {"raw.png", "enh.png", "thumb.png", "audio.wav"}) {
                    std::error_code ec;
                    fs::remove(dir / f, ec);
                }
            }
        } else {
            files.raw = "raw.png";
            files.enhanced = "enh.png";
            files.thumb = "thumb.png";
            files.audio = "audio.wav";
        }
        if (!p.enhanced) files.enhanced.clear();
        write_file(dir / "meta.json", vitrine::meta_json(p, *station, rx, files));
        write_file(dir / "telemetry.json", vitrine::telemetry_json(p));

        double total = 0.0;
        for (const auto& e : fs::directory_iterator(dir)) total += mb_of(e.path());
        std::printf("      %s  %.0f s audio  %.2f MB (audio %.2f MB, raw %.2f MB)\n",
                    dir.string().c_str(), p.audio.duration_s(), total, mb_of(dir / files.audio),
                    mb_of(dir / files.raw));
        ++written;
    }
    std::printf("%d package(s) written to %s\n", written, o.out.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        usage();
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        const auto& b = build_info();
        std::printf("owfax-vitrine %.*s (%.*s)\n", static_cast<int>(b.version.size()),
                    b.version.data(), static_cast<int>(b.git_revision.size()),
                    b.git_revision.data());
        return 0;
    }
    Options o;
    if (!parse(argc, argv, o)) {
        usage();
        return 2;
    }
    try {
        return package(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "owfax-vitrine: %s\n", e.what());
        return 1;
    }
}
