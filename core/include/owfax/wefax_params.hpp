// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// WEFAX (HF radiofacsimile) mode constants — spec §6.1.

#pragma once

#include <optional>

namespace owfax::wefax {

/// Audio sub-carrier frequency for black pixels, in Hz.
inline constexpr double kBlackHz = 1500.0;
/// Audio sub-carrier frequency for white pixels, in Hz.
inline constexpr double kWhiteHz = 2300.0;
/// Centre of the sub-carrier; the assigned frequency minus this gives the USB dial.
inline constexpr double kCenterHz = (kBlackHz + kWhiteHz) / 2.0;  // 1900 Hz
/// Peak deviation from the centre, in Hz.
inline constexpr double kDeviationHz = (kWhiteHz - kBlackHz) / 2.0;  // 400 Hz

/// APT stop tone, common to both IOCs, in Hz.
inline constexpr double kAptStopHz = 450.0;

/// Index of Cooperation: sets the image aspect ratio.
enum class Ioc { k576 = 576, k288 = 288 };

/// Lines per minute.
enum class Lpm { k60 = 60, k90 = 90, k120 = 120 };

inline constexpr double kPi = 3.14159265358979323846;

/// Number of pixels in one scan line: round(IOC × π) (1810 for IOC 576).
constexpr int pixels_per_line(Ioc ioc) noexcept {
    const double exact = static_cast<double>(static_cast<int>(ioc)) * kPi;
    return static_cast<int>(exact + 0.5);
}

/// Duration of one scan line, in seconds (0.5 s at 120 LPM).
constexpr double line_duration_s(Lpm lpm) noexcept {
    return 60.0 / static_cast<double>(static_cast<int>(lpm));
}

/// APT start tone that announces a transmission with the given IOC, in Hz.
constexpr double apt_start_hz(Ioc ioc) noexcept {
    return ioc == Ioc::k576 ? 300.0 : 675.0;
}

/// Identifies the IOC from a measured APT start tone (± tolerance_hz), if any.
constexpr std::optional<Ioc> ioc_from_apt_start(double tone_hz,
                                                double tolerance_hz = 20.0) noexcept {
    for (Ioc ioc : {Ioc::k576, Ioc::k288}) {
        const double delta = tone_hz - apt_start_hz(ioc);
        if (delta <= tolerance_hz && delta >= -tolerance_hz) {
            return ioc;
        }
    }
    return std::nullopt;
}

/// Receiver dial frequency in USB for a station's assigned frequency, both in kHz
/// (spec CAT-003, CAT-008): assigned − 1.9 kHz.
constexpr double usb_dial_khz(double assigned_khz) noexcept {
    return assigned_khz - kCenterHz / 1000.0;
}

/// Maps an instantaneous sub-carrier frequency to a luminance in [0, 1]
/// (0 = black, 1 = white), clamped.
constexpr double luminance_from_hz(double hz) noexcept {
    const double v = (hz - kBlackHz) / (kWhiteHz - kBlackHz);
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

}  // namespace owfax::wefax
