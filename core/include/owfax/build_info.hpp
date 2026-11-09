// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#pragma once

#include <string_view>

namespace owfax {

/// Build identification, filled in at configure time.
/// The source URL and revision must be shown to every user of the web UI
/// (AGPL-3.0 section 13, spec UI-006).
struct BuildInfo {
    std::string_view version;       ///< Semantic version, e.g. "0.1.0".
    std::string_view git_revision;  ///< Commit id, "-dirty" if modified, or "unknown".
    std::string_view source_url;    ///< Public repository URL.
    std::string_view license;       ///< SPDX license identifier.
};

/// Returns the build information of the linked core library.
const BuildInfo& build_info() noexcept;

}  // namespace owfax
