// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/capabilities.hpp"
#include "owfax_test.hpp"

using owfax::classify;
using owfax::HardwareTier;
using owfax::HostCapabilities;

namespace {
constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

HostCapabilities host(unsigned cores, std::uint64_t ram_mib) {
    HostCapabilities caps;
    caps.cpu_cores = cores;
    caps.ram_bytes = ram_mib * kMiB;
    return caps;
}
}  // namespace

OWFAX_TEST(unknown_host_is_discovery) {
    OWFAX_CHECK(classify(HostCapabilities{}) == HardwareTier::kDiscovery);
}

OWFAX_TEST(reference_hardware_tiers) {
    OWFAX_CHECK(classify(host(4, 512)) == HardwareTier::kDiscovery);   // Pi Zero 2 W
    OWFAX_CHECK(classify(host(4, 1024)) == HardwareTier::kDiscovery);  // Pi 3B
    OWFAX_CHECK(classify(host(4, 2048)) == HardwareTier::kStandard);   // Pi 4 2 GB
    OWFAX_CHECK(classify(host(4, 4096)) == HardwareTier::kAdvanced);   // Pi 4 4 GB
    OWFAX_CHECK(classify(host(2, 8192)) == HardwareTier::kDiscovery);  // dual-core PC
}

OWFAX_TEST(tier_names_are_stable) {
    OWFAX_CHECK(owfax::to_string(HardwareTier::kDiscovery) == "discovery");
    OWFAX_CHECK(owfax::to_string(HardwareTier::kStandard) == "standard");
    OWFAX_CHECK(owfax::to_string(HardwareTier::kAdvanced) == "advanced");
}

OWFAX_TEST(detection_does_not_crash) {
    const HostCapabilities caps = owfax::detect_host_capabilities();
    OWFAX_CHECK(!caps.os_family.empty());
}
