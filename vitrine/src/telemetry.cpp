// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/vitrine/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace owfax::vitrine {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Noise reference bands: outside the WEFAX band and its FM sidebands, inside the
// passband of an SSB filter of 2.4 kHz or more.
constexpr double kNoiseBands[][2] = {{300.0, 900.0}, {2700.0, 2900.0}};
constexpr double kSnrBandwidthHz = 2500.0;  // same reference as the test modulator
constexpr float kSnrMin = -20.0F;
constexpr float kSnrMax = 60.0F;

/// In-place iterative radix-2 FFT (size a power of two). Same algorithm as the
/// diagnostic spectrum (core/src/dsp/spectrum.cpp), which keeps its own private copy.
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

/// Welch power spectrum of x[0..n): Hann windows of `fft_n` with 50 % overlap.
/// Returns the mean power per bin (arbitrary scale, same for every bin).
std::vector<double> welch(const float* x, std::size_t n, std::size_t fft_n,
                          const std::vector<double>& window) {
    std::vector<double> acc(fft_n / 2 + 1, 0.0);
    std::size_t frames = 0;
    std::vector<std::complex<double>> buf(fft_n);
    const std::size_t hop = fft_n / 2;
    for (std::size_t start = 0; start + fft_n <= n; start += hop) {
        for (std::size_t i = 0; i < fft_n; ++i) {
            buf[i] = static_cast<double>(x[start + i]) * window[i];
        }
        fft(buf);
        for (std::size_t k = 0; k < acc.size(); ++k) acc[k] += std::norm(buf[k]);
        ++frames;
    }
    if (frames > 0) {
        for (double& v : acc) v /= static_cast<double>(frames);
    }
    return acc;
}

double band_sum(const std::vector<double>& p, double bin_hz, double lo, double hi,
                std::size_t& bins) {
    const auto k0 = static_cast<std::size_t>(std::ceil(lo / bin_hz));
    const auto k1 = std::min(p.size() - 1, static_cast<std::size_t>(std::floor(hi / bin_hz)));
    double s = 0.0;
    bins = 0;
    for (std::size_t k = k0; k <= k1 && k < p.size(); ++k) {
        s += p[k];
        ++bins;
    }
    return s;
}

float snr_of(const std::vector<double>& p, double bin_hz, const SnrBands& bands) {
    // Noise power per bin: median of the reference bins rather than their mean, so that
    // the FM sidebands of a strong signal spilling into part of them do not cap the SNR.
    std::vector<double> noise;
    for (const auto& nb : kNoiseBands) {
        const auto k0 = static_cast<std::size_t>(std::ceil(nb[0] / bin_hz));
        const auto k1 = static_cast<std::size_t>(std::floor(nb[1] / bin_hz));
        for (std::size_t k = k0; k <= k1 && k < p.size(); ++k) noise.push_back(p[k]);
    }
    std::size_t sig_bins = 0;
    const double total = band_sum(p, bin_hz, bands.signal_lo_hz, bands.signal_hi_hz, sig_bins);
    if (noise.empty() || sig_bins == 0) return kSnrMin;
    const auto mid = noise.begin() + static_cast<std::ptrdiff_t>(noise.size() / 2);
    std::nth_element(noise.begin(), mid, noise.end());
    const double n0 = *mid;
    const double signal = total - n0 * static_cast<double>(sig_bins);
    const double noise_ref = n0 * (kSnrBandwidthHz / bin_hz);
    if (signal <= 0.0 || noise_ref <= 0.0) return signal > 0.0 ? kSnrMax : kSnrMin;
    const double db = 10.0 * std::log10(signal / noise_ref);
    return std::clamp(static_cast<float>(db), kSnrMin, kSnrMax);
}

bool parse_hms(const std::string& s, int& seconds) {
    int h = 0, m = 0, sec = 0;
    char c1 = 0, c2 = 0;
    if (std::sscanf(s.c_str(), "%d%c%d%c%d", &h, &c1, &m, &c2, &sec) != 5) return false;
    if (c1 != ':' || c2 != ':' || h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 60) {
        return false;
    }
    seconds = h * 3600 + m * 60 + sec;
    return true;
}

}  // namespace

