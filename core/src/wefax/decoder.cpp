// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Pipeline:
//  1. FM discriminator → instantaneous frequency at ~8 kHz                 (DSP-001)
//  2. APT start (300/675 Hz) and stop (450 Hz) tone detection               (DSP-002)
//  3. Black/white levels from the frequency histogram → tuning offset       (DSP-004)
//  4. Line period and origin:                                               (DSP-003/005)
//     a. coarse clock error by folding the phasing window,
//     b. phasing pulse tracked line by line + linear regression,
//     c. refinement on long images by folding at one-sample resolution.
//  5. Rendering with sub-sample interpolation + per-line quality            (DSP-008)

#include "owfax/wefax/decoder.hpp"

#include "owfax/wefax/stream_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "owfax/dsp/fir.hpp"
#include "owfax/dsp/fm_demod.hpp"

namespace owfax::wefax {
namespace {

constexpr double kWorkingRate = 8000.0;
constexpr double kDemodCutoffHz = 1200.0;
constexpr double kToneBlockS = 0.25;
constexpr double kToneThreshold = 0.3;
constexpr std::size_t kStartToneMinBlocks = 8;  // 2 s
constexpr std::size_t kStopToneMinBlocks = 6;   // 1.5 s
constexpr double kPhasingWindowS = 15.0;
constexpr double kMaxPhasingS = 60.0;
constexpr double kPhasingPulseFraction = 0.05;
constexpr double kMinPulseContrast = 0.3;
constexpr std::size_t kCoarseBins = 400;
constexpr std::size_t kMinPhasingLinesForFit = 8;
constexpr std::size_t kRefineMinLines = 300;
constexpr std::size_t kRefineMaxLines = 800;
constexpr double kMinContentStd = 12.0;        // pixel std (0-255) of a textured line
constexpr double kMinTexturedFraction = 0.10;  // below this share of textured lines: blank

struct Run {
    std::size_t begin;  // first sample
    std::size_t end;    // one past the last sample
};

double to_d(std::size_t v) {
    return static_cast<double>(v);
}

float clamp01(float v) {
    return v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
}

/// First run of `tone_hz` holding at least `min_blocks` detected 0.25 s blocks.
/// A single missed block inside the run is tolerated: on real HF signals (GYA,
/// 2026-11-09) QRM chops the APT tone into ~1 s pieces separated by 0.25 s gaps.
std::optional<Run> find_tone_run(const std::vector<float>& sig, double rate, double tone_hz,
                                 std::size_t from, std::size_t min_blocks) {
    constexpr std::size_t kMaxGapBlocks = 1;
    const auto block = static_cast<std::size_t>(rate * kToneBlockS);
    if (block == 0) return std::nullopt;
    std::size_t run_start = 0;
    std::size_t run_end = 0;  // end of the last detected block
    std::size_t hits = 0;
    std::size_t gap = 0;
    for (std::size_t pos = from; pos + block <= sig.size(); pos += block) {
        if (dsp::tone_ratio(&sig[pos], block, tone_hz, rate) > kToneThreshold) {
            if (hits == 0) run_start = pos;
            ++hits;
            gap = 0;
            run_end = pos + block;
        } else if (hits > 0 && ++gap > kMaxGapBlocks) {
            if (hits >= min_blocks) return Run{run_start, run_end};
            hits = 0;
            gap = 0;
        }
    }
    if (hits >= min_blocks) return Run{run_start, run_end};
    return std::nullopt;
}

struct Levels {
    double black = kBlackHz;
    double white = kWhiteHz;
};

/// Black and white frequencies = the two main peaks of the frequency histogram over
/// phasing + image (both levels are abundant there; peaks are robust to noise).
Levels estimate_levels(const std::vector<float>& sig, std::size_t from, std::size_t to,
                       double shift_hz) {
    constexpr double kLo = 1000.0;
    constexpr double kBinHz = 10.0;
    constexpr std::size_t kBins = 180;          // 1000 … 2800 Hz
    constexpr std::size_t kMinSeparation = 40;  // 400 Hz
    std::vector<double> hist(kBins, 0.0);
    const std::size_t stride = std::max<std::size_t>(1, (to - from) / 400000);
    for (std::size_t i = from; i < to; i += stride) {
        const double f = static_cast<double>(sig[i]);
        if (f < kLo) continue;
        const auto b = static_cast<std::size_t>((f - kLo) / kBinHz);
        if (b < kBins) hist[b] += 1.0;
    }
    std::vector<double> h(kBins, 0.0);  // 5-bin box smoothing
    for (std::size_t b = 2; b + 2 < kBins; ++b) {
        h[b] = hist[b - 2] + hist[b - 1] + hist[b] + hist[b + 1] + hist[b + 2];
    }
    auto argmax = [&](auto accept) {
        std::size_t best = 0;
        for (std::size_t b = 1; b + 1 < kBins; ++b) {
            if (accept(b) && h[b] > h[best]) best = b;
        }
        return best;
    };
    const std::size_t p1 = argmax([](std::size_t) { return true; });
    const std::size_t p2 =
        argmax([&](std::size_t b) { return (b > p1 ? b - p1 : p1 - b) >= kMinSeparation; });
    Levels lv;
    if (h[p1] <= 0.0 || p1 == 0) return lv;

    auto refine = [&](std::size_t b) {
        const double den = h[b - 1] - 2.0 * h[b] + h[b + 1];
        const double delta = den != 0.0 ? 0.5 * (h[b - 1] - h[b + 1]) / den : 0.0;
        return kLo + (to_d(b) + 0.5 + std::clamp(delta, -0.5, 0.5)) * kBinHz;
    };
    if (h[p2] < 0.03 * h[p1] || p2 == 0) {
        // Only one clear level (e.g. the white end of a chart, no phasing): deduce the
        // other one from the station's nominal shift, if the peak is plausibly black or white.
        const double f = refine(p1);
        if (std::fabs(f - (kCenterHz + shift_hz / 2.0)) < 250.0) {
            lv.white = f;
            lv.black = f - shift_hz;
        } else if (std::fabs(f - (kCenterHz - shift_hz / 2.0)) < 250.0) {
            lv.black = f;
            lv.white = f + shift_hz;
        }
        return lv;
    }
    const double a = refine(p1);
    const double b = refine(p2);
    const double black = std::min(a, b);
    const double white = std::max(a, b);
    if (white - black < 600.0 || white - black > 1000.0) return lv;  // implausible
    lv.black = black;
    lv.white = white;
    return lv;
}

/// Average luminance profile of lum[from, to) folded with the given period.
std::vector<double> fold(const std::vector<float>& lum, std::size_t from, std::size_t to,
                         double period, std::size_t bins) {
    std::vector<double> sum(bins, 0.0);
    std::vector<std::size_t> count(bins, 0);
    const double step = to_d(bins) / period;
    double phase = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        auto b = static_cast<std::size_t>(phase);
        if (b >= bins) b = bins - 1;
        sum[b] += static_cast<double>(clamp01(lum[i]));
        ++count[b];
        phase += step;
        if (phase >= to_d(bins)) phase -= to_d(bins);
    }
    for (std::size_t b = 0; b < bins; ++b) sum[b] = count[b] > 0 ? sum[b] / to_d(count[b]) : 0.0;
    return sum;
}

double variance(const std::vector<double>& p) {
    double mean = 0.0;
    for (double v : p) mean += v;
    mean /= to_d(p.size());
    double var = 0.0;
    for (double v : p) var += (v - mean) * (v - mean);
    return var / to_d(p.size());
}

/// Clock error maximising the sharpness (variance) of the folded profile.
/// `bins` = 0 folds at one-sample resolution.
double best_ppm(const std::vector<float>& lum, std::size_t from, std::size_t to, double nominal,
                double center, double half_range, double step, std::size_t bins) {
    if (bins == 0) bins = static_cast<std::size_t>(nominal);
    double best = center;
    double best_score = -1.0;
    const auto steps = static_cast<int>(std::round(2.0 * half_range / step));
    for (int k = 0; k <= steps; ++k) {
        const double ppm = center - half_range + k * step;
        const double score = variance(fold(lum, from, to, nominal * (1.0 + ppm * 1e-6), bins));
        if (score > best_score) {
            best_score = score;
            best = ppm;
        }
    }
    return best;
}

struct Pulse {
    double center_fraction = 0.0;  // position of the pulse centre within the folded line
    double contrast = 0.0;         // mean(pulse) − mean(rest), signed
};

Pulse find_pulse(const std::vector<double>& profile) {
    const std::size_t n = profile.size();
    const std::size_t w =
        std::max<std::size_t>(1, static_cast<std::size_t>(to_d(n) * kPhasingPulseFraction));
    double total = 0.0;
    for (double v : profile) total += v;
    double win = 0.0;
    for (std::size_t i = 0; i < w; ++i) win += profile[i];

    Pulse best;
    std::size_t best_start = 0;
    for (std::size_t s = 0; s < n; ++s) {
        const double contrast = win / to_d(w) - (total - win) / to_d(n - w);
        if (std::fabs(contrast) > std::fabs(best.contrast)) {
            best.contrast = contrast;
            best_start = s;
        }
        win += profile[(s + w) % n] - profile[s];
    }
    double center = (to_d(best_start) + to_d(w) / 2.0) / to_d(n);
    if (center >= 1.0) center -= 1.0;
    best.center_fraction = center;
    return best;
}

struct LineFit {
    double origin;      // sample position of the centre of phasing line 0
    double period;      // samples per line
    std::size_t lines;  // phasing lines (index of the first image line)
};

/// Follows the phasing pulse line by line and fits centre(k) = origin + k × period.
std::optional<LineFit> track_phasing(const std::vector<float>& lum, double origin, double period,
                                     bool white_pulse, double limit) {
    const double half_window = 0.08 * period;
    const double pulse_len = kPhasingPulseFraction * period;
    std::vector<double> ks;
    std::vector<double> centers;
    std::size_t last_hit = 0;
    int misses = 0;
    for (std::size_t k = 0;; ++k) {
        const double expected = origin + to_d(k) * period;
        if (expected + 0.6 * period >= limit) break;
        const double a = std::max(0.0, expected - half_window);
        const auto ia = static_cast<std::size_t>(a);
        const auto ib = std::min(lum.size(), static_cast<std::size_t>(expected + half_window));
        double mass = 0.0;
        double moment = 0.0;
        for (std::size_t i = ia; i < ib; ++i) {
            const float v = clamp01(lum[i]);
            const double pulse_ness = static_cast<double>(white_pulse ? v : 1.0F - v);
            const double weight = std::max(0.0, pulse_ness - 0.5);
            mass += weight;
            moment += weight * to_d(i);
        }
        // The middle of a phasing line is uniform background.
        double mid = 0.0;
        const auto ma = static_cast<std::size_t>(std::max(0.0, expected + 0.2 * period));
        const auto mb = std::min(lum.size(), static_cast<std::size_t>(expected + 0.8 * period));
        for (std::size_t i = ma; i < mb; ++i) {
            const float v = clamp01(lum[i]);
            mid += static_cast<double>(white_pulse ? v : 1.0F - v);
        }
        mid = mb > ma ? mid / to_d(mb - ma) : 1.0;

        const double expected_mass = 0.5 * pulse_len;
        const bool hit = mass > 0.4 * expected_mass && mass < 1.5 * expected_mass && mid < 0.3;
        if (hit) {
            ks.push_back(to_d(k));
            centers.push_back(moment / mass);
            last_hit = k;
            misses = 0;
        } else if (!ks.empty() && ++misses >= 2) {
            break;
        }
    }
    // Robust least squares: fit, drop outliers (e.g. a window still holding the end of
    // the APT tone, or a noise burst), refit.
    std::vector<bool> keep(ks.size(), true);
    double slope = period;
    double intercept = origin;
    for (int pass = 0; pass < 3; ++pass) {
        double n = 0.0, mk = 0.0, mc = 0.0;
        for (std::size_t i = 0; i < ks.size(); ++i) {
            if (!keep[i]) continue;
            n += 1.0;
            mk += ks[i];
            mc += centers[i];
        }
        if (n < to_d(kMinPhasingLinesForFit)) return std::nullopt;
        mk /= n;
        mc /= n;
        double skc = 0.0;
        double skk = 0.0;
        for (std::size_t i = 0; i < ks.size(); ++i) {
            if (!keep[i]) continue;
            skc += (ks[i] - mk) * (centers[i] - mc);
            skk += (ks[i] - mk) * (ks[i] - mk);
        }
        if (skk <= 0.0) return std::nullopt;
        slope = skc / skk;
        intercept = mc - slope * mk;

        std::vector<double> abs_res;
        for (std::size_t i = 0; i < ks.size(); ++i) {
            if (keep[i]) abs_res.push_back(std::fabs(centers[i] - (intercept + slope * ks[i])));
        }
        auto mid_it = abs_res.begin() + static_cast<std::ptrdiff_t>(abs_res.size() / 2);
        std::nth_element(abs_res.begin(), mid_it, abs_res.end());
        const double tolerance = std::max(2.0, 4.0 * *mid_it);
        for (std::size_t i = 0; i < ks.size(); ++i) {
            keep[i] = std::fabs(centers[i] - (intercept + slope * ks[i])) <= tolerance;
        }
    }
    return LineFit{intercept, slope, last_hit + 1};
}

double sample_at(const std::vector<float>& v, double x) {
    if (x <= 0.0) return static_cast<double>(clamp01(v.front()));
    const auto i = static_cast<std::size_t>(x);
    if (i + 1 >= v.size()) return static_cast<double>(clamp01(v.back()));
    const double t = x - to_d(i);
    const double raw = static_cast<double>(v[i]) * (1.0 - t) + static_cast<double>(v[i + 1]) * t;
    return std::clamp(raw, 0.0, 1.0);
}

/// FM click suppression (DSP-020), on the instantaneous frequency (Hz).
/// Real receptions (DWD 22:00 UTC, 2026-11-09): striped lines carry ~5x more
/// out-of-range samples than clean ones. Two kinds of samples are repaired:
///  - far outside [black − margin, white + margin]: certainly noise, replaced by the
///    median of the nearby in-range samples;
///  - single-sample spikes (differing from both neighbours by more than half the
///    black–white span in the same direction): replaced by the 3-sample median.
/// A 1-pixel line spans ~2 samples at 8 kHz and is therefore never touched.
std::size_t suppress_clicks(std::vector<float>& f, double black, double white) {
    const double span = white - black;
    const auto lo = static_cast<float>(black - 0.25 * span);
    const auto hi = static_cast<float>(white + 0.25 * span);
    const auto spike = static_cast<float>(0.5 * span);
    const std::vector<float> src(f);
    const std::size_t n = src.size();
    std::size_t repaired = 0;
    float window[8];
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const float v = src[i];
        if (v < lo || v > hi) {
            std::size_t k = 0;
            for (std::size_t j = (i >= 4 ? i - 4 : 0); j <= std::min(n - 1, i + 4); ++j) {
                if (j != i && src[j] >= lo && src[j] <= hi && k < 8) window[k++] = src[j];
            }
            if (k > 0) {
                std::nth_element(window, window + k / 2, window + k);
                f[i] = window[k / 2];
            } else {
                f[i] = std::clamp(v, lo, hi);
            }
            ++repaired;
            continue;
        }
        const float a = src[i - 1];
        const float b = src[i + 1];
        if ((v - a > spike && v - b > spike) || (a - v > spike && b - v > spike)) {
            f[i] = std::max(std::min(a, b), std::min(std::max(a, b), v));  // median of 3
            ++repaired;
        }
    }
    return repaired;
}

