// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/audio/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace owfax::audio {
namespace {

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

double decode_sample(const std::uint8_t* p, std::uint16_t format, std::uint16_t bits) {
    if (format == kFormatFloat) {
        if (bits == 32) {
            const std::uint32_t raw = le32(p);
            float v = 0.0F;
            std::memcpy(&v, &raw, sizeof(v));
            return static_cast<double>(v);
        }
        const std::uint64_t raw =
            static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
        double v = 0.0;
        std::memcpy(&v, &raw, sizeof(v));
        return v;
    }
    switch (bits) {
        case 8: return (static_cast<double>(p[0]) - 128.0) / 128.0;
        case 16: return static_cast<double>(static_cast<std::int16_t>(le16(p))) / 32768.0;
        case 24: {
            std::int32_t v = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) |
                                                       (static_cast<std::uint32_t>(p[1]) << 8) |
                                                       (static_cast<std::uint32_t>(p[2]) << 16));
            if ((v & 0x800000) != 0) v -= 0x1000000;
            return static_cast<double>(v) / 8388608.0;
        }
        default: return static_cast<double>(static_cast<std::int32_t>(le32(p))) / 2147483648.0;
    }
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

void put_tag(std::vector<std::uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

}  // namespace

AudioBuffer read_wav(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    const std::size_t size = bytes.size();
    const std::uint8_t* p = bytes.data();
    if (size < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
        throw std::runtime_error(path + ": not a RIFF/WAVE file");
    }

    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t rate = 0;
    const std::uint8_t* data = nullptr;
    std::size_t data_size = 0;

    std::size_t pos = 12;
    while (pos + 8 <= size) {
        const std::uint8_t* id = p + pos;
        const std::size_t chunk_size = le32(p + pos + 4);
        const std::size_t body = pos + 8;
        const std::size_t avail = std::min(chunk_size, size - body);
        if (std::memcmp(id, "fmt ", 4) == 0 && avail >= 16) {
            format = le16(p + body);
            channels = le16(p + body + 2);
            rate = le32(p + body + 4);
            bits = le16(p + body + 14);
            if (format == kFormatExtensible && avail >= 26) format = le16(p + body + 24);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = p + body;
            data_size = avail;
        }
        if (avail < chunk_size) break;
        pos = body + chunk_size + (chunk_size & 1U);
    }

    const bool pcm_ok =
        format == kFormatPcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool float_ok = format == kFormatFloat && (bits == 32 || bits == 64);
    if (data == nullptr || channels == 0 || rate == 0 || rate > 1000000 || !(pcm_ok || float_ok)) {
        throw std::runtime_error(path + ": unsupported WAV format");
    }

    const std::size_t bytes_per_sample = bits / 8U;
    const std::size_t frame_size = bytes_per_sample * channels;
    const std::size_t frames = data_size / frame_size;

    AudioBuffer audio;
    audio.sample_rate = static_cast<int>(rate);
    audio.samples.resize(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        const std::uint8_t* frame = data + f * frame_size;
        double sum = 0.0;
        for (std::size_t c = 0; c < channels; ++c) {
            sum += decode_sample(frame + c * bytes_per_sample, format, bits);
        }
        audio.samples[f] = static_cast<float>(sum / channels);
    }
    return audio;
}

void write_wav(const std::string& path, const AudioBuffer& audio) {
    if (audio.sample_rate <= 0) throw std::runtime_error("write_wav: invalid sample rate");
    const auto rate = static_cast<std::uint32_t>(audio.sample_rate);
    const auto data_size = static_cast<std::uint32_t>(audio.samples.size() * 2U);

    std::vector<std::uint8_t> out;
    out.reserve(44 + data_size);
    put_tag(out, "RIFF");
    put32(out, 36 + data_size);
    put_tag(out, "WAVE");
    put_tag(out, "fmt ");
    put32(out, 16);
    put16(out, kFormatPcm);
    put16(out, 1);  // mono
    put32(out, rate);
    put32(out, rate * 2);  // byte rate
    put16(out, 2);         // block align
    put16(out, 16);        // bits per sample
    put_tag(out, "data");
    put32(out, data_size);
    for (float s : audio.samples) {
        const double clamped = std::clamp(static_cast<double>(s), -1.0, 1.0);
        const auto v = static_cast<std::int16_t>(std::lround(clamped * 32767.0));
        put16(out, static_cast<std::uint16_t>(v));
    }

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) throw std::runtime_error("cannot write " + path);
}

}  // namespace owfax::audio