std::vector<SmeterReading> read_smeter_csv(const std::string& path, std::int64_t start_unix) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<SmeterReading> out;
    const std::int64_t day0 = (start_unix >= 0 ? start_unix : start_unix - 86399) / 86400 * 86400;
    std::int64_t day = day0;
    std::string line;
    while (std::getline(in, line)) {
        const auto comma = line.find(',');
        if (comma == std::string::npos) continue;
        int tod = 0;
        if (!parse_hms(line.substr(0, comma), tod)) continue;  // header or malformed
        char* end = nullptr;
        const std::string value = line.substr(comma + 1);
        const double db = std::strtod(value.c_str(), &end);
        if (end == value.c_str()) continue;
        std::int64_t t = day + tod;
        // Before the capture started (by more than an hour): the clock went past midnight.
        if (t < start_unix - 3600) t += 86400;
        if (!out.empty() && t < out.back().unix_time - 3600) t += 86400;
        day = t - tod;
        out.push_back({t, db});
    }
    return out;
}

Telemetry measure(const audio::AudioBuffer& audio, const SnrBands& bands,
                  const std::vector<SmeterReading>& smeter,
                  std::optional<double> audio_start_unix) {
    if (audio.sample_rate <= 0) throw std::invalid_argument("measure: no sample rate");
    const auto rate = static_cast<std::size_t>(audio.sample_rate);
    // ~0.19 s per transform: 5.4 Hz bins at 11025 Hz, 5.9 Hz at 48 kHz; 9 or more
    // overlapping windows per second.
    std::size_t fft_n = 256;
    while (fft_n < rate / 6) fft_n <<= 1;
    std::vector<double> window(fft_n);
    for (std::size_t i = 0; i < fft_n; ++i) {
        window[i] =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(fft_n));
    }
    const double bin_hz = static_cast<double>(rate) / static_cast<double>(fft_n);

    Telemetry t;
    const std::size_t seconds = (audio.samples.size() + rate - 1) / rate;
    t.audio_dbfs.reserve(seconds);
    t.snr_db.reserve(seconds);
    for (std::size_t s = 0; s < seconds; ++s) {
        const std::size_t a = s * rate;
        const std::size_t n = std::min(rate, audio.samples.size() - a);
        const float* x = audio.samples.data() + a;
        double sq = 0.0;
        for (std::size_t i = 0; i < n; ++i) sq += static_cast<double>(x[i]) * x[i];
        const double ms =
            2.0 * sq / static_cast<double>(std::max<std::size_t>(n, 1));  // sine = 0 dB
        t.audio_dbfs.push_back(ms > 1e-20 ? static_cast<float>(10.0 * std::log10(ms)) : -200.0F);
        // The last, partial second reuses the previous SNR rather than a noisy estimate.
        if (n < fft_n && !t.snr_db.empty()) {
            t.snr_db.push_back(t.snr_db.back());
        } else {
            t.snr_db.push_back(snr_of(welch(x, n, fft_n, window), bin_hz, bands));
        }
    }

    if (!smeter.empty() && audio_start_unix) {
        t.smeter_db_s9.reserve(seconds);
        std::size_t j = 0;
        for (std::size_t s = 0; s < seconds; ++s) {
            const double when = *audio_start_unix + static_cast<double>(s) + 0.5;
            while (j + 1 < smeter.size() && static_cast<double>(smeter[j + 1].unix_time) <= when)
                ++j;
            double v = smeter[j].db_s9;
            if (j + 1 < smeter.size() && static_cast<double>(smeter[j].unix_time) <= when) {
                const double t0 = static_cast<double>(smeter[j].unix_time);
                const double t1 = static_cast<double>(smeter[j + 1].unix_time);
                const double f = t1 > t0 ? (when - t0) / (t1 - t0) : 0.0;
                v += (smeter[j + 1].db_s9 - smeter[j].db_s9) * std::clamp(f, 0.0, 1.0);
            }
            t.smeter_db_s9.push_back(static_cast<float>(v));
        }
    }
    return t;
}

}  // namespace owfax::vitrine
