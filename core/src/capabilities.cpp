// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/capabilities.hpp"

#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace owfax {
namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

std::uint64_t detect_ram_bytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        return static_cast<std::uint64_t>(status.ullTotalPhys);
    }
    return 0;
#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
    }
    return 0;
#else
    return 0;
#endif
}

const char* detect_os_family() {
#if defined(__ANDROID__)
    return "android";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

}  // namespace

HostCapabilities detect_host_capabilities() {
    HostCapabilities caps;
    caps.cpu_cores = std::thread::hardware_concurrency();
    caps.ram_bytes = detect_ram_bytes();
    caps.os_family = detect_os_family();
    return caps;
}

HardwareTier classify(const HostCapabilities& caps) noexcept {
    // Provisional thresholds (spec TST-004 will calibrate them):
    //  - Advanced: Pi 4 with 4 GB, Pi 5, recent PC.
    //  - Standard: Pi 4 with 2 GB, Android 8+ class phones, Windows 10 PCs.
    //  - Discovery: everything else (Pi Zero 2 W has 512 MB, Pi 3B has 1 GB).
    if (caps.cpu_cores >= 4 && caps.ram_bytes >= 3500 * kMiB) {
        return HardwareTier::kAdvanced;
    }
    if (caps.cpu_cores >= 4 && caps.ram_bytes >= 1800 * kMiB) {
        return HardwareTier::kStandard;
    }
    return HardwareTier::kDiscovery;
}

std::string_view to_string(HardwareTier tier) noexcept {
    switch (tier) {
        case HardwareTier::kDiscovery: return "discovery";
        case HardwareTier::kStandard: return "standard";
        case HardwareTier::kAdvanced: return "advanced";
    }
    return "discovery";
}

}  // namespace owfax