/// Renders one line of luminance samples starting at sample position `start`.
void render_line(const std::vector<float>& lum, double start, double period, std::size_t width,
                 std::uint8_t* out) {
    const double px = period / to_d(width);
    for (std::size_t x = 0; x < width; ++x) {
        const double c = start + (to_d(x) + 0.5) * px;
        const double v =
            (sample_at(lum, c - 0.25 * px) + sample_at(lum, c) + sample_at(lum, c + 0.25 * px)) /
            3.0;
        out[x] = static_cast<std::uint8_t>(std::lround(v * 255.0));
    }
}

/// Degraded-pixel repair (DSP-021). Real receptions (DWD 22:00 and GYA, 2026-11-09):
/// the stripes are noisy *segments* of lines (fading dips, QRM), not whole lost lines,
/// and real horizontal chart lines must never be touched. So the decision is taken on
/// the received signal, pixel by pixel: a pixel is degraded when many of the
/// demodulator samples under it fell outside the black-white range (FM clicks).
/// Degraded pixels with a clean pixel above and below within kMaxGap rows are replaced
/// by vertical interpolation; anything else stays as received (no invention, DSP-030).
/// Returns the indices of the lines that were modified.
std::vector<std::size_t> repair_pixels(GrayImage& img, const std::vector<std::uint8_t>& ok,
                                       double first_line, double period) {
    constexpr double kBadFraction = 0.25;  // share of out-of-range samples under a pixel
    constexpr std::size_t kMaxGap = 2;     // rows that may be bridged vertically
    const std::size_t w = img.width;
    const std::size_t h = img.height;
    std::vector<std::size_t> lines;
    if (w == 0 || h < 3) return lines;
    const double px = period / to_d(w);
    std::vector<std::uint8_t> bad(w * h, 0);
    for (std::size_t y = 0; y < h; ++y) {
        const double s = first_line + to_d(y) * period;
        for (std::size_t x = 0; x < w; ++x) {
            // Window of +-1.5 pixels around the pixel centre.
            const double c = s + (to_d(x) + 0.5) * px;
            const auto a = static_cast<std::size_t>(std::max(0.0, c - 1.5 * px));
            const auto b =
                std::min(ok.size(), static_cast<std::size_t>(std::max(0.0, c + 1.5 * px)) + 1);
            std::size_t n = 0;
            std::size_t out = 0;
            for (std::size_t i = a; i < b; ++i, ++n) out += ok[i] ? 0U : 1U;
            if (n > 0 && to_d(out) > kBadFraction * to_d(n)) bad[y * w + x] = 1;
        }
    }
    std::vector<std::uint8_t> touched(h, 0);
    for (std::size_t x = 0; x < w; ++x) {
        std::size_t y = 1;
        while (y + 1 < h) {
            if (!bad[y * w + x]) {
                ++y;
                continue;
            }
            std::size_t e = y;
            while (e < h && bad[e * w + x]) ++e;
            const std::size_t run = e - y;
            if (run <= kMaxGap && e < h && !bad[(y - 1) * w + x]) {
                const double above = img.at(x, y - 1);
                const double below = img.at(x, e);
                for (std::size_t k = 0; k < run; ++k) {
                    const double t = to_d(k + 1) / to_d(run + 1);
                    img.at(x, y + k) =
                        static_cast<std::uint8_t>(std::lround((1.0 - t) * above + t * below));
                    touched[y + k] = 1;
                }
            }
            y = e + 1;
        }
    }
    for (std::size_t y = 0; y < h; ++y) {
        if (touched[y]) lines.push_back(y);
    }
    return lines;
}

