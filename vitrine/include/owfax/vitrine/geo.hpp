// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Geography for the web showcase (docs/VITRINE-WEB.md, PUB-001 … PUB-004): Maidenhead
// locators, great-circle distance and azimuth from the receiving station to the
// transmitter, and day/night on the path.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace owfax::vitrine {

struct GeoPoint {
    double lat_deg = 0.0;  ///< North positive.
    double lon_deg = 0.0;  ///< East positive.
};

/// Centre of a Maidenhead locator square of 4, 6 or 8 characters ("JN18", "IN88gs",
/// "IN88gs52"), case-insensitive. Empty optional if malformed.
std::optional<GeoPoint> from_locator(std::string_view locator);

/// Maidenhead locator of a point, with 4, 6 or 8 characters (subsquare in lower case).
std::string to_locator(const GeoPoint& p, int chars = 6);

struct GreatCircle {
    double distance_km = 0.0;
    double distance_nm = 0.0;  ///< Nautical miles.
    double azimuth_deg = 0.0;  ///< Initial bearing from `from`, 0 … 360, clockwise from north.
};

/// Great-circle distance and initial bearing on a spherical Earth (R = 6371 km).
GreatCircle great_circle(const GeoPoint& from, const GeoPoint& to);

/// Sun elevation above the horizon (degrees) at a point and a Unix time
/// (NOAA approximation, ±0.5°).
double sun_elevation_deg(const GeoPoint& p, std::int64_t unix_time);

/// "day" when the sun is up at both ends of the path, "night" when it is more than
/// 6° below the horizon at both ends (civil twilight), "mixed" otherwise (greyline).
std::string path_daylight(const GeoPoint& a, const GeoPoint& b, std::int64_t unix_time);

}  // namespace owfax::vitrine
