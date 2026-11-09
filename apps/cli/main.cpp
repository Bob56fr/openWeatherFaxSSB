// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// owfax-cli — command line front-end to the core (development and headless use).

#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "owfax/audio/resample.hpp"
#include "owfax/audio/wav.hpp"
#include "owfax/build_info.hpp"
#include "owfax/capabilities.hpp"
#include "owfax/wefax/decoder.hpp"
#include "owfax/wefax/modulator.hpp"
#include "owfax/wefax/probe.hpp"

namespace {

using Args = std::vector<std::string>;

void print_usage() {
    std::printf(
        "Usage: owfax-cli <command> [options]\n"
        "\n"
        "  --version                     Version, license and source location\n"
        "  --capabilities                Detected host capabilities and hardware tier\n"
        "\n"
        "  decode <in.wav> <out.png>     Decode a WEFAX recording\n"
        "      --ioc 576|288             Force the IOC (default: from APT start tone)\n"
        "      --lpm 120|90|60           Lines per minute (default 120)\n"
        "      --ppm <value>             Force the sound card clock error (default: auto)\n"
        "      --shift <Hz>              Station black/white shift (default 800, DWD 850)\n"
        "  decode-all <in.wav> <prefix>  Split a long recording at each APT start and decode\n"
        "                                every transmission to <prefix>_NN.png (same options)\n"
        "\n"
        "  synth <out.wav>               Generate a synthetic WEFAX test signal\n"
        "      --lines <n>               Image lines (default 300)\n"
        "      --rate <Hz>               Sample rate (default 11025)\n"
        "      --ioc 576|288             IOC (default 576)\n"
        "      --offset <Hz>             Simulated SSB mistuning\n"
        "      --ppm <value>             Simulated sound card clock error\n"
        "      --snr <dB>                White noise, SNR in 2.5 kHz\n"
        "      --no-start                Omit the APT start tone (missed start)\n"
        "      --png <chart.png>         Also write the source test chart\n"
        "\n"
        "  probe <in.wav>                Quick WEFAX presence/quality check (one line, CSV)\n"
        "\n"
        "  cut <in.wav> <out.wav>        Extract and/or resample a recording\n"
        "      --from <s> --to <s>       Time range (default: whole file)\n"
        "      --rate <Hz>               Output sample rate (default: unchanged)\n");
}

int cmd_probe(const Args& args) {
    if (args.size() != 1) {
        print_usage();
        return 2;
    }
    const auto audio = owfax::audio::read_wav(args[0]);
    const auto r = owfax::wefax::probe(audio);
    std::printf(
        "score=%.2f rms_dbfs=%.1f clipped=%.4f band=%.2f fm_in_range=%.2f apt_start=%.2f "
        "apt_stop=%.2f spread_hz=%.0f\n",
        r.score, r.rms_dbfs, r.clipped_fraction, r.band_fraction, r.fm_in_range, r.apt_start_ratio,
        r.apt_stop_ratio, r.freq_spread_hz);
    return 0;
}

void print_version() {
    const auto& info = owfax::build_info();
    std::printf("owfax-cli %.*s (%.*s)\n", static_cast<int>(info.version.size()),
                info.version.data(), static_cast<int>(info.git_revision.size()),
                info.git_revision.data());
    std::printf("License: %.*s — this program comes with ABSOLUTELY NO WARRANTY.\n",
                static_cast<int>(info.license.size()), info.license.data());
    std::printf("Source:  %.*s\n", static_cast<int>(info.source_url.size()),
                info.source_url.data());
}

void print_capabilities() {
    const owfax::HostCapabilities caps = owfax::detect_host_capabilities();
    const auto tier = owfax::to_string(owfax::classify(caps));
    std::printf("OS family:     %s\n", caps.os_family.c_str());
    std::printf("CPU cores:     %u\n", caps.cpu_cores);
    std::printf("RAM:           %llu MiB\n",
                static_cast<unsigned long long>(caps.ram_bytes / (1024ULL * 1024ULL)));
    std::printf("Hardware tier: %.*s\n", static_cast<int>(tier.size()), tier.data());
}

/// Returns the value following `name` in args (and removes both), if present.
std::optional<std::string> take_option(Args& args, const char* name) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            std::string value = args[i + 1];
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                       args.begin() + static_cast<std::ptrdiff_t>(i + 2));
            return value;
        }
    }
    return std::nullopt;
}

