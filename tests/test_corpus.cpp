// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Regression test on real receptions (spec TST-001/TST-002). Runs only when the
// corpus has been downloaded (tools/corpus/fetch.sh) and OWFAX_CORPUS_DIR points to it.
// Each entry must decode at least as well as its recorded baseline.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "owfax/audio/wav.hpp"
#include "owfax/wefax/decoder.hpp"
#include "owfax_test.hpp"

#ifndef OWFAX_CORPUS_TSV
#error "OWFAX_CORPUS_TSV must be defined by CMake"
#endif

namespace {

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, '\t')) out.push_back(field);
    return out;
}

}  // namespace

OWFAX_TEST(corpus_real_receptions_no_regression) {
    const char* dir = std::getenv("OWFAX_CORPUS_DIR");
    if (dir == nullptr) {
        std::printf("    skipped: OWFAX_CORPUS_DIR not set (see tools/corpus/fetch.sh)\n");
        return;
    }
    std::ifstream tsv(OWFAX_CORPUS_TSV);
    OWFAX_CHECK(tsv.good());
    std::string line;
    std::getline(tsv, line);  // header
    int entries = 0;
    while (std::getline(tsv, line)) {
        const auto f = split_tabs(line);
        if (f.size() < 10) continue;
        ++entries;
        const std::string path = std::string(dir) + "/" + f[0];
        const double base_lines = std::stod(f[5]);
        const double base_ppm = std::stod(f[6]);
        const double base_tuning = std::stod(f[7]);
        const double base_quality = std::stod(f[8]) / 100.0;

        owfax::wefax::DecodeOptions opt;
        opt.shift_hz = std::stod(f[9]);
        const auto result = owfax::wefax::decode(owfax::audio::read_wav(path), opt);
        const auto& r = result.report;
        double quality = 0.0;
        for (float q : r.line_quality) quality += static_cast<double>(q);
        if (!r.line_quality.empty()) quality /= static_cast<double>(r.line_quality.size());

        std::printf(
            "    %s: lines %zu (base %.0f) ppm %+.1f (%+.1f) tuning %+.1f (%+.1f) "
            "quality %.2f (%.2f)\n",
            f[0].c_str(), result.image.height, base_lines, r.clock_ppm, base_ppm,
            r.tuning_offset_hz, base_tuning, quality, base_quality);
        OWFAX_CHECK_NEAR(static_cast<double>(result.image.height), base_lines, 2.0);
        OWFAX_CHECK_NEAR(r.clock_ppm, base_ppm, 2.0);
        OWFAX_CHECK_NEAR(r.tuning_offset_hz, base_tuning, 3.0);
        OWFAX_CHECK(quality >= base_quality - 0.02);
    }
    OWFAX_CHECK(entries > 0);
}
