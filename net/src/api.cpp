// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/net/api.hpp"

#include <chrono>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

#include "owfax/build_info.hpp"
#include "owfax/engine/receiver.hpp"
#include "owfax/image/gray_image.hpp"
#include "owfax/net/json.hpp"

namespace owfax::net {
namespace {

namespace fs = std::filesystem;
using engine::json_escape;

constexpr std::size_t kDefaultChartLimit = 200;

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string trim_json(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

Response error(int status, const std::string& message) {
    return Response::json("{\"error\": \"" + json_escape(message) + "\"}\n", status);
}

/// "20261110T0735Z_…": only chart files are ever served from the charts directory
/// (never status.json, nor a settings file placed there by mistake).
bool is_chart_stem(const std::string& s) {
    if (s.size() < 16 || s[8] != 'T' || s[13] != 'Z' || s[14] != '_') return false;
    for (std::size_t i = 0; i < 13; ++i) {
        if (i != 8 && (s[i] < '0' || s[i] > '9')) return false;
    }
    return is_safe_name(s);
}

/// JSON files of a directory (names without extension), sorted.
std::vector<std::string> json_stems(const std::string& dir, bool charts_only) {
    std::vector<std::string> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const fs::path& p = e.path();
        if (p.extension() != ".json" || !e.is_regular_file(ec)) continue;
        const std::string stem = p.stem().string();
        if (stem == "status" || !is_safe_name(stem)) continue;
        if (charts_only && !is_chart_stem(stem)) continue;
        out.push_back(stem);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string array_of_files(const std::string& dir, const std::vector<std::string>& stems) {
    std::string body = "[";
    bool first = true;
    for (const auto& s : stems) {
        const auto text = read_file(fs::path(dir) / (s + ".json"));
        if (!text) continue;  // removed meanwhile
        body += first ? "\n" : ",\n";
        body += trim_json(*text);
        first = false;
    }
    body += first ? "]\n" : "\n]\n";
    return body;
}

std::string content_type(const fs::path& p) {
    const std::string ext = p.extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js") return "text/javascript; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

/// Value of `name` in a query string ("a=1&levels=16"), empty if absent.
std::string query_value(const std::string& query, const std::string& name) {
    std::istringstream in(query);
    std::string pair;
    while (std::getline(in, pair, '&')) {
        if (pair.rfind(name + "=", 0) == 0) return pair.substr(name.size() + 1);
    }
    return {};
}

std::size_t query_limit(const std::string& query) {
    std::istringstream in(query);
    std::string pair;
    while (std::getline(in, pair, '&')) {
        if (pair.rfind("limit=", 0) == 0) {
            const long v = std::strtol(pair.c_str() + 6, nullptr, 10);
            if (v > 0) return static_cast<std::size_t>(v);
        }
    }
    return kDefaultChartLimit;
}

std::string html_escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

}  // namespace

bool is_safe_name(const std::string& name) {
    if (name.empty() || name.size() > 200 || name[0] == '.') return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return name.find("..") == std::string::npos;
}

Api::Api(ApiConfig config) : config_(std::move(config)), status_("{\"state\": \"starting\"}") {}

HttpServer::Handler Api::handler() {
    return [this](const Request& r) { return handle(r); };
}

void Api::set_status(std::string json_object) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = std::move(json_object);
}

std::string Api::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Api::publish(const std::string& json_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (WebSocket* ws : clients_) ws->post(json_message);
}

std::size_t Api::live_clients() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
}

Response Api::handle(const Request& request) {
    const auto parts = split_path(request.path);
    const bool read = request.method == "GET" || request.method == "HEAD";
    if (parts.size() >= 2 && parts[0] == "api") {
        if (parts[1] != "v1") return error(404, "unknown API version (this server: v1)");
        // Endpoints that change something (skipper only, NFR-006).
        if (parts.size() == 3 && parts[2] == "settings") {
            if (read) return settings_get(request);
            if (request.method == "PUT") return settings_put(request);
            return error(405, "settings: GET or PUT");
        }
        if (parts.size() == 3 && parts[2] == "session") {
            if (request.method == "POST" || request.method == "DELETE") return session(request);
            return error(405, "session: POST (log in) or DELETE (log out)");
        }
        if (parts.size() == 3 && parts[2] == "password") {
            if (request.method == "POST") return password(request);
            return error(405, "password: POST");
        }
        if (!read) return error(405, "this endpoint is read only");
        if (parts.size() == 3 && parts[2] == "info") return info();
        if (parts.size() == 3 && parts[2] == "diagnostics") return diagnostics();
        if (parts.size() == 3 && parts[2] == "journal") return journal();
        if (parts.size() == 3 && parts[2] == "status") return Response::json(status() + "\n");
        if (parts.size() == 3 && parts[2] == "live") return live();
        if (parts.size() >= 3 && parts[2] == "charts") {
            if (parts.size() == 3) return charts(request);
            if (parts.size() == 4) return chart(parts[3], "", request.query);
            if (parts.size() == 5) return chart(parts[3], parts[4], request.query);
        }
        if (parts.size() >= 3 && parts[2] == "stations" && parts.size() <= 4) {
            return stations(parts.size() == 4 ? parts[3] : "");
        }
        return error(404, "no such API endpoint: " + request.path);
    }
    if (!read) return error(405, "the web interface is read only");
    return web(request.path);
}

// ------------------------------------------------- skipper settings (UI-002/004)

std::string Api::bearer(const Request& request) const {
    const std::string h = request.header("authorization");
    return h.rfind("Bearer ", 0) == 0 ? h.substr(7) : std::string();
}

Response Api::settings_reply(bool logged_in, int status) {
    const SettingsStore& st = *config_.settings;
    const StationSettings s = st.settings();
    const bool restart = !s.input.empty() && s.input != config_.running_input;
    return Response::json("{\n  \"version\": " + std::to_string(st.version()) +
                              ",\n  \"password_set\": " + (st.password_set() ? "true" : "false") +
                              ",\n  \"logged_in\": " + (logged_in ? "true" : "false") +
                              ",\n  \"restart_required\": " + (restart ? "true" : "false") +
                              ",\n  \"running_input\": " + json_quote(config_.running_input) +
                              ",\n  \"settings\": " + settings_json(s) + "\n}\n",
                          status);
}

Response Api::settings_get(const Request& request) {
    if (!config_.settings) return error(404, "settings are not editable (owfaxd without --config)");
    return settings_reply(sessions_.valid(bearer(request)));
}

Response Api::settings_put(const Request& request) {
    if (!config_.settings) return error(404, "settings are not editable (owfaxd without --config)");
    if (!sessions_.valid(bearer(request))) return error(401, "skipper login required");
    const auto body = parse_json(request.body);
    const Json* version = body ? body->get("version") : nullptr;
    const Json* values = body ? body->get("settings") : nullptr;
    if (!version || !version->is(Json::Type::kNumber) || !values) {
        return error(400, "expected {\"version\": n, \"settings\": {...}}");
    }
    std::string why;
    const auto s = settings_from_json(*values, config_.settings->settings(), why);
    if (!s) return error(400, why);
    if (!config_.settings->update(*s, static_cast<std::uint64_t>(version->number))) {
        // Someone else changed the settings meanwhile (UI-004): send the current ones.
        return settings_reply(true, 409);
    }
    if (config_.on_settings) config_.on_settings(*s);
    return settings_reply(true);
}

Response Api::session(const Request& request) {
    if (!config_.settings) return error(404, "settings are not editable (owfaxd without --config)");
    if (request.method == "DELETE") {
        sessions_.close(bearer(request));
        return Response::json("{\"logged_in\": false}\n");
    }
    if (!config_.settings->password_set()) {
        return error(409, "no skipper password yet: create one first");
    }
    if (const long wait = sessions_.locked_for(); wait > 0) {
        Response r = error(429, "too many failed attempts, wait " + std::to_string(wait) + " s");
        r.headers.emplace_back("Retry-After", std::to_string(wait));
        return r;
    }
    const auto body = parse_json(request.body);
    const Json* pw = body ? body->get("password") : nullptr;
    if (!pw || !pw->is(Json::Type::kString) || !config_.settings->check_password(pw->string)) {
        sessions_.failed();
        return error(401, "wrong password");
    }
    sessions_.succeeded();
    return Response::json("{\"token\": " + json_quote(sessions_.open()) + ", \"expires_in\": " +
                          std::to_string(std::chrono::seconds(Sessions::kLifetime).count()) +
                          "}\n");
}

Response Api::password(const Request& request) {
    if (!config_.settings) return error(404, "settings are not editable (owfaxd without --config)");
    const auto body = parse_json(request.body);
    const Json* fresh = body ? body->get("new") : nullptr;
    if (!fresh || !fresh->is(Json::Type::kString)) return error(400, "expected {\"new\": \"…\"}");
    std::string token = bearer(request);
    if (config_.settings->password_set()) {
        // Changing it: a skipper session and the current password.
        if (!sessions_.valid(token)) return error(401, "skipper login required");
        if (const long wait = sessions_.locked_for(); wait > 0) {
            return error(429, "too many failed attempts, wait " + std::to_string(wait) + " s");
        }
        const Json* current = body->get("current");
        if (!current || !current->is(Json::Type::kString) ||
            !config_.settings->check_password(current->string)) {
            sessions_.failed();
            return error(401, "wrong current password");
        }
    }
    // First time: anyone on the local network may create it (first setup, UI-003).
    try {
        config_.settings->set_password(fresh->string);
    } catch (const std::invalid_argument& e) {
        return error(400, e.what());
    }
    sessions_.succeeded();
    if (!sessions_.valid(token)) token = sessions_.open();
    sessions_.close_all_except(token);  // other screens must log in again
    return Response::json("{\"token\": " + json_quote(token) + ", \"expires_in\": " +
                          std::to_string(std::chrono::seconds(Sessions::kLifetime).count()) +
                          "}\n");
}

Response Api::info() const {
    const auto& b = build_info();
    std::string body = "{\n";
    body += "  \"name\": \"openWeatherFaxSSB\",\n";
    body += "  \"version\": \"" + json_escape(std::string(b.version)) + "\",\n";
    body += "  \"git_revision\": \"" + json_escape(std::string(b.git_revision)) + "\",\n";
    body += "  \"license\": \"" + json_escape(std::string(b.license)) + "\",\n";
    body += "  \"source_url\": \"" + json_escape(std::string(b.source_url)) + "\",\n";
    body += "  \"api_version\": 1,\n";
    body += "  \"receive_only\": true,\n";
    body += "  \"input\": \"" + json_escape(config_.input) + "\"\n";
    body += "}\n";
    return Response::json(body);
}

Response Api::diagnostics() const {
    if (!config_.diagnostics) return error(404, "no diagnostics from this engine");
    return Response::json(config_.diagnostics() + "\n");
}

Response Api::journal() const {
    constexpr std::uintmax_t kMaxBytes = 512 * 1024;  // the latest part only
    if (config_.journal_path.empty()) return error(404, "no journal (option --journal)");
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(config_.journal_path, ec);
    if (ec) return error(404, "journal not found");
    std::ifstream in(config_.journal_path, std::ios::binary);
    if (size > kMaxBytes) in.seekg(static_cast<std::streamoff>(size - kMaxBytes));
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (size > kMaxBytes) {
        const auto nl = text.find('\n');  // start on a whole line
        if (nl != std::string::npos) text.erase(0, nl + 1);
    }
    Response r;
    r.content_type = "text/plain; charset=utf-8";
    r.body = std::move(text);
    r.headers.emplace_back("Cache-Control", "no-store");
    r.headers.emplace_back("Content-Disposition", "attachment; filename=\"owfaxd-journal.txt\"");
    return r;
}

Response Api::charts(const Request& request) const {
    auto stems = json_stems(config_.charts_dir, true);
    std::reverse(stems.begin(), stems.end());  // stems start with the UTC time
    const std::size_t limit = query_limit(request.query);
    if (stems.size() > limit) stems.resize(limit);
    return Response::json(array_of_files(config_.charts_dir, stems));
}

Response Api::chart(const std::string& stem, const std::string& part,
                    const std::string& query) const {
    if (!is_chart_stem(stem)) return error(404, "no such chart");
    const fs::path dir(config_.charts_dir);
    if (part.empty()) {
        const auto text = read_file(dir / (stem + ".json"));
        if (!text) return error(404, "no such chart: " + stem);
        return Response::json(trim_json(*text) + "\n");
    }
    std::string suffix;
    if (part == "live.png") {
        suffix = "_live.png";
    } else if (part == "raw.png") {
        suffix = ".png";
    } else if (part == "enhanced.png") {
        suffix = "_enh.png";
    } else {
        return error(404, "chart images are live.png, raw.png and enhanced.png");
    }
    const std::string levels = query_value(query, "levels");
    if (!levels.empty() && levels != "16" && levels != "256") {
        return error(400, "levels: 16 (light display version) or 256 (default)");
    }
    const std::string width = query_value(query, "width");
    if (!width.empty() && width != "360" && width != "720") {
        return error(400, "width: 360 or 720 (thumbnails), or absent (full size)");
    }
    const fs::path source = dir / (stem + suffix);
    std::optional<std::string> data;
    if (levels == "16" || !width.empty()) {
        // Display versions: 16 grey levels (about 4x smaller, for the Wi-Fi of a boat)
        // and/or a thumbnail width. Made on the first request and kept next to the
        // original, remade when the original changes (live preview).
        std::string name = stem + suffix.substr(0, suffix.size() - 4);
        if (levels == "16") name += ".l16";
        if (!width.empty()) name += ".w" + width;
        const fs::path cached = dir / (name + ".png");
        std::error_code ec;
        const auto src_time = fs::last_write_time(source, ec);
        if (ec) return error(404, "no " + part + " for " + stem);
        const auto cache_time = fs::last_write_time(cached, ec);
        if (!ec && cache_time >= src_time) data = read_file(cached);
        if (!data) {
            const auto png = read_file(source);
            if (!png) return error(404, "no " + part + " for " + stem);
            auto img = decode_png(reinterpret_cast<const std::uint8_t*>(png->data()), png->size());
            if (!width.empty()) img = shrink_to_width(img, std::stoul(width));
            const auto light = encode_png(img, levels == "16" ? 4 : 8);
            data = std::string(light.begin(), light.end());
            fs::path tmp = cached;
            tmp +=
                ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out.write(data->data(), static_cast<std::streamsize>(data->size()));
            }
            fs::rename(tmp, cached, ec);
            if (ec) fs::remove(tmp, ec);
        }
    } else {
        data = read_file(source);
    }
    if (!data) return error(404, "no " + part + " for " + stem);
    Response r;
    r.content_type = "image/png";
    r.body = *data;
    // The live preview is rewritten during reception; final images never change.
    r.headers.emplace_back("Cache-Control", part == "live.png" ? "no-store" : "max-age=3600");
    return r;
}

Response Api::stations(const std::string& id) const {
    if (config_.catalog_dir.empty()) return error(404, "no station catalogue configured");
    if (id.empty()) {
        return Response::json(
            array_of_files(config_.catalog_dir, json_stems(config_.catalog_dir, false)));
    }
    if (!is_safe_name(id)) return error(404, "no such station");
    const auto text = read_file(fs::path(config_.catalog_dir) / (id + ".json"));
    if (!text) return error(404, "no such station: " + id);
    return Response::json(trim_json(*text) + "\n");
}

Response Api::live() {
    Response r;
    r.websocket = [this](WebSocket& ws) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clients_.insert(&ws);
            ws.post("{\"type\": \"status\", \"status\": " + status_ + "}");
        }
        ws.run();
        std::lock_guard<std::mutex> lock(mutex_);
        clients_.erase(&ws);
    };
    return r;
}

