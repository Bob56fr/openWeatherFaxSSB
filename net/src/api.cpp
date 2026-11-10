// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/net/api.hpp"

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

/// JSON files of a directory (names without extension), sorted.
std::vector<std::string> json_stems(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const fs::path& p = e.path();
        if (p.extension() != ".json" || !e.is_regular_file(ec)) continue;
        const std::string stem = p.stem().string();
        if (stem == "status" || !is_safe_name(stem)) continue;
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
    if (parts.size() >= 2 && parts[0] == "api") {
        if (parts[1] != "v1") return error(404, "unknown API version (this server: v1)");
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
    return web(request.path);
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
    auto stems = json_stems(config_.charts_dir);
    std::reverse(stems.begin(), stems.end());  // stems start with the UTC time
    const std::size_t limit = query_limit(request.query);
    if (stems.size() > limit) stems.resize(limit);
    return Response::json(array_of_files(config_.charts_dir, stems));
}

Response Api::chart(const std::string& stem, const std::string& part,
                    const std::string& query) const {
    if (!is_safe_name(stem) || stem == "status") return error(404, "no such chart");
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
        return Response::json(array_of_files(config_.catalog_dir, json_stems(config_.catalog_dir)));
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
