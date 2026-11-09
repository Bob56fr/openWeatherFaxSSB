// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/wefax/modulator.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

#include "owfax/dsp/fir.hpp"

namespace owfax::wefax {
namespace {

constexpr double kAmplitude = 0.5;
constexpr double kNoiseBandwidthHz = 2500.0;

double square(double t, double tone_hz) {
    return std::fmod(t * tone_hz, 1.0) < 0.5 ? 1.0 : 0.0;
}

}  // namespace

audio::AudioBuffer modulate(const GrayImage& image, const ModulatorOptions& o) {
    if (o.sample_rate <= 0 || image.width == 0 || image.height == 0) {
        throw std::invalid_argument("modulate: invalid image or sample rate");
    }
    const double rate = o.sample_rate;
    // A fast sound card clock records more samples per real second.
    const double dt = 1.0 / (rate * (1.0 + o.clock_ppm * 1e-6));
    const double line_s = line_duration_s(o.lpm);
    const double t_phasing = o.start_tone_s;
    const double t_image = t_phasing + o.phasing_s;
    const double t_stop = t_image + static_cast<double>(image.height) * line_s;
    const double t_trailer = t_stop + o.stop_tone_s;
    const double t_end = t_trailer + o.trailer_s;

    audio::AudioBuffer out;
    out.sample_rate = o.sample_rate;
    out.samples.resize(static_cast<std::size_t>(t_end / dt));

    double sigma = 0.0;
    if (o.snr_db) {
        const double signal_power = kAmplitude * kAmplitude / 2.0;
        const double noise_in_band = signal_power / std::pow(10.0, *o.snr_db / 10.0);
        sigma = std::sqrt(noise_in_band * (rate / 2.0) / kNoiseBandwidthHz);
    }
    std::mt19937 rng(o.noise_seed);
    std::normal_distribution<double> gauss(0.0, sigma > 0.0 ? sigma : 1.0);

    const double width = static_cast<double>(image.width);
    double phase = 0.0;
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    int burst_left = 0;
    for (std::size_t i = 0; i < out.samples.size(); ++i) {
        const double t = static_cast<double>(i) * dt;
        double lum = 0.0;
        if (t < t_phasing) {
            lum = square(t, apt_start_hz(o.ioc));
        } else if (t < t_image) {
            const double frac = std::fmod(t - t_phasing, line_s) / line_s;
            lum = (frac < 0.025 || frac >= 0.975) ? 1.0 : 0.0;
        } else if (t < t_stop) {
            const double ti = t - t_image;
            const auto line = std::min(image.height - 1, static_cast<std::size_t>(ti / line_s));
            const double frac = (ti - static_cast<double>(line) * line_s) / line_s;
            const auto x = std::min(image.width - 1, static_cast<std::size_t>(frac * width));
            lum = image.at(x, line) / 255.0;
        } else if (t < t_trailer) {
            lum = square(t - t_stop, kAptStopHz);
        }
        const double f = kCenterHz + (lum - 0.5) * o.shift_hz + o.tuning_offset_hz;
        phase += dsp::kTwoPi * f * dt;
        if (phase >= dsp::kTwoPi) phase -= dsp::kTwoPi;
        double s = kAmplitude * std::sin(phase);
        if (sigma > 0.0) s += gauss(rng);
        if (o.impulses_per_s > 0.0) {
            if (burst_left == 0 && uniform(rng) < o.impulses_per_s * dt) {
                burst_left = 1 + static_cast<int>(uniform(rng) * 0.002 * rate);  // up to 2 ms
            }
            if (burst_left > 0) {
                --burst_left;
                s += o.impulse_amplitude * (2.0 * uniform(rng) - 1.0);
            }
        }
        out.samples[i] = static_cast<float>(s);
    }
    return out;
}

GrayImage make_test_chart(Ioc ioc, std::size_t lines) {
    const auto w = static_cast<std::size_t>(pixels_per_line(ioc));
    const std::size_t h = lines;
    GrayImage img(w, h, 255);
    auto fill = [&](std::size_t x0, std::size_t y0, std::size_t x1, std::size_t y1,
                    std::uint8_t v) {
        for (std::size_t y = y0; y < std::min(y1, h); ++y) {
            for (std::size_t x = x0; x < std::min(x1, w); ++x) img.at(x, y) = v;
        }
    };
    const std::size_t mx = w * 3 / 100;
    const std::size_t my = std::max<std::size_t>(1, h * 3 / 100);

    // Frame.
    fill(mx, my, w - mx, my + 4, 0);
    fill(mx, h - my - 4, w - mx, h - my, 0);
    fill(mx, my, mx + 4, h - my, 0);
    fill(w - mx - 4, my, w - mx, h - my, 0);
    // Grid (like latitude/longitude lines).
    for (std::size_t k = 1; k < 8; ++k) {
        const std::size_t x = mx + (w - 2 * mx) * k / 8;
        fill(x, my, x + 2, h - my, 0);
    }
    for (std::size_t k = 1; k < 6; ++k) {
        const std::size_t y = my + (h - 2 * my) * k / 6;
        fill(mx, y, w - mx, y + 2, 0);
    }
    // Diagonal (like an isobar or a front).
    for (std::size_t y = my; y < h - my; ++y) {
        const std::size_t x = mx + (w - 2 * mx) * (y - my) / std::max<std::size_t>(1, h - 2 * my);
        fill(x, y, x + 3, y + 1, 0);
    }
    // Checkerboard (like text).
    for (std::size_t y = h * 3 / 4; y < h * 9 / 10; ++y) {
        for (std::size_t x = w * 6 / 10; x < w * 8 / 10; ++x) {
            img.at(x, y) = ((x / 8 + y / 4) % 2 == 0) ? 0 : 255;
        }
    }
    // Gray ramp (level linearity).
    const std::size_t rx0 = w / 10;
    const std::size_t rx1 = w / 2;
    for (std::size_t y = h * 8 / 10; y < h * 9 / 10; ++y) {
        for (std::size_t x = rx0; x < rx1; ++x) {
            img.at(x, y) = static_cast<std::uint8_t>(255 * (x - rx0) / (rx1 - rx0));
        }
    }
    return img;
}

}  // namespace owfax::wefax