double ppm_of(double period, double nominal) {
    return (period / nominal - 1.0) * 1e6;
}

}  // namespace

dsp::FmDemodulator make_demodulator(double in_rate) {
    const auto decim = std::max<std::size_t>(1, static_cast<std::size_t>(in_rate / kWorkingRate));
    return dsp::FmDemodulator(in_rate, kCenterHz, kDemodCutoffHz, decim);
}

DecodeResult decode(const audio::AudioBuffer& audio, const DecodeOptions& opt) {
    if (audio.sample_rate <= 0) throw std::invalid_argument("decode: invalid sample rate");

    // 1. Instantaneous frequency.
    auto demod = make_demodulator(static_cast<double>(audio.sample_rate));
    std::vector<float> sig;
    sig.reserve(static_cast<std::size_t>(to_d(audio.samples.size()) * demod.output_rate() /
                                         static_cast<double>(audio.sample_rate)) +
                1);
    demod.process(audio.samples.data(), audio.samples.size(), sig);
    return decode_frequency(std::move(sig), demod.output_rate(), opt);
}

DecodeResult decode_frequency(std::vector<float> sig, double rate, const DecodeOptions& opt) {
    if (rate <= 0.0) throw std::invalid_argument("decode: invalid rate");
    const double nominal = rate * line_duration_s(opt.lpm);
    if (to_d(sig.size()) < 10.0 * nominal) throw std::runtime_error("decode: recording too short");

    DecodeResult result;
    DecodeReport& rep = result.report;

    // 2. APT tones.
    std::optional<Run> start;
    if (opt.ioc) {
        rep.ioc = *opt.ioc;
        start = find_tone_run(sig, rate, apt_start_hz(*opt.ioc), 0, kStartToneMinBlocks);
    } else {
        const auto r576 = find_tone_run(sig, rate, apt_start_hz(Ioc::k576), 0, kStartToneMinBlocks);
        const auto r288 = find_tone_run(sig, rate, apt_start_hz(Ioc::k288), 0, kStartToneMinBlocks);
        if (r576 && (!r288 || r576->begin <= r288->begin)) {
            start = r576;
        } else if (r288) {
            start = r288;
            rep.ioc = Ioc::k288;
        }
    }
    rep.apt_start_found = start.has_value();
    const std::size_t after_start = start ? start->end : 0;
    const auto stop = find_tone_run(sig, rate, kAptStopHz, after_start, kStopToneMinBlocks);
    rep.apt_stop_found = stop.has_value();
    const std::size_t signal_end = stop ? stop->begin : sig.size();

    // 3. Levels and tuning offset, then frequency → luminance (in place).
    const Levels lv =
        estimate_levels(sig, std::min(after_start, signal_end), signal_end, opt.shift_hz);
    rep.black_hz = lv.black;
    rep.white_hz = lv.white;
    rep.tuning_offset_hz = (lv.black + lv.white) / 2.0 - kCenterHz;
    const double span = lv.white - lv.black;
    // Line quality is always measured on the signal as received, before any repair,
    // so that enhancement never hides a poor reception (DSP-008, DSP-030).
    std::vector<std::uint8_t> received_ok(sig.size());
    {
        const double lo = lv.black - 0.3 * span;
        const double hi = lv.white + 0.3 * span;
        for (std::size_t i = 0; i < sig.size(); ++i) {
            const double f = static_cast<double>(sig[i]);
            received_ok[i] = (f > lo && f < hi) ? 1 : 0;
        }
    }
    if (opt.suppress_clicks) rep.clicks_repaired = suppress_clicks(sig, lv.black, lv.white);
    for (float& v : sig) v = static_cast<float>((static_cast<double>(v) - lv.black) / span);

    // 4a. Coarse clock error from the phasing window.
    const std::size_t ph_from = std::min(after_start, signal_end);
    const std::size_t ph_to =
        std::min(signal_end, ph_from + static_cast<std::size_t>(rate * kPhasingWindowS));
    const bool estimate_clock = !opt.clock_ppm;
    double ppm = opt.clock_ppm.value_or(0.0);
    if (estimate_clock && to_d(ph_to - ph_from) > 4.0 * nominal) {
        ppm = best_ppm(sig, ph_from, ph_to, nominal, 0.0, opt.max_clock_ppm, 20.0, kCoarseBins);
    }
    double period = nominal * (1.0 + ppm * 1e-6);

    // 4b. Phasing pulse: line origin, precise period, number of phasing lines.
    const Pulse pulse = find_pulse(fold(sig, ph_from, ph_to, period, kCoarseBins));
    double first_line = to_d(ph_from);
    if (std::fabs(pulse.contrast) > kMinPulseContrast) {
        const double origin = to_d(ph_from) + pulse.center_fraction * period;
        const double limit = std::min(to_d(signal_end), to_d(ph_from) + rate * kMaxPhasingS);
        if (const auto fit = track_phasing(sig, origin, period, pulse.contrast > 0.0, limit)) {
            rep.phasing_found = true;
            if (estimate_clock) {
                period = fit->period;
                ppm = ppm_of(period, nominal);
            }
            first_line = fit->origin + to_d(fit->lines) * period;
        }
    }
    if (!rep.phasing_found && estimate_clock && to_d(signal_end - ph_from) > 20.0 * nominal) {
        // No phasing: fine search on the image structures themselves.
        ppm = best_ppm(sig, ph_from, signal_end, nominal, ppm, 30.0, 1.0, 0);
        period = nominal * (1.0 + ppm * 1e-6);
    }

    // 4c. Refinement on long images (vertical structures: frame, grid, coastlines).
    if (estimate_clock && to_d(signal_end) > first_line + to_d(kRefineMinLines) * period) {
        const auto img_from = static_cast<std::size_t>(first_line);
        const std::size_t img_to = std::min(
            signal_end, img_from + static_cast<std::size_t>(to_d(kRefineMaxLines) * period));
        // Real case (GYA 2026-11-09 23:12): the phasing estimate was 13 ppm off, so the
        // search must be wide enough to correct it, then fine.
        ppm = best_ppm(sig, img_from, img_to, nominal, ppm, 20.0, 1.0, 0);
        ppm = best_ppm(sig, img_from, img_to, nominal, ppm, 1.0, 0.1, 0);
        period = nominal * (1.0 + ppm * 1e-6);
    }
    rep.clock_ppm = ppm;

    // 5. Rendering.
    const double usable = to_d(signal_end) - first_line;
    const std::size_t lines = usable > 0.0 ? static_cast<std::size_t>(usable / period) : 0;
    const auto width = static_cast<std::size_t>(pixels_per_line(rep.ioc));
    result.image = GrayImage(width, lines, 0);
    rep.line_quality.resize(lines);
    for (std::size_t y = 0; y < lines; ++y) {
        const double s = first_line + to_d(y) * period;
        render_line(sig, s, period, width, &result.image.at(0, y));
        const auto a = static_cast<std::size_t>(std::max(0.0, s));
        const auto b = std::min(sig.size(), static_cast<std::size_t>(s + period));
        std::size_t good = 0;
        for (std::size_t i = a; i < b; ++i) {
            good += received_ok[i];
        }
        rep.line_quality[y] = b > a ? static_cast<float>(to_d(good) / to_d(b - a)) : 0.0F;
    }

    if (opt.repair_lines) {
        rep.repaired_lines = repair_pixels(result.image, received_ok, first_line, period);
    }
    rep.apt_start_s = start ? to_d(start->begin) / rate : -1.0;
    rep.image_start_s = first_line / rate;
    rep.image_end_s = (first_line + to_d(lines) * period) / rate;
    return result;
}

