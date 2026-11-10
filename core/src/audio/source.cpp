// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/audio/source.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace owfax::audio {
namespace {

class BufferSource final : public AudioSource {
public:
    BufferSource(AudioBuffer audio, std::string description, bool realtime)
        : audio_(std::move(audio)), description_(std::move(description)), realtime_(realtime) {
        if (audio_.sample_rate <= 0) throw std::invalid_argument("audio source: invalid rate");
    }

    int sample_rate() const override { return audio_.sample_rate; }

    std::size_t read(float* out, std::size_t max) override {
        if (stop_.load() || pos_ >= audio_.samples.size()) return 0;
        const std::size_t n = std::min(max, audio_.samples.size() - pos_);
        if (realtime_) {
            if (pos_ == 0) t0_ = std::chrono::steady_clock::now();
            const auto due = t0_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                       std::chrono::duration<double>(static_cast<double>(pos_ + n) /
                                                                     audio_.sample_rate));
            // Sleep in short steps so that interrupt() stays responsive.
            while (!stop_.load() && std::chrono::steady_clock::now() < due) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (stop_.load()) return 0;
        }
        std::copy_n(audio_.samples.begin() + static_cast<std::ptrdiff_t>(pos_), n, out);
        pos_ += n;
        return n;
    }

    void interrupt() override { stop_.store(true); }
    std::string description() const override { return description_; }
    bool is_live() const override { return realtime_; }

private:
    AudioBuffer audio_;
    std::string description_;
    bool realtime_;
    std::size_t pos_ = 0;
    std::chrono::steady_clock::time_point t0_;
    std::atomic<bool> stop_{false};
};

class Pcm16Source final : public AudioSource {
public:
    Pcm16Source(std::FILE* file, int rate, std::string description)
        : file_(file), rate_(rate), description_(std::move(description)) {
        if (file_ == nullptr) throw std::invalid_argument("audio source: no file");
        if (rate_ <= 0) throw std::invalid_argument("audio source: invalid rate");
    }

    int sample_rate() const override { return rate_; }

    std::size_t read(float* out, std::size_t max) override {
        if (stop_.load() || max == 0) return 0;
        raw_.resize(std::min<std::size_t>(max, 4096) * 2);
        // A pipe may return an odd byte count: keep the half sample for the next call.
        std::size_t have = carry_ ? 1 : 0;
        if (carry_) raw_[0] = carry_byte_;
        const std::size_t got = std::fread(raw_.data() + have, 1, raw_.size() - have, file_);
        have += got;
        if (got == 0) return 0;
        const std::size_t n = have / 2;
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned lo = raw_[2 * i];
            const unsigned hi = raw_[2 * i + 1];
            const auto v = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8U)));
            out[i] = static_cast<float>(v) / 32768.0F;
        }
        carry_ = (have % 2) != 0;
        if (carry_) carry_byte_ = raw_[have - 1];
        return n;
    }

    void interrupt() override { stop_.store(true); }
    std::string description() const override { return description_; }

private:
    std::FILE* file_;
    int rate_;
    std::string description_;
    std::vector<unsigned char> raw_;
    bool carry_ = false;
    unsigned char carry_byte_ = 0;
    std::atomic<bool> stop_{false};
};

}  // namespace

std::unique_ptr<AudioSource> make_buffer_source(AudioBuffer audio, std::string description,
                                                bool realtime) {
    return std::make_unique<BufferSource>(std::move(audio), std::move(description), realtime);
}

std::unique_ptr<AudioSource> make_pcm16_source(std::FILE* file, int sample_rate,
                                               std::string description) {
    return std::make_unique<Pcm16Source>(file, sample_rate, std::move(description));
}

}  // namespace owfax::audio
