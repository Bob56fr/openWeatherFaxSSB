// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "alsa_input.hpp"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace owfax::io {
namespace {

constexpr unsigned kLatencyUs = 400000;  // device buffer; read() blocks <= ~1/4 of it

[[noreturn]] void fail(const std::string& what, int err) {
    throw std::runtime_error("ALSA: " + what + ": " + snd_strerror(err));
}

class AlsaSource final : public audio::AudioSource {
public:
    AlsaSource(const std::string& device, int rate) : device_(device), rate_(rate) {
        if (rate <= 0) throw std::invalid_argument("ALSA: invalid sample rate");
        int err = snd_pcm_open(&pcm_, device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
        if (err < 0) fail("cannot open '" + device + "'", err);
        // Mono first; "hw:" devices often only offer stereo, so fall back to 2 channels
        // averaged (prefer "plughw:", which converts for us).
        for (unsigned ch : {1U, 2U}) {
            err = snd_pcm_set_params(pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, ch,
                                     static_cast<unsigned>(rate), 1, kLatencyUs);
            if (err == 0) {
                channels_ = ch;
                break;
            }
        }
        if (err < 0) {
            snd_pcm_close(pcm_);
            fail("cannot configure '" + device + "' (S16_LE, " + std::to_string(rate) + " Hz)",
                 err);
        }
        snd_pcm_uframes_t buffer = 0;
        snd_pcm_uframes_t period = 0;
        if (snd_pcm_get_params(pcm_, &buffer, &period) == 0 && period > 0) {
            period_ = static_cast<std::size_t>(period);
        }
        err = snd_pcm_start(pcm_);
        if (err < 0) {
            snd_pcm_close(pcm_);
            fail("cannot start '" + device + "'", err);
        }
    }

    ~AlsaSource() override {
        if (pcm_ != nullptr) snd_pcm_close(pcm_);
    }

    AlsaSource(const AlsaSource&) = delete;
    AlsaSource& operator=(const AlsaSource&) = delete;

    int sample_rate() const override { return rate_; }

    std::size_t read(float* out, std::size_t max) override {
        if (max == 0) return 0;
        const std::size_t frames = std::min(max, period_);
        raw_.resize(frames * channels_);
        while (!stop_.load()) {
            const snd_pcm_sframes_t got =
                snd_pcm_readi(pcm_, raw_.data(), static_cast<snd_pcm_uframes_t>(frames));
            if (got > 0) {
                const auto n = static_cast<std::size_t>(got);
                for (std::size_t i = 0; i < n; ++i) {
                    float acc = 0.0F;
                    for (std::size_t c = 0; c < channels_; ++c) {
                        acc += static_cast<float>(raw_[i * channels_ + c]);
                    }
                    out[i] = acc / (32768.0F * static_cast<float>(channels_));
                }
                return n;
            }
            if (got == -EAGAIN || got == 0) continue;
            if (got == -EPIPE || got == -ESTRPIPE) ++overruns_;
            const int err = snd_pcm_recover(pcm_, static_cast<int>(got), 1);
            if (err < 0) fail("capture error on '" + device_ + "'", err);
            snd_pcm_start(pcm_);
        }
        return 0;
    }

    void interrupt() override { stop_.store(true); }

    std::string description() const override {
        return "ALSA " + device_ + " @ " + std::to_string(rate_) + " Hz" +
               (channels_ == 2 ? " (stereo averaged)" : "");
    }

    bool is_live() const override { return true; }

    std::size_t overruns() const override { return overruns_.load(); }

private:
    std::string device_;
    int rate_;
    snd_pcm_t* pcm_ = nullptr;
    std::size_t channels_ = 1;
    std::size_t period_ = 1024;
    std::vector<std::int16_t> raw_;
    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> overruns_{0};
};

}  // namespace

std::unique_ptr<audio::AudioSource> open_alsa(const std::string& device, int sample_rate) {
    return std::make_unique<AlsaSource>(device, sample_rate);
}

std::vector<InputDevice> list_alsa() {
    std::vector<InputDevice> out;
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return out;
    for (void** h = hints; *h != nullptr; ++h) {
        char* name = snd_device_name_get_hint(*h, "NAME");
        char* desc = snd_device_name_get_hint(*h, "DESC");
        char* ioid = snd_device_name_get_hint(*h, "IOID");
        // IOID is null for devices that do both directions.
        const bool capture = ioid == nullptr || std::string(ioid) == "Input";
        if (name != nullptr && capture && std::string(name) != "null") {
            std::string d = desc != nullptr ? desc : "";
            for (auto& c : d) {
                if (c == '\n') c = ' ';
            }
            out.push_back({std::string("alsa:") + name, d});
        }
        std::free(name);
        std::free(desc);
        std::free(ioid);
    }
    snd_device_name_free_hint(hints);
    return out;
}

}  // namespace owfax::io