Response Api::web(const std::string& path) const {
    if (config_.web_dir.empty()) {
        if (path != "/" && path != "/index.html") return error(404, "not found: " + path);
        const auto& b = build_info();
        const std::string version = html_escape(std::string(b.version));
        const std::string source = html_escape(std::string(b.source_url));
        Response r;
        r.content_type = "text/html; charset=utf-8";
        r.body =
            "<!doctype html><html lang=\"fr\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<title>openWeatherFaxSSB</title></head><body>"
            "<h1>openWeatherFaxSSB " +
            version +
            "</h1><p>Moteur de réception en marche. Interface web : à venir. "
            "API locale :</p><ul>"
            "<li><a href=\"/api/v1/status\">/api/v1/status</a></li>"
            "<li><a href=\"/api/v1/charts\">/api/v1/charts</a></li>"
            "<li><a href=\"/api/v1/stations\">/api/v1/stations</a></li>"
            "<li><a href=\"/api/v1/info\">/api/v1/info</a></li></ul>"
            "<p>Logiciel libre AGPL-3.0 — <a href=\"" +
            source + "\">code source</a> (" + html_escape(std::string(b.git_revision)) +
            ").</p></body></html>\n";
        return r;
    }
    std::string rel;
    for (const auto& p : split_path(path)) {
        // Backslashes and drive letters would be path syntax on Windows.
        if (p[0] == '.' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos) {
            return error(404, "not found");
        }
        rel += "/" + p;
    }
    fs::path file = fs::path(config_.web_dir) / (rel.empty() ? "index.html" : rel.substr(1));
    std::error_code ec;
    if (fs::is_directory(file, ec)) file /= "index.html";
    const auto data = read_file(file);
    if (!data) return error(404, "not found: " + path);
    Response r;
    r.content_type = content_type(file);
    r.body = *data;
    r.headers.emplace_back("Cache-Control", "no-cache");
    return r;
}

}  // namespace owfax::net
