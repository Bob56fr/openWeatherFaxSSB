// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/dsp/spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace owfax::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;

/// In-place iterative radix-2 FFT (size a power of two).
void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

double to_db(double power) {
    return power > 1e-20 ? 10.0 * std::log10(power) : -200.0;
}

}  // namespace

SpectrumAnalyzer::SpectrumAnalyzer(int sample_rate, std::size_t bands, double max_hz, double hop_s)
    : bands_(bands), max_hz_(max_hz), rate_(static_cast<double>(sample_rate)) {
    if (sample_rate <= 0 || bands == 0 || max_hz <= 0.0 || hop_s <= 0.0) {
        throw std::invalid_argument("SpectrumAnalyzer: invalid parameters");
    }
    max_hz_ = std::min(max_hz, rate_ / 2.0);
    // About 85 ms of audio per transform: ~12 Hz resolution at 48 kHz, 11 Hz at 11025.
    n_ = 256;
    while (static_cast<double>(n_) < rate_ * 0.085) n_ <<= 1;
    hop_ = std::max<std::size_t>(1, static_cast<std::size_t>(rate_ * hop_s));
    window_.resize(n_);
    for (std::size_t i = 0; i < n_; ++i) {
        window_[i] = static_cast<float>(
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n_)));
    }
    ring_.assign(n_, 0.0F);
}

std::optional<SpectrumFrame> SpectrumAnalyzer::push(const float* samples, std::size_t count) {
    pending_.insert(pending_.end(), samples, samples + count);
    std::size_t used = 0;
    std::optional<SpectrumFrame> frame;
    while (used < pending_.size() && !frame) {
        const float s = pending_[used++];
        ring_[filled_ % n_] = s;
        ++filled_;
        const double a = std::fabs(static_cast<double>(s));
        sum_sq_ += a * a;
        peak_ = std::max(peak_, a);
        if (a >= 0.999) ++clipped_;
        ++level_n_;
        if (++since_ < hop_ || filled_ < n_) continue;
        since_ = 0;

        // Oldest sample first.
        std::vector<std::complex<double>> buf(n_);
        for (std::size_t i = 0; i < n_; ++i) {
            buf[i] = static_cast<double>(ring_[(filled_ + i) % n_] * window_[i]);
        }
        fft(buf);
        // Hann window, one-sided: a full-scale sine reads 0 dB in its band.
        const double norm = 16.0 / (static_cast<double>(n_) * static_cast<double>(n_));
        const double bin_hz = rate_ / static_cast<double>(n_);
        SpectrumFrame f;
        f.max_hz = max_hz_;
        f.db.assign(bands_, -200.0F);
        for (std::size_t b = 0; b < bands_; ++b) {
            const double lo = max_hz_ * static_cast<double>(b) / static_cast<double>(bands_);
            const double hi = max_hz_ * static_cast<double>(b + 1) / static_cast<double>(bands_);
            auto k0 = static_cast<std::size_t>(std::floor(lo / bin_hz));
            auto k1 = static_cast<std::size_t>(std::ceil(hi / bin_hz));
            k1 = std::max(k1, k0 + 1);
            double best = 0.0;  // strongest FFT bin of the band (a tone stays visible)
            for (std::size_t k = k0; k < k1 && k <= n_ / 2; ++k) {
                best = std::max(best, std::norm(buf[k]) * norm);
            }
            f.db[b] = static_cast<float>(to_db(best));
        }
        f.rms_dbfs = to_db(2.0 * sum_sq_ / static_cast<double>(level_n_));  // sine = 0 dBFS
        f.peak_dbfs = peak_ > 0.0 ? 20.0 * std::log10(peak_) : -200.0;
        f.clipped = clipped_;
        sum_sq_ = 0.0;
        peak_ = 0.0;
        clipped_ = 0;
        level_n_ = 0;
        frame = std::move(f);
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
    return frame;
}

}  // namespace owfax::dsp