std::vector<DecodeResult> decode_all(const audio::AudioBuffer& audio, const DecodeOptions& opt) {
    if (audio.sample_rate <= 0) throw std::invalid_argument("decode_all: invalid sample rate");
    constexpr double kMinSegmentS = 90.0;  // phasing (30 s) + one minute of image

    const auto in_rate = static_cast<double>(audio.sample_rate);
    const auto decim = std::max<std::size_t>(1, static_cast<std::size_t>(in_rate / kWorkingRate));
    dsp::FmDemodulator demod(in_rate, kCenterHz, kDemodCutoffHz, decim);
    std::vector<float> sig;
    sig.reserve(audio.samples.size() / decim + 1);
    demod.process(audio.samples.data(), audio.samples.size(), sig);
    const double rate = demod.output_rate();

    // All APT start tones, in order (input sample positions).
    std::vector<std::size_t> starts;
    std::vector<double> tones;
    if (opt.ioc) {
        tones.push_back(apt_start_hz(*opt.ioc));
    } else {
        tones = {apt_start_hz(Ioc::k576), apt_start_hz(Ioc::k288)};
    }
    for (double tone : tones) {
        std::size_t pos = 0;
        while (const auto run = find_tone_run(sig, rate, tone, pos, kStartToneMinBlocks)) {
            starts.push_back(run->begin * decim);
            pos = run->end;
        }
    }
    std::sort(starts.begin(), starts.end());
    sig.clear();
    sig.shrink_to_fit();

    std::vector<std::size_t> bounds;
    const auto min_len = static_cast<std::size_t>(kMinSegmentS * in_rate);
    if (starts.empty() || starts.front() >= min_len) bounds.push_back(0);
    bounds.insert(bounds.end(), starts.begin(), starts.end());
    bounds.push_back(audio.samples.size());

    std::vector<DecodeResult> results;
    for (std::size_t i = 0; i + 1 < bounds.size(); ++i) {
        const std::size_t a = bounds[i];
        const std::size_t b = bounds[i + 1];
        if (b - a < min_len) continue;
        audio::AudioBuffer part;
        part.sample_rate = audio.sample_rate;
        part.samples.assign(audio.samples.begin() + static_cast<std::ptrdiff_t>(a),
                            audio.samples.begin() + static_cast<std::ptrdiff_t>(b));
        try {
            DecodeResult r = decode(part, opt);
            if (r.image.height < 20) continue;
            // Reject segments without content, e.g. the idle white carrier the DWD keeps
            // between charts (real capture, 2026-11-09 22:11-22:45 UTC).
            // A chart has many lines with internal texture; a carrier (white or black)
            // gives uniform lines whatever its level.
            std::size_t textured = 0;
            for (std::size_t y = 0; y < r.image.height; ++y) {
                double sum = 0.0;
                double sum2 = 0.0;
                for (std::size_t x = 0; x < r.image.width; ++x) {
                    const double px = r.image.at(x, y);
                    sum += px;
                    sum2 += px * px;
                }
                const double w = to_d(r.image.width);
                const double mean = sum / w;
                if (std::sqrt(std::max(0.0, sum2 / w - mean * mean)) >= kMinContentStd) {
                    ++textured;
                }
            }
            if (to_d(textured) < kMinTexturedFraction * to_d(r.image.height)) continue;
            const double offset_s = to_d(a) / in_rate;
            if (r.report.apt_start_s >= 0.0) r.report.apt_start_s += offset_s;
            r.report.image_start_s += offset_s;
            r.report.image_end_s += offset_s;
            results.push_back(std::move(r));
        } catch (const std::exception&) {
            // Unusable segment (noise, too short): skip it.
        }
    }
    return results;
}

