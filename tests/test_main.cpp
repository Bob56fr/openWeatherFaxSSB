// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax_test.hpp"

int main() {
    using namespace owfax::test;
    int failed_tests = 0;
    for (const TestCase& tc : registry()) {
        const int before = failure_count();
        tc.fn();
        const bool ok = failure_count() == before;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", tc.name);
        if (!ok) ++failed_tests;
    }
    std::printf("\n%zu tests, %d failed\n", registry().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
