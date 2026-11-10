// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/io/audio_input.hpp"

#include <cstdio>
#include <stdexcept>

#include "alsa_input.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace owfax::io {
namespace {

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool ends_with_wav(const std::string& s) {
    if (s.size() < 4) return false;
    std::string tail = s.substr(s.size() - 4);
    for (auto& c : tail) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return tail == ".wav";
}

}  // namespace

std::unique_ptr<audio::AudioSource> open_input(const std::string& uri,
                                               const InputOptions& options) {
    if (starts_with(uri, "wav:") || (uri.find(':') == std::string::npos && ends_with_wav(uri)) ||
        (uri.size() > 2 && uri[1] == ':' && ends_with_wav(uri))) {  // C:\x.wav
        const std::string path = starts_with(uri, "wav:") ? uri.substr(4) : uri;
        auto audio = audio::read_wav(path);
        const std::string desc = "WAV " + path + " @ " + std::to_string(audio.sample_rate) + " Hz" +
                                 (options.realtime ? " (real time)" : "");
        return audio::make_buffer_source(std::move(audio), desc, options.realtime);
    }
    if (uri == "stdin" || uri == "-") {
#if defined(_WIN32)
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        return audio::make_pcm16_source(
            stdin, options.sample_rate,
            "stdin s16le @ " + std::to_string(options.sample_rate) + " Hz");
    }
    if (starts_with(uri, "alsa:")) {
#if defined(OWFAX_HAVE_ALSA)
        return open_alsa(uri.substr(5), options.sample_rate);
#else
        throw std::runtime_error("ALSA support was not built in (install libasound2-dev)");
#endif
    }
    throw std::runtime_error("unknown audio input '" + uri +
                             "' (expected wav:<path>, stdin or alsa:<device>)");
}

std::vector<InputDevice> list_inputs() {
    std::vector<InputDevice> out;
#if defined(OWFAX_HAVE_ALSA)
    for (auto& d : list_alsa()) out.push_back(std::move(d));
#endif
    return out;
}

std::vector<std::string> input_backends() {
    std::vector<std::string> b{"wav", "stdin"};
#if defined(OWFAX_HAVE_ALSA)
    b.emplace_back("alsa");
#endif
    return b;
}

}  // namespace owfax::io