// ---------------------------------------------------------------------------------
// Streaming decoder (DSP-007)
// ---------------------------------------------------------------------------------

namespace {

/// Incremental version of find_tone_run(): fed one 0.25 s block at a time.
struct ToneTracker {
    double tone_hz = 0.0;
    std::size_t min_hits = 0;
    std::size_t hits = 0;
    std::size_t gap = 0;
    std::size_t start = 0;  // absolute sample index
    std::size_t end = 0;

    /// Returns a completed run (absolute sample indices) when one ends.
    std::optional<Run> feed(bool hit, std::size_t pos, std::size_t block) {
        if (hit) {
            if (hits == 0) start = pos;
            ++hits;
            gap = 0;
            end = pos + block;
        } else if (hits > 0 && ++gap > 1) {
            const Run run{start, end};
            const bool ok = hits >= min_hits;
            hits = 0;
            gap = 0;
            if (ok) return run;
        }
        return std::nullopt;
    }
    void reset() { hits = gap = 0; }
};

constexpr double kIdleKeepS = 12.0;  // history kept while waiting for a start
constexpr double kMaxTransmissionS = 30.0 * 60.0;
constexpr double kPhasingRetryS = 2.0;
// Live lines lag the input by this much, so that the APT stop tone (confirmed after
// 1.5 s + one 0.25 s block) is detected before any line inside it is shown.
constexpr double kLiveDelayS = 2.5;

}  // namespace

