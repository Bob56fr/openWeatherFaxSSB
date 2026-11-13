// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/vitrine/geo.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace owfax::vitrine {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthKm = 6371.0;
constexpr double kKmPerNm = 1.852;

double rad(double deg) {
    return deg * kPi / 180.0;
}
double deg(double rad) {
    return rad * 180.0 / kPi;
}

}  // namespace

std::optional<GeoPoint> from_locator(std::string_view loc) {
    if (loc.size() != 4 && loc.size() != 6 && loc.size() != 8) return std::nullopt;
    auto up = [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); };
    const char f0 = up(loc[0]), f1 = up(loc[1]);
    if (f0 < 'A' || f0 > 'R' || f1 < 'A' || f1 > 'R') return std::nullopt;
    if (!std::isdigit(static_cast<unsigned char>(loc[2])) ||
        !std::isdigit(static_cast<unsigned char>(loc[3]))) {
        return std::nullopt;
    }
    // South-west corner, refined pair by pair; then the centre of the last square.
    double lon = (f0 - 'A') * 20.0 - 180.0 + (loc[2] - '0') * 2.0;
    double lat = (f1 - 'A') * 10.0 - 90.0 + (loc[3] - '0') * 1.0;
    double w = 2.0, h = 1.0;
    if (loc.size() >= 6) {
        const char s0 = up(loc[4]), s1 = up(loc[5]);
        if (s0 < 'A' || s0 > 'X' || s1 < 'A' || s1 > 'X') return std::nullopt;
        w /= 24.0;
        h /= 24.0;
        lon += (s0 - 'A') * w;
        lat += (s1 - 'A') * h;
    }
    if (loc.size() == 8) {
        if (!std::isdigit(static_cast<unsigned char>(loc[6])) ||
            !std::isdigit(static_cast<unsigned char>(loc[7]))) {
            return std::nullopt;
        }
        w /= 10.0;
        h /= 10.0;
        lon += (loc[6] - '0') * w;
        lat += (loc[7] - '0') * h;
    }
    return GeoPoint{lat + h / 2.0, lon + w / 2.0};
}

std::string to_locator(const GeoPoint& p, int chars) {
    double lon = std::clamp(p.lon_deg + 180.0, 0.0, 359.999999);
    double lat = std::clamp(p.lat_deg + 90.0, 0.0, 179.999999);
    std::string s;
    s += static_cast<char>('A' + static_cast<int>(lon / 20.0));
    s += static_cast<char>('A' + static_cast<int>(lat / 10.0));
    lon = std::fmod(lon, 20.0);
    lat = std::fmod(lat, 10.0);
    s += static_cast<char>('0' + static_cast<int>(lon / 2.0));
    s += static_cast<char>('0' + static_cast<int>(lat / 1.0));
    if (chars >= 6) {
        lon = std::fmod(lon, 2.0);
        lat = std::fmod(lat, 1.0);
        const double w = 2.0 / 24.0, h = 1.0 / 24.0;
        s += static_cast<char>('a' + static_cast<int>(lon / w));
        s += static_cast<char>('a' + static_cast<int>(lat / h));
        if (chars >= 8) {
            lon = std::fmod(lon, w);
            lat = std::fmod(lat, h);
            s += static_cast<char>('0' + static_cast<int>(lon / (w / 10.0)));
            s += static_cast<char>('0' + static_cast<int>(lat / (h / 10.0)));
        }
    }
    return s;
}

GreatCircle great_circle(const GeoPoint& a, const GeoPoint& b) {
    const double p1 = rad(a.lat_deg), p2 = rad(b.lat_deg);
    const double dp = p2 - p1, dl = rad(b.lon_deg - a.lon_deg);
    const double h = std::sin(dp / 2) * std::sin(dp / 2) +
                     std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    GreatCircle g;
    g.distance_km = 2.0 * kEarthKm * std::asin(std::min(1.0, std::sqrt(h)));
    g.distance_nm = g.distance_km / kKmPerNm;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    g.azimuth_deg = std::fmod(deg(std::atan2(y, x)) + 360.0, 360.0);
    return g;
}

double sun_elevation_deg(const GeoPoint& p, std::int64_t unix_time) {
    // NOAA solar position, simplified (fractional year in radians).
    const double days = static_cast<double>(unix_time) / 86400.0;
    // Day of year counted from 1 January 1970 modulo the tropical year: within a day of
    // the calendar day of year, well inside the precision needed here.
    const double day_of_year = std::floor(std::fmod(days, 365.2422));
    const double hour = std::fmod(static_cast<double>(unix_time % 86400 + 86400), 86400.0) / 3600.0;
    const double g = 2.0 * kPi / 365.0 * (day_of_year + (hour - 12.0) / 24.0);
    // Equation of time (minutes) and declination (radians).
    const double eqtime = 229.18 * (0.000075 + 0.001868 * std::cos(g) - 0.032077 * std::sin(g) -
                                    0.014615 * std::cos(2 * g) - 0.040849 * std::sin(2 * g));
    const double decl = 0.006918 - 0.399912 * std::cos(g) + 0.070257 * std::sin(g) -
                        0.006758 * std::cos(2 * g) + 0.000907 * std::sin(2 * g) -
                        0.002697 * std::cos(3 * g) + 0.00148 * std::sin(3 * g);
    const double true_solar_min = hour * 60.0 + eqtime + 4.0 * p.lon_deg;
    const double ha = rad(true_solar_min / 4.0 - 180.0);
    const double lat = rad(p.lat_deg);
    const double cos_zenith =
        std::sin(lat) * std::sin(decl) + std::cos(lat) * std::cos(decl) * std::cos(ha);
    return 90.0 - deg(std::acos(std::clamp(cos_zenith, -1.0, 1.0)));
}

std::string path_daylight(const GeoPoint& a, const GeoPoint& b, std::int64_t unix_time) {
    const double ea = sun_elevation_deg(a, unix_time);
    const double eb = sun_elevation_deg(b, unix_time);
    if (ea > 0.0 && eb > 0.0) return "day";
    if (ea < -6.0 && eb < -6.0) return "night";
    return "mixed";
}

}  // namespace owfax::vitrine
