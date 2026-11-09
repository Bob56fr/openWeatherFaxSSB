// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// owfax-cli — command line front-end to the core (development and headless use).

#include <cstdio>
#include <cstring>
#include <string>

#include "owfax/build_info.hpp"
#include "owfax/capabilities.hpp"

namespace {

void print_version() {
    const auto& info = owfax::build_info();
    std::printf("owfax-cli %.*s (%.*s)\n", static_cast<int>(info.version.size()),
                info.version.data(), static_cast<int>(info.git_revision.size()),
                info.git_revision.data());
    std::printf("License: %.*s — this program comes with ABSOLUTELY NO WARRANTY.\n",
                static_cast<int>(info.license.size()), info.license.data());
    std::printf("Source:  %.*s\n", static_cast<int>(info.source_url.size()),
                info.source_url.data());
}

void print_capabilities() {
    const owfax::HostCapabilities caps = owfax::detect_host_capabilities();
    const auto tier = owfax::to_string(owfax::classify(caps));
    std::printf("OS family:     %s\n", caps.os_family.c_str());
    std::printf("CPU cores:     %u\n", caps.cpu_cores);
    std::printf("RAM:           %llu MiB\n",
                static_cast<unsigned long long>(caps.ram_bytes / (1024ULL * 1024ULL)));
    std::printf("Hardware tier: %.*s\n", static_cast<int>(tier.size()), tier.data());
}

void print_usage() {
    std::printf(
        "Usage: owfax-cli [option]\n"
        "  --version        Show version, license and source location\n"
        "  --capabilities   Show detected host capabilities and hardware tier\n"
        "  --help           Show this help\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }
    if (std::strcmp(argv[1], "--version") == 0) {
        print_version();
        return 0;
    }
    if (std::strcmp(argv[1], "--capabilities") == 0) {
        print_capabilities();
        return 0;
    }
    std::fprintf(stderr, "Unknown option: %s\n\n", argv[1]);
    print_usage();
    return 2;
}