struct StreamDecoder::Impl {
    enum class State { kIdle, kPhasing, kImage };

    DecodeOptions opt;
    std::optional<DecodeOptions> enhanced_opt;
    dsp::FmDemodulator demod;
    double rate;
    double nominal;
    std::size_t block;
    State state = State::kIdle;

    std::vector<float> freq;  // instantaneous frequency, freq[0] = absolute index `origin`
    std::vector<float> lum;   // luminance, same indexing (once levels are known)
    std::size_t origin = 0;
    std::size_t scanned = 0;  // absolute index of the next tone block to analyse

    ToneTracker start576{apt_start_hz(Ioc::k576), kStartToneMinBlocks};
    ToneTracker start288{apt_start_hz(Ioc::k288), kStartToneMinBlocks};
    ToneTracker stop{kAptStopHz, kStopToneMinBlocks};

    Ioc ioc = Ioc::k576;
    std::size_t tx_start = 0;      // absolute
    std::size_t phasing_from = 0;  // absolute
    double next_attempt = 0.0;     // absolute index of the next phasing analysis
    Levels levels;
    double first_line = 0.0;  // absolute
    double period = 0.0;
    std::size_t next_line = 0;
    double refined_clock_ppm = 0.0;
    std::vector<StreamEvent> events;

    Impl(int sample_rate, const DecodeOptions& o, std::optional<DecodeOptions> enh)
        : opt(o),
          enhanced_opt(std::move(enh)),
          demod(make_demodulator(static_cast<double>(sample_rate))),
          rate(demod.output_rate()),
          nominal(rate * line_duration_s(o.lpm)),
          block(static_cast<std::size_t>(rate * kToneBlockS)) {}

