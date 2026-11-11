// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/net/settings.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "owfax/net/crypto.hpp"

namespace owfax::net {
namespace {

namespace fs = std::filesystem;

std::string number_text(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", v);
    return buf;
}

bool label_ok(const std::string& s) {
    if (s.empty() || s.size() > 40) return false;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

std::string hash_password(const std::string& password, const std::string& salt_hex,
                          std::uint32_t iterations) {
    const auto salt = from_hex(salt_hex);
    const auto key = pbkdf2_sha256(
        password, std::string_view(reinterpret_cast<const char*>(salt.data()), salt.size()),
        iterations, 32);
    return to_hex(key.data(), key.size());
}

}  // namespace

bool StationSettings::operator==(const StationSettings& o) const {
    return label == o.label && shift_hz == o.shift_hz && start_without_apt == o.start_without_apt &&
           enhanced == o.enhanced && input == o.input && station == o.station;
}

std::string settings_json(const StationSettings& s) {
    return "{\"label\": " + json_quote(s.label) + ", \"shift_hz\": " + number_text(s.shift_hz) +
           ", \"start_without_apt\": " + (s.start_without_apt ? "true" : "false") +
           ", \"enhanced\": " + (s.enhanced ? "true" : "false") +
           ", \"input\": " + json_quote(s.input) + ", \"station\": " + json_quote(s.station) + "}";
}

std::optional<StationSettings> settings_from_json(const Json& j, const StationSettings& base,
                                                  std::string& error) {
    if (!j.is(Json::Type::kObject)) {
        error = "settings must be a JSON object";
        return std::nullopt;
    }
    StationSettings s = base;
    if (const Json* v = j.get("label")) {
        if (!v->is(Json::Type::kString) || !label_ok(v->string)) {
            error = "label: 1 to 40 letters, digits, '_', '-' or '.'";
            return std::nullopt;
        }
        s.label = v->string;
    }
    if (const Json* v = j.get("shift_hz")) {
        if (!v->is(Json::Type::kNumber) || !(v->number >= 200.0 && v->number <= 1200.0)) {
            error = "shift_hz: 200 to 1200 Hz (DWD 850, most stations 800)";
            return std::nullopt;
        }
        s.shift_hz = std::round(v->number);
    }
    if (const Json* v = j.get("start_without_apt")) {
        if (!v->is(Json::Type::kBool)) {
            error = "start_without_apt: true or false";
            return std::nullopt;
        }
        s.start_without_apt = v->boolean;
    }
    if (const Json* v = j.get("enhanced")) {
        if (!v->is(Json::Type::kBool)) {
            error = "enhanced: true or false";
            return std::nullopt;
        }
        s.enhanced = v->boolean;
    }
    if (const Json* v = j.get("input")) {
        if (!v->is(Json::Type::kString) || v->string.size() > 300) {
            error = "input: an audio input URI (wav:…, stdin, alsa:…)";
            return std::nullopt;
        }
        s.input = v->string;
    }
    if (const Json* v = j.get("station")) {
        // Catalogue id ("dwd-pinneberg"), or empty for none.
        if (!v->is(Json::Type::kString) || (!v->string.empty() && !label_ok(v->string))) {
            error = "station: a catalogue id such as dwd-pinneberg";
            return std::nullopt;
        }
        s.station = v->string;
    }
    return s;
}

// ---------------------------------------------------------------------------------

SettingsStore::SettingsStore(std::string path, StationSettings defaults)
    : path_(std::move(path)), settings_(std::move(defaults)) {}

bool SettingsStore::load(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (path_.empty()) return true;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return true;  // first start: created at the first change
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto j = parse_json(text);
    if (!j || !j->is(Json::Type::kObject)) {
        error = path_ + ": not valid JSON";
        return false;
    }
    StationSettings s = settings_;
    if (const Json* st = j->get("settings")) {
        const auto parsed = settings_from_json(*st, settings_, error);
        if (!parsed) {
            error = path_ + ": " + error;
            return false;
        }
        s = *parsed;
    }
    if (const Json* v = j->get("version"); v && v->is(Json::Type::kNumber) && v->number >= 1) {
        version_ = static_cast<std::uint64_t>(v->number);
    }
    if (const Json* p = j->get("password"); p && p->is(Json::Type::kObject)) {
        const Json* salt = p->get("salt");
        const Json* hash = p->get("hash");
        const Json* it = p->get("iterations");
        if (!salt || !hash || !it || !salt->is(Json::Type::kString) ||
            !hash->is(Json::Type::kString) || !it->is(Json::Type::kNumber) || it->number < 1 ||
            from_hex(salt->string).empty() || from_hex(hash->string).size() != 32) {
            error = path_ + ": malformed password entry";
            return false;
        }
        salt_hex_ = salt->string;
        hash_hex_ = hash->string;
        iterations_ = static_cast<std::uint32_t>(it->number);
    }
    settings_ = s;
    return true;
}

StationSettings SettingsStore::settings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

std::uint64_t SettingsStore::version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

bool SettingsStore::password_set() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !hash_hex_.empty();
}

bool SettingsStore::check_password(const std::string& password) const {
    std::string salt;
    std::string hash;
    std::uint32_t iterations = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        salt = salt_hex_;
        hash = hash_hex_;
        iterations = iterations_;
    }
    if (hash.empty()) return false;
    return constant_time_equal(hash_password(password, salt, iterations), hash);
}

