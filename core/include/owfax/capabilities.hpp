// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Host capability detection and hardware tier classification — spec §2.2.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace owfax {

/// Hardware tiers from spec §2.2. Each tier enables the features of the previous one.
enum class HardwareTier {
    kDiscovery,  ///< Audio input + full WEFAX decode. Must work on any supported device.
    kStandard,   ///< + CAT, one SDR, scheduling, RTTY/NAVTEX, advanced DSP.
    kAdvanced,   ///< + several SDRs, frequency diversity, two simultaneous stations.
};

/// What the core could measure about the host it runs on.
struct HostCapabilities {
    unsigned cpu_cores = 0;       ///< Logical cores; 0 if unknown.
    std::uint64_t ram_bytes = 0;  ///< Physical memory; 0 if unknown.
    std::string os_family;        ///< "windows", "linux", "android" or "unknown".
};

/// Probes the current host. Never throws; unknown values stay at their defaults.
HostCapabilities detect_host_capabilities();

/// Classifies a host into a tier. Pure function, so it can be unit tested.
/// Unknown values are treated conservatively (lowest tier).
/// Thresholds are provisional and will be tuned on the reference hardware (spec TST-004).
HardwareTier classify(const HostCapabilities& caps) noexcept;

/// Stable identifier for logs, the API and translations: "discovery", "standard", "advanced".
std::string_view to_string(HardwareTier tier) noexcept;

}  // namespace owfax