    std::size_t end() const { return origin + freq.size(); }
    double seconds(double abs_index) const { return abs_index / rate; }

    void drop_before(std::size_t abs_index) {
        if (abs_index <= origin) return;
        const std::size_t drop = std::min(freq.size(), abs_index - origin);
        freq.erase(freq.begin(), freq.begin() + static_cast<std::ptrdiff_t>(drop));
        if (!lum.empty()) {
            lum.erase(lum.begin(),
                      lum.begin() + static_cast<std::ptrdiff_t>(std::min(drop, lum.size())));
        }
        origin += drop;
    }

    void begin(std::size_t abs_start, std::size_t abs_phasing, Ioc which, bool from_tone) {
        drop_before(abs_start);
        state = State::kPhasing;
        ioc = opt.ioc.value_or(which);
        tx_start = abs_start;
        phasing_from = abs_phasing;
        next_attempt = to_d(phasing_from) + rate * (kPhasingWindowS + 1.0);
        stop.reset();
        StreamEvent e;
        e.kind = StreamEvent::Kind::kStart;
        e.time_s = seconds(to_d(abs_start));
        e.ioc = ioc;
        e.apt_start = from_tone;
        events.push_back(std::move(e));
    }

    void update_lum(std::size_t from_rel) {
        lum.resize(freq.size());
        const double span = levels.white - levels.black;
        for (std::size_t i = from_rel; i < freq.size(); ++i) {
            lum[i] = static_cast<float>((static_cast<double>(freq[i]) - levels.black) / span);
        }
    }

    void try_lock() {
        if (to_d(end()) < next_attempt) return;
        const std::size_t ph_from = phasing_from - origin;
        const std::size_t avail = freq.size();
        const std::size_t ph_to =
            std::min(avail, ph_from + static_cast<std::size_t>(rate * kPhasingWindowS));
        levels = estimate_levels(freq, ph_from, avail, opt.shift_hz);
        update_lum(0);

        double ppm = opt.clock_ppm.value_or(0.0);
        if (!opt.clock_ppm) {
            ppm = best_ppm(lum, ph_from, ph_to, nominal, 0.0, opt.max_clock_ppm, 20.0, kCoarseBins);
        }
        double per = nominal * (1.0 + ppm * 1e-6);
        const Pulse pulse = find_pulse(fold(lum, ph_from, ph_to, per, kCoarseBins));
        bool locked = false;
        bool phasing_found = false;
        double first = to_d(ph_from);
        if (std::fabs(pulse.contrast) > kMinPulseContrast) {
            const double org = to_d(ph_from) + pulse.center_fraction * per;
            if (const auto fit = track_phasing(lum, org, per, pulse.contrast > 0.0, to_d(avail))) {
                const double fit_end = fit->origin + to_d(fit->lines) * fit->period;
                // Phasing is over when at least 3 image lines follow the last pulse.
                if (fit_end + 3.0 * fit->period < to_d(avail)) {
                    if (!opt.clock_ppm) per = fit->period;
                    first = fit_end;
                    locked = true;
                    phasing_found = true;
                }
            }
        }
        const bool give_up = to_d(avail - ph_from) > rate * kMaxPhasingS;
        if (!locked && !give_up) {
            next_attempt = to_d(end()) + rate * kPhasingRetryS;
            return;
        }
        period = per;
        first_line = to_d(origin) + first;
        next_line = 0;
        state = State::kImage;
        StreamEvent e;
        e.kind = StreamEvent::Kind::kLocked;
        e.time_s = seconds(first_line);
        e.clock_ppm = (period / nominal - 1.0) * 1e6;
        e.tuning_offset_hz = (levels.black + levels.white) / 2.0 - kCenterHz;
        e.phasing_found = phasing_found;
        events.push_back(std::move(e));
    }

    /// Progressive clock refinement on the lines already received (same method as the
    /// offline pipeline). The current line keeps its position, so the live image has no
    /// break; following lines use the refined period. Real case: GYA phasing estimate
    /// -61 ppm, image refinement -43 ppm (~40 px of slant avoided over a chart).
    void refine_period() {
        if (opt.clock_ppm) return;
        const double from = first_line - to_d(origin);
        const double to = from + to_d(next_line) * period;
        if (from < 0.0) return;
        const auto a = static_cast<std::size_t>(from);
        const auto b = std::min(lum.size(), static_cast<std::size_t>(to));
        double ppm = (period / nominal - 1.0) * 1e6;
        ppm = best_ppm(lum, a, b, nominal, ppm, 20.0, 1.0, 0);
        ppm = best_ppm(lum, a, b, nominal, ppm, 1.0, 0.1, 0);
        const double current = first_line + to_d(next_line) * period;
        period = nominal * (1.0 + ppm * 1e-6);
        first_line = current - to_d(next_line) * period;
        refined_clock_ppm = ppm;
    }