bool take_flag(Args& args, const char* name) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == name) {
            args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

owfax::wefax::Ioc parse_ioc(const std::string& s) {
    if (s == "576") return owfax::wefax::Ioc::k576;
    if (s == "288") return owfax::wefax::Ioc::k288;
    throw std::invalid_argument("IOC must be 576 or 288");
}

owfax::wefax::Lpm parse_lpm(const std::string& s) {
    if (s == "120") return owfax::wefax::Lpm::k120;
    if (s == "90") return owfax::wefax::Lpm::k90;
    if (s == "60") return owfax::wefax::Lpm::k60;
    throw std::invalid_argument("LPM must be 120, 90 or 60");
}

void print_report(const owfax::wefax::DecodeResult& result) {
    const auto& r = result.report;
    double quality = 0.0;
    for (float q : r.line_quality) quality += static_cast<double>(q);
    if (!r.line_quality.empty()) quality /= static_cast<double>(r.line_quality.size());

    std::printf("APT start:      %s", r.apt_start_found ? "found" : "not found");
    if (r.apt_start_found) std::printf(" at %.1f s", r.apt_start_s);
    std::printf("\nIOC:            %d\n", static_cast<int>(r.ioc));
    std::printf("Phasing:        %s\n", r.phasing_found ? "found" : "not found");
    std::printf("APT stop:       %s\n", r.apt_stop_found ? "found" : "not found");
    std::printf("Black / white:  %.0f / %.0f Hz\n", r.black_hz, r.white_hz);
    std::printf("Tuning offset:  %+.1f Hz\n", r.tuning_offset_hz);
    std::printf("Clock error:    %+.1f ppm\n", r.clock_ppm);
    std::printf("Image:          %zu x %zu, %.1f s → %.1f s, quality %.0f %%\n", result.image.width,
                result.image.height, r.image_start_s, r.image_end_s, quality * 100.0);
    // Machine-readable: whole transmission incl. APT tones, for corpus extraction.
    const double seg_start = r.apt_start_found ? r.apt_start_s : r.image_start_s;
    std::printf("Segment:        %.1f %.1f\n", seg_start, r.image_end_s + 10.0);
}

int cmd_cut(Args args) {
    const auto from = take_option(args, "--from");
    const auto to = take_option(args, "--to");
    const auto rate = take_option(args, "--rate");
    if (args.size() != 2) {
        print_usage();
        return 2;
    }
    auto audio = owfax::audio::read_wav(args[0]);
    if (from || to) {
        audio = owfax::audio::slice(audio, from ? std::stod(*from) : 0.0,
                                    to ? std::stod(*to) : audio.duration_s());
    }
    if (rate) audio = owfax::audio::resample(audio, std::stoi(*rate));
    owfax::audio::write_wav(args[1], audio);
    std::printf("Written %s: %.1f s at %d Hz\n", args[1].c_str(), audio.duration_s(),
                audio.sample_rate);
    return 0;
}

owfax::wefax::DecodeOptions take_decode_options(Args& args) {
    owfax::wefax::DecodeOptions opt;
    if (auto v = take_option(args, "--ioc")) opt.ioc = parse_ioc(*v);
    if (auto v = take_option(args, "--lpm")) opt.lpm = parse_lpm(*v);
    if (auto v = take_option(args, "--ppm")) opt.clock_ppm = std::stod(*v);
    if (auto v = take_option(args, "--shift")) opt.shift_hz = std::stod(*v);
    return opt;
}

owfax::audio::AudioBuffer load(const std::string& path) {
    auto audio = owfax::audio::read_wav(path);
    std::printf("Input:          %s, %d Hz, %.1f s\n", path.c_str(), audio.sample_rate,
                audio.duration_s());
    return audio;
}

int cmd_decode_all(Args args) {
    const auto opt = take_decode_options(args);
    if (args.size() != 2) {
        print_usage();
        return 2;
    }
    const auto results = owfax::wefax::decode_all(load(args[0]), opt);
    std::printf("Transmissions:  %zu\n", results.size());
    for (std::size_t i = 0; i < results.size(); ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "_%02zu.png", i + 1);
        const std::string path = args[1] + name;
        std::printf("\n--- #%zu\n", i + 1);
        print_report(results[i]);
        owfax::write_png(path, results[i].image);
        std::printf("Written:        %s\n", path.c_str());
    }
    return results.empty() ? 1 : 0;
}

int cmd_decode(Args args) {
    const auto opt = take_decode_options(args);
    if (args.size() != 2) {
        print_usage();
        return 2;
    }
    const auto result = owfax::wefax::decode(load(args[0]), opt);
    print_report(result);
    if (result.image.height == 0) {
        std::fprintf(stderr, "No image lines decoded.\n");
        return 1;
    }
    owfax::write_png(args[1], result.image);
    std::printf("Written:        %s\n", args[1].c_str());
    return 0;
}

int cmd_synth(Args args) {
    owfax::wefax::ModulatorOptions opt;
    std::size_t lines = 300;
    std::optional<std::string> png;
    if (auto v = take_option(args, "--lines")) lines = static_cast<std::size_t>(std::stoul(*v));
    if (auto v = take_option(args, "--rate")) opt.sample_rate = std::stoi(*v);
    if (auto v = take_option(args, "--ioc")) opt.ioc = parse_ioc(*v);
    if (auto v = take_option(args, "--offset")) opt.tuning_offset_hz = std::stod(*v);
    if (auto v = take_option(args, "--shift")) opt.shift_hz = std::stod(*v);
    if (auto v = take_option(args, "--ppm")) opt.clock_ppm = std::stod(*v);
    if (auto v = take_option(args, "--snr")) opt.snr_db = std::stod(*v);
    if (take_flag(args, "--no-start")) opt.start_tone_s = 0.0;
    png = take_option(args, "--png");
    if (args.size() != 1) {
        print_usage();
        return 2;
    }
    const auto chart = owfax::wefax::make_test_chart(opt.ioc, lines);
    const auto audio = owfax::wefax::modulate(chart, opt);
    owfax::audio::write_wav(args[0], audio);
    std::printf("Written %s: %.1f s at %d Hz\n", args[0].c_str(), audio.duration_s(),
                audio.sample_rate);
    if (png) {
        owfax::write_png(*png, chart);
        std::printf("Written %s\n", png->c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return 0;
    }
    const std::string cmd = args[0];
    args.erase(args.begin());
    try {
        if (cmd == "--version") {
            print_version();
            return 0;
        }
        if (cmd == "--capabilities") {
            print_capabilities();
            return 0;
        }
        if (cmd == "decode") return cmd_decode(args);
        if (cmd == "synth") return cmd_synth(args);
        if (cmd == "probe") return cmd_probe(args);
        if (cmd == "decode-all") return cmd_decode_all(args);
        if (cmd == "cut") return cmd_cut(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr, "Unknown command: %s\n\n", cmd.c_str());
    print_usage();
    return 2;
}
