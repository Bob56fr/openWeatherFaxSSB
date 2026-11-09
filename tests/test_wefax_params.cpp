// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/wefax_params.hpp"
#include "owfax_test.hpp"

using namespace owfax::wefax;

// Compile-time checks: these constants must never drift.
static_assert(kCenterHz == 1900.0, "WEFAX centre frequency is 1900 Hz");
static_assert(kDeviationHz == 400.0, "WEFAX deviation is ±400 Hz");
static_assert(pixels_per_line(Ioc::k576) == 1810, "IOC 576 → 1810 px/line");
static_assert(pixels_per_line(Ioc::k288) == 905, "IOC 288 → 905 px/line");

OWFAX_TEST(line_duration_matches_lpm) {
    OWFAX_CHECK_NEAR(line_duration_s(Lpm::k120), 0.5, 1e-12);
    OWFAX_CHECK_NEAR(line_duration_s(Lpm::k90), 60.0 / 90.0, 1e-12);
    OWFAX_CHECK_NEAR(line_duration_s(Lpm::k60), 1.0, 1e-12);
}

OWFAX_TEST(apt_start_tones_identify_ioc) {
    OWFAX_CHECK(apt_start_hz(Ioc::k576) == 300.0);
    OWFAX_CHECK(apt_start_hz(Ioc::k288) == 675.0);
    OWFAX_CHECK(ioc_from_apt_start(305.0) == Ioc::k576);
    OWFAX_CHECK(ioc_from_apt_start(668.0) == Ioc::k288);
    OWFAX_CHECK(!ioc_from_apt_start(kAptStopHz).has_value());
    OWFAX_CHECK(!ioc_from_apt_start(1900.0).has_value());
}

OWFAX_TEST(usb_dial_is_assigned_minus_1_9_khz) {
    // DWD Pinneberg 7880 kHz → dial 7878.1 kHz USB.
    OWFAX_CHECK_NEAR(usb_dial_khz(7880.0), 7878.1, 1e-9);
    OWFAX_CHECK_NEAR(usb_dial_khz(13882.5), 13880.6, 1e-9);
}

OWFAX_TEST(luminance_mapping_is_clamped) {
    OWFAX_CHECK_NEAR(luminance_from_hz(kBlackHz), 0.0, 1e-12);
    OWFAX_CHECK_NEAR(luminance_from_hz(kWhiteHz), 1.0, 1e-12);
    OWFAX_CHECK_NEAR(luminance_from_hz(kCenterHz), 0.5, 1e-12);
    OWFAX_CHECK_NEAR(luminance_from_hz(1000.0), 0.0, 1e-12);
    OWFAX_CHECK_NEAR(luminance_from_hz(3000.0), 1.0, 1e-12);
}
