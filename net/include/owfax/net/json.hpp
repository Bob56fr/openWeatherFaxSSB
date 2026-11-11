// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Minimal JSON reader for request bodies and the settings file (RFC 8259): objects,
// arrays, strings (with \u escapes, UTF-8 out), numbers, booleans, null. Bounded
// nesting; anything malformed is rejected rather than guessed.

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace owfax::net {

struct Json {
    enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
    Type type = Type::kNull;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    bool is(Type t) const noexcept { return type == t; }
    /// Member of an object, or nullptr.
    const Json* get(const std::string& key) const;
};

/// Parses a complete JSON text. Empty optional if malformed.
std::optional<Json> parse_json(std::string_view text);

/// JSON string literal with escapes, quotes included.
std::string json_quote(std::string_view s);

}  // namespace owfax::net
