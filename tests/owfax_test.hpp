// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Minimal self-registering test harness. Usage:
//
//   OWFAX_TEST(my_test_name) {
//       OWFAX_CHECK(1 + 1 == 2);
//       OWFAX_CHECK_NEAR(0.1 + 0.2, 0.3, 1e-12);
//   }

#pragma once

#include <cmath>
#include <cstdio>
#include <vector>

namespace owfax::test {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report_failure(const char* file, int line, const char* expr) {
    std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, expr);
    ++failure_count();
}

}  // namespace owfax::test

#define OWFAX_TEST(name)                                                                     \
    static void owfax_test_##name();                                                         \
    static const ::owfax::test::Registrar owfax_registrar_##name{#name, &owfax_test_##name}; \
    static void owfax_test_##name()

#define OWFAX_CHECK(expr)                                                      \
    do {                                                                       \
        if (!(expr)) ::owfax::test::report_failure(__FILE__, __LINE__, #expr); \
    } while (0)

#define OWFAX_CHECK_NEAR(actual, expected, tolerance) \
    OWFAX_CHECK(std::fabs((actual) - (expected)) <= (tolerance))
