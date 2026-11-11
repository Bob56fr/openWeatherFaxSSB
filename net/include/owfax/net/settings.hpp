// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Station settings changed from the web interface by the skipper (spec UI-002,
// UI-004, ARC-004), and the skipper password (NFR-006).
//
// File (JSON, written atomically, owner-only on POSIX):
//   {"version": 3, "settings": {...}, "password": {"algorithm": "pbkdf2-sha256",
//    "iterations": 100000, "salt": "<hex>", "hash": "<hex>"}}
// The password itself is never stored or logged. The version number increases at
// every change: a client must send the version it edited, so that two skippers
// editing at the same time never overwrite each other silently (UI-004).

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "owfax/net/json.hpp"

namespace owfax::net {

struct StationSettings {
    std::string label = "wefax";    ///< Label in chart file names.
    double shift_hz = 800.0;        ///< Black-white shift of the station (DWD: 850).
    bool start_without_apt = true;  ///< Start on a fax signal whose APT was missed.
    bool enhanced = true;           ///< Also write the enhanced image.
    std::string input;              ///< Audio input URI (applied at the next start).
    std::string station;            ///< Station followed (catalogue id), set by the assistant.

    bool operator==(const StationSettings& o) const;
};

/// JSON object of the settings.
std::string settings_json(const StationSettings& s);

/// Reads and validates settings from a JSON object; missing members keep the values
/// of `base`. Error message (for the user) on invalid input.
std::optional<StationSettings> settings_from_json(const Json& j, const StationSettings& base,
                                                  std::string& error);

class SettingsStore {
public:
    static constexpr std::uint32_t kIterations = 100000;
    static constexpr std::size_t kMinPasswordLength = 8;

    /// `path` empty: kept in memory only.
    explicit SettingsStore(std::string path, StationSettings defaults = {});

    /// Reads the file if it exists. False (and `error`) if it is malformed; the
    /// defaults are kept then.
    bool load(std::string& error);

    StationSettings settings() const;
    std::uint64_t version() const;
    bool password_set() const;
    bool check_password(const std::string& password) const;
    /// Throws std::invalid_argument if too short.
    void set_password(const std::string& password);
    /// Replaces the settings if `expected_version` is the current one. False on a
    /// version conflict (someone else changed them meanwhile).
    bool update(const StationSettings& s, std::uint64_t expected_version);

private:
    void save_locked() const;

    std::string path_;
    mutable std::mutex mutex_;
    StationSettings settings_;
    std::uint64_t version_ = 1;
    std::string salt_hex_;
    std::string hash_hex_;
    std::uint32_t iterations_ = kIterations;
};

/// Skipper sessions and brute-force protection.
class Sessions {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::hours kLifetime{12};
    static constexpr int kMaxFailures = 5;
    static constexpr std::chrono::seconds kLockout{30};

    /// New session token (64 hex digits).
    std::string open();
    bool valid(const std::string& token);
    void close(const std::string& token);
    void close_all_except(const std::string& token);

    /// Seconds to wait before the next login attempt (0 = allowed).
    long locked_for();
    void failed();
    void succeeded();

private:
    std::mutex mutex_;
    std::map<std::string, Clock::time_point> sessions_;
    int failures_ = 0;
    Clock::time_point locked_until_{};
};

}  // namespace owfax::net
