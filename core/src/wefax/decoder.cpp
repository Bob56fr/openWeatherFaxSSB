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

#include <algorithm>
#include <cmath>
#include <stdexcept>

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

std::optional<Run> find_tone_run(const std::vector<float>& sig, double rate, double tone_hz,
                                 std::size_t from, std::size_t min_blocks) {
    const auto block = static_cast<std::size_t>(rate * kToneBlockS);
    if (block == 0) return std::nullopt;
    std::size_t run_start = 0;
    std::size_t run_len = 0;
    for (std::size_t pos = from; pos + block <= sig.size(); pos += block) {
        if (dsp::tone_ratio(&sig[pos], block, tone_hz, rate) > kToneThreshold) {
            if (run_len == 0) run_start = pos;
            ++run_len;
        } else {
            if (run_len >= min_blocks) return Run{run_start, pos};
            run_len = 0;
        }
    }
    if (run_len >= min_blocks) return Run{run_start, run_start + run_len * block};
    return std::nullopt;
}

struct Levels {
    double black = kBlackHz;
    double white = kWhiteHz;
};

/// Black and white frequencies = the two main peaks of the frequency histogram over
/// phasing + image (both levels are abundant there; peaks are robust to noise).
Levels estimate_levels(const std::vector<float>& sig, std::size_t from, std::size_t to) {
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
        // other one from the nominal 800 Hz shift, if the peak is plausibly black or white.
        const double f = refine(p1);
        if (std::fabs(f - kWhiteHz) < 250.0) {
            lv.white = f;
            lv.black = f - (kWhiteHz - kBlackHz);
        } else if (std::fabs(f - kBlackHz) < 250.0) {
            lv.black = f;
            lv.white = f + (kWhiteHz - kBlackHz);
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

double ppm_of(double period, double nominal) {
    return (period / nominal - 1.0) * 1e6;
}

}  // namespace

DecodeResult decode(const audio::AudioBuffer& audio, const DecodeOptions& opt) {
    if (audio.sample_rate <= 0) throw std::invalid_argument("decode: invalid sample rate");
    const auto in_rate = static_cast<double>(audio.sample_rate);
    const auto decim = std::max<std::size_t>(1, static_cast<std::size_t>(in_rate / kWorkingRate));

    // 1. Instantaneous frequency.
    dsp::FmDemodulator demod(in_rate, kCenterHz, kDemodCutoffHz, decim);
    std::vector<float> sig;
    sig.reserve(audio.samples.size() / decim + 1);
    demod.process(audio.samples.data(), audio.samples.size(), sig);
    const double rate = demod.output_rate();
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
    const Levels lv = estimate_levels(sig, std::min(after_start, signal_end), signal_end);
    rep.black_hz = lv.black;
    rep.white_hz = lv.white;
    rep.tuning_offset_hz = (lv.black + lv.white) / 2.0 - kCenterHz;
    const double span = lv.white - lv.black;
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
        ppm = best_ppm(sig, img_from, img_to, nominal, ppm, 4.0, 0.25, 0);
        period = nominal * (1.0 + ppm * 1e-6);
    }
    rep.clock_ppm = ppm;

    // 5. Rendering.
    const double usable = to_d(signal_end) - first_line;
    const std::size_t lines = usable > 0.0 ? static_cast<std::size_t>(usable / period) : 0;
    const auto width = static_cast<std::size_t>(pixels_per_line(rep.ioc));
    result.image = GrayImage(width, lines, 0);
    rep.line_quality.resize(lines);
    const double px = period / to_d(width);
    for (std::size_t y = 0; y < lines; ++y) {
        const double s = first_line + to_d(y) * period;
        for (std::size_t x = 0; x < width; ++x) {
            const double c = s + (to_d(x) + 0.5) * px;
            const double v = (sample_at(sig, c - 0.25 * px) + sample_at(sig, c) +
                              sample_at(sig, c + 0.25 * px)) /
                             3.0;
            result.image.at(x, y) = static_cast<std::uint8_t>(std::lround(v * 255.0));
        }
        const auto a = static_cast<std::size_t>(std::max(0.0, s));
        const auto b = std::min(sig.size(), static_cast<std::size_t>(s + period));
        std::size_t good = 0;
        for (std::size_t i = a; i < b; ++i) {
            if (sig[i] > -0.3F && sig[i] < 1.3F) ++good;
        }
        rep.line_quality[y] = b > a ? static_cast<float>(to_d(good) / to_d(b - a)) : 0.0F;
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

}  // namespace owfax::wefax