    void emit_lines(std::size_t limit_abs) {
        const auto width = static_cast<std::size_t>(pixels_per_line(ioc));
        while (first_line + to_d(next_line + 1) * period + 2.0 <= to_d(limit_abs)) {
            if (next_line == kRefineMinLines || next_line == 2 * kRefineMinLines) refine_period();
            const double s_abs = first_line + to_d(next_line) * period;
            const double s_rel = s_abs - to_d(origin);
            StreamEvent e;
            e.kind = StreamEvent::Kind::kLine;
            e.time_s = seconds(s_abs);
            e.line_index = next_line;
            e.pixels.resize(width);
            render_line(lum, s_rel, period, width, e.pixels.data());
            const auto a = static_cast<std::size_t>(std::max(0.0, s_rel));
            const auto b = std::min(lum.size(), static_cast<std::size_t>(s_rel + period));
            std::size_t good = 0;
            for (std::size_t i = a; i < b; ++i) {
                if (lum[i] > -0.3F && lum[i] < 1.3F) ++good;
            }
            e.quality = b > a ? static_cast<float>(to_d(good) / to_d(b - a)) : 0.0F;
            events.push_back(std::move(e));
            ++next_line;
        }
    }

    void complete(std::size_t end_abs) {
        if (state == State::kImage) emit_lines(std::min(end_abs, end()));
        StreamEvent e;
        e.kind = StreamEvent::Kind::kComplete;
        e.time_s = seconds(to_d(end_abs));
        // Final image: full offline pipeline on the transmission, stop tone included.
        const std::size_t keep =
            std::min(freq.size(), end_abs - origin + static_cast<std::size_t>(rate * 6.0));
        std::vector<float> tx(freq.begin(), freq.begin() + static_cast<std::ptrdiff_t>(keep));
        auto run = [&](DecodeOptions o, std::vector<float> sig) {
            o.ioc = ioc;
            try {
                auto r = std::make_shared<DecodeResult>(decode_frequency(std::move(sig), rate, o));
                if (r->image.height > 0) return std::shared_ptr<const DecodeResult>(std::move(r));
            } catch (const std::exception&) {
                // Too short or unusable: no image.
            }
            return std::shared_ptr<const DecodeResult>();
        };
        if (enhanced_opt) e.enhanced = run(*enhanced_opt, tx);
        e.final = run(opt, std::move(tx));
        events.push_back(std::move(e));
        drop_before(end_abs);
        lum.clear();
        state = State::kIdle;
        start576.reset();
        start288.reset();
        stop.reset();
    }

    /// Analyses new 0.25 s blocks for APT tones. Returns false if the buffer was
    /// rearranged (transmission completed) and scanning must resume later.
    void scan_tones() {
        while (scanned + block <= end()) {
            if (scanned < origin) scanned = origin;
            if (scanned + block > end()) break;
            const float* p = &freq[scanned - origin];
            const std::size_t pos = scanned;
            scanned += block;
            if (state == State::kIdle) {
                const bool h576 =
                    dsp::tone_ratio(p, block, start576.tone_hz, rate) > kToneThreshold;
                const bool h288 =
                    dsp::tone_ratio(p, block, start288.tone_hz, rate) > kToneThreshold;
                const auto r576 = start576.feed(h576, pos, block);
                const auto r288 = start288.feed(h288, pos, block);
                const bool use576 = !opt.ioc || *opt.ioc == Ioc::k576;
                const bool use288 = !opt.ioc || *opt.ioc == Ioc::k288;
                if (r576 && use576) {
                    begin(r576->begin, r576->end, Ioc::k576, true);
                } else if (r288 && use288) {
                    begin(r288->begin, r288->end, Ioc::k288, true);
                }
            } else {
                const bool hit = dsp::tone_ratio(p, block, kAptStopHz, rate) > kToneThreshold;
                stop.feed(hit, pos, block);
                // React as soon as the stop tone is confirmed (1.5 s), not at its end:
                // live lines are delayed by kLiveDelayS so none falls inside the tone.
                if (stop.hits >= stop.min_hits && stop.start > phasing_from) {
                    complete(stop.start);
                }
            }
        }
    }

    void push(const float* samples, std::size_t count) {
        const std::size_t before = freq.size();
        demod.process(samples, count, freq);
        if (state == State::kImage) update_lum(before);
        scan_tones();
        if (state == State::kPhasing) try_lock();
        if (state == State::kImage) {
            const auto delay = static_cast<std::size_t>(rate * kLiveDelayS);
            if (end() > delay) emit_lines(end() - delay);
            if (to_d(end() - tx_start) > rate * kMaxTransmissionS) complete(end());
        }
        if (state == State::kIdle) {
            const auto keep = static_cast<std::size_t>(rate * kIdleKeepS);
            if (freq.size() > 2 * keep) drop_before(end() - keep);
        }
    }
};

StreamDecoder::StreamDecoder(int sample_rate, DecodeOptions options,
                             std::optional<DecodeOptions> enhanced) {
    if (sample_rate <= 0) throw std::invalid_argument("StreamDecoder: invalid sample rate");
    impl_ = std::make_unique<Impl>(sample_rate, options, std::move(enhanced));
}

StreamDecoder::~StreamDecoder() = default;

void StreamDecoder::push(const float* samples, std::size_t count) {
    impl_->push(samples, count);
}

std::vector<StreamEvent> StreamDecoder::take_events() {
    std::vector<StreamEvent> out;
    out.swap(impl_->events);
    return out;
}

void StreamDecoder::force_start() {
    if (impl_->state != Impl::State::kIdle) return;
    const std::size_t now = impl_->end();
    impl_->begin(now, now, impl_->opt.ioc.value_or(Ioc::k576), false);
}

void StreamDecoder::force_stop() {
    if (impl_->state == Impl::State::kIdle) return;
    impl_->complete(impl_->end());
}

bool StreamDecoder::receiving() const noexcept {
    return impl_->state != Impl::State::kIdle;
}

}  // namespace owfax::wefax
