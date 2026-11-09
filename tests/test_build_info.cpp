// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/build_info.hpp"
#include "owfax_test.hpp"

OWFAX_TEST(build_info_is_complete) {
    const auto& info = owfax::build_info();
    OWFAX_CHECK(!info.version.empty());
    OWFAX_CHECK(!info.git_revision.empty());
    // AGPL-3.0 §13: the source location must always be available (spec UI-006).
    OWFAX_CHECK(info.source_url.substr(0, 8) == "https://");
    OWFAX_CHECK(info.license == "AGPL-3.0-or-later");
}
