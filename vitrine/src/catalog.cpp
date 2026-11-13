// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/vitrine/catalog.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace owfax::vitrine {
namespace fs = std::filesystem;
using net::Json;

namespace {

constexpr double kMatchKhz = 1.0;

std::string text(const Json* j) {
    return j && j->is(Json::Type::kString) ? j->string : "";
}

std::optional<double> number(const Json* j) {
    if (j && j->is(Json::Type::kNumber)) return j->number;
    return std::nullopt;
}

}  // namespace

std::optional<StationRef> station_from_json(const Json& doc, double khz) {
    if (!doc.is(Json::Type::kObject)) return std::nullopt;
    const Json* pos = doc.get("position");
    const Json* wefax = doc.get("wefax");
    if (!pos || !wefax) return std::nullopt;
    const auto lat = number(pos->get("lat"));
    const auto lon = number(pos->get("lon"));
    const Json* freqs = wefax->get("frequencies");
    if (!lat || !lon || !freqs || !freqs->is(Json::Type::kArray)) return std::nullopt;

    const Json* match = nullptr;
    for (const Json& f : freqs->array) {
        const auto assigned = number(f.get("assigned_khz"));
        if (assigned && std::fabs(*assigned - khz) <= kMatchKhz) {
            match = &f;
            break;
        }
    }
    if (!match) return std::nullopt;

    StationRef s;
    s.id = text(doc.get("id"));
    s.name = text(doc.get("name"));
    s.callsign = text(match->get("callsign"));
    s.assigned_khz = *number(match->get("assigned_khz"));
    s.power_kw = number(match->get("power_kw"));
    s.position = {*lat, *lon};
    if (const auto shift = number(wefax->get("shift_hz"))) s.shift_hz = *shift;
    if (const Json* pub = doc.get("publication"); pub && pub->is(Json::Type::kObject)) {
        const Json* allowed = pub->get("allowed");
        s.publish_allowed = allowed && allowed->is(Json::Type::kBool) && allowed->boolean;
        s.attribution = text(pub->get("attribution"));
        s.terms_url = text(pub->get("terms_url"));
    }
    // An attribution is mandatory to publish (PUB-024).
    if (s.attribution.empty()) s.publish_allowed = false;
    return s;
}

std::optional<StationRef> find_station(const std::string& catalog_dir, double khz,
                                       std::string& error) {
    std::vector<StationRef> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(catalog_dir, ec)) {
        if (entry.path().extension() != ".json") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto doc = net::parse_json(ss.str());
        if (!doc) {
            error = "malformed catalogue file " + entry.path().filename().string();
            return std::nullopt;
        }
        if (auto s = station_from_json(*doc, khz)) found.push_back(std::move(*s));
    }
    if (ec) {
        error = "cannot read catalogue directory " + catalog_dir;
        return std::nullopt;
    }
    if (found.size() != 1) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.1f kHz", khz);
        error = found.empty() ? std::string("no station on ") + buf + " in the catalogue"
                              : std::string("several stations on ") + buf + " in the catalogue";
        return std::nullopt;
    }
    return found.front();
}

}  // namespace owfax::vitrine
