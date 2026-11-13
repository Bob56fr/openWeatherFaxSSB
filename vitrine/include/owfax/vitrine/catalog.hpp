// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Station lookup in the catalogue for the web showcase (PUB-002, PUB-024).

#pragma once

#include <optional>
#include <string>

#include "owfax/net/json.hpp"
#include "owfax/vitrine/package.hpp"

namespace owfax::vitrine {

/// Station from one catalogue document, for the frequency `khz` (within 1 kHz of an
/// assigned frequency). Empty optional if the document is not a station or has no
/// such frequency.
std::optional<StationRef> station_from_json(const net::Json& doc, double khz);

/// Searches every *.json file of `catalog_dir` for a station transmitting on `khz`.
/// Empty optional and `error` set if none (or several) match.
std::optional<StationRef> find_station(const std::string& catalog_dir, double khz,
                                       std::string& error);

}  // namespace owfax::vitrine