void SettingsStore::set_password(const std::string& password) {
    if (password.size() < kMinPasswordLength) {
        throw std::invalid_argument("the password needs at least 8 characters");
    }
    const auto salt = secure_random(16);
    const std::string salt_hex = to_hex(salt.data(), salt.size());
    const std::string hash = hash_password(password, salt_hex, kIterations);
    std::lock_guard<std::mutex> lock(mutex_);
    salt_hex_ = salt_hex;
    hash_hex_ = hash;
    iterations_ = kIterations;
    save_locked();
}

bool SettingsStore::update(const StationSettings& s, std::uint64_t expected_version) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expected_version != version_) return false;
    settings_ = s;
    ++version_;
    save_locked();
    return true;
}

void SettingsStore::save_locked() const {
    if (path_.empty()) return;
    std::string text = "{\n  \"version\": " + std::to_string(version_) +
                       ",\n  \"settings\": " + settings_json(settings_);
    if (!hash_hex_.empty()) {
        text += ",\n  \"password\": {\"algorithm\": \"pbkdf2-sha256\", \"iterations\": " +
                std::to_string(iterations_) + ", \"salt\": " + json_quote(salt_hex_) +
                ", \"hash\": " + json_quote(hash_hex_) + "}";
    }
    text += "\n}\n";
    const fs::path path(path_);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out << text;
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    std::error_code ec;
    // Owner only (the hash must not be readable by other accounts). No-op on Windows.
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace,
                    ec);
    fs::rename(tmp, path, ec);
    if (ec) throw std::runtime_error("cannot replace " + path.string() + ": " + ec.message());
}

// ---------------------------------------------------------------------------------

std::string Sessions::open() {
    const auto bytes = secure_random(32);
    const std::string token = to_hex(bytes.data(), bytes.size());
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        it = it->second < now ? sessions_.erase(it) : std::next(it);
    }
    if (sessions_.size() >= 32) sessions_.erase(sessions_.begin());  // bounded
    sessions_[token] = now + kLifetime;
    return token;
}

bool Sessions::valid(const std::string& token) {
    if (token.size() != 64) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    bool ok = false;
    const auto now = Clock::now();
    for (const auto& [t, expiry] : sessions_) {
        if (constant_time_equal(t, token) && expiry > now) ok = true;
    }
    return ok;
}

void Sessions::close(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(token);
}

void Sessions::close_all_except(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        it = it->first == token ? std::next(it) : sessions_.erase(it);
    }
}

long Sessions::locked_for() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    if (now >= locked_until_) return 0;
    return static_cast<long>(
               std::chrono::duration_cast<std::chrono::seconds>(locked_until_ - now).count()) +
           1;
}

void Sessions::failed() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (++failures_ >= kMaxFailures) {
        failures_ = 0;
        locked_until_ = Clock::now() + kLockout;
    }
}

void Sessions::succeeded() {
    std::lock_guard<std::mutex> lock(mutex_);
    failures_ = 0;
}

}  // namespace owfax::net
