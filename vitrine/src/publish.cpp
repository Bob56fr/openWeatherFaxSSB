// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/vitrine/publish.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

#include "owfax/net/json.hpp"

namespace fs = std::filesystem;

namespace owfax::vitrine {

namespace {

using net::Json;

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Written next to its place, then renamed: never half a file after a power cut.
void write_text(const fs::path& p, const std::string& text) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out << text;
        out.flush();
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    fs::rename(tmp, p);
}

void append_line(const fs::path& p, const std::string& line) {
    std::ofstream out(p, std::ios::binary | std::ios::app);
    out << line << '\n';
}

std::int64_t number_of(const Json& j, const char* key) {
    const Json* v = j.get(key);
    return v && v->is(Json::Type::kNumber) ? static_cast<std::int64_t>(v->number) : 0;
}

std::string string_of(const Json& j, const char* key) {
    const Json* v = j.get(key);
    return v && v->is(Json::Type::kString) ? v->string : std::string();
}

bool is_chart_id(const std::string& s) {
    static const std::regex id(R"([0-9]{8}T[0-9]{4}Z_[A-Za-z0-9-]+_[0-9.]+kHz)");
    return std::regex_match(s, id);
}

/// One line, at most 300 characters: answers go to the journal.
std::string one_line(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    if (s.size() > 300) s = s.substr(0, 297) + "...";
    return s;
}

QueueItem load_item(const fs::path& dir) {
    QueueItem item;
    item.chart_id = dir.filename().string();
    item.dir = dir;
    if (const auto j = net::parse_json(read_text(dir / "item.json"))) {
        item.enqueued_unix = number_of(*j, "enqueued_unix");
        item.attempts = static_cast<int>(number_of(*j, "attempts"));
        item.last_error = string_of(*j, "last_error");
    }
    return item;
}

std::vector<QueueItem> items_in(const fs::path& dir) {
    std::vector<QueueItem> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (is_chart_id(name) && it->is_directory()) out.push_back(load_item(it->path()));
    }
    std::sort(out.begin(), out.end(),
              [](const QueueItem& a, const QueueItem& b) { return a.chart_id < b.chart_id; });
    return out;
}

void remove_quietly(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

}  // namespace

std::int64_t retry_delay_s(int failures, const QueuePolicy& policy) {
    if (failures <= 0) return 0;
    std::int64_t d = policy.first_delay_s;
    for (int i = 1; i < failures && d < policy.max_delay_s; ++i) d *= 2;
    return std::min(d, policy.max_delay_s);
}

std::string utc_text(std::int64_t t) {
    const std::int64_t days = (t >= 0 ? t : t - 86399) / 86400;
    const std::int64_t secs = t - days * 86400;
    // Civil date from days since 1970-01-01 (H. Hinnant).
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const auto y = static_cast<long long>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
    char buf[96];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02d:%02d:%02dZ", y, m, d,
                  static_cast<int>(secs / 3600), static_cast<int>(secs / 60 % 60),
                  static_cast<int>(secs % 60));
    return buf;
}

const std::vector<PackageFile>& package_files() {
    static const std::vector<PackageFile> files = {
        {"meta", "meta.json", true},   {"telemetry", "telemetry.json", true},
        {"raw", "raw.webp", true},     {"enhanced", "enh.webp", false},
        {"thumb", "thumb.webp", true}, {"audio", "audio.m4a", true},
    };
    return files;
}

SendResult classify_answer(int http, const std::string& body, const std::string& transport_error) {
    SendResult r;
    r.http = http;
    if (http == 0 || !transport_error.empty()) {
        r.status = SendStatus::kRetry;
        r.detail = "no answer from the site: " +
                   one_line(transport_error.empty() ? "connection failed" : transport_error);
        return r;
    }
    const auto j = net::parse_json(body);
    const std::string code = j ? string_of(*j, "code") : "";
    const std::string message = j ? string_of(*j, "message") : "";
    const std::string why =
        one_line(code.empty() ? "HTTP " + std::to_string(http)
                              : "HTTP " + std::to_string(http) + " " + code + ": " + message);
    if (http == 200 || http == 201) {
        const std::string result = j ? string_of(*j, "result") : "";
        if (result == "created" || result == "exists") {
            r.status = SendStatus::kSent;
            r.detail = result;
            r.url = j ? string_of(*j, "url") : "";
            return r;
        }
        // A 200 that is not the plugin's answer: wrong address, captive portal, cache.
        r.status = SendStatus::kRetry;
        r.detail = "HTTP " + std::to_string(http) + " but not the plugin's answer (site address?)";
        return r;
    }
    switch (http) {
        case 400:  // missing or unexpected file
        case 413:  // a file over its size limit
        case 415:  // not the type it claims
        case 422:  // meta.json or telemetry.json refused
            r.status = SendStatus::kRefused;
            r.detail = why;
            return r;
        case 401:
        case 403:
            r.status = SendStatus::kRetry;
            r.detail = why + " (check the account and its application password)";
            return r;
        case 404:
            r.status = SendStatus::kRetry;
            r.detail = why + " (is the owfax-vitrine plugin active?)";
            return r;
        case 301:
        case 302:
        case 307:
        case 308:
            r.status = SendStatus::kRetry;
            r.detail = why + " (the site redirects: give its final address, https://...)";
            return r;
        default:  // 408, 429 rate limit, 5xx, 507 quota full of charts to keep …
            r.status = SendStatus::kRetry;
            r.detail = why;
            return r;
    }
}

PublishQueue::PublishQueue(fs::path root, QueuePolicy policy)
    : root_(std::move(root)), policy_(policy) {
    fs::create_directories(root_ / "pending");
    fs::create_directories(root_ / "refused");
}

std::string PublishQueue::log(std::int64_t now, const std::string& line) const {
    const std::string text = utc_text(now) + "  " + line;
    append_line(root_ / "journal.log", text);
    if (on_log) on_log(text);
    return text;
}

int PublishQueue::failures() const {
    const auto j = net::parse_json(read_text(root_ / "state.json"));
    return j ? static_cast<int>(number_of(*j, "failures")) : 0;
}

std::int64_t PublishQueue::next_try_unix() const {
    const auto j = net::parse_json(read_text(root_ / "state.json"));
    return j ? number_of(*j, "next_try_unix") : 0;
}

void PublishQueue::save_state(int failures, std::int64_t next_try,
                              const std::string& last_error) const {
    write_text(root_ / "state.json", "{\"failures\": " + std::to_string(failures) +
                                         ", \"next_try_unix\": " + std::to_string(next_try) +
                                         ", \"last_error\": " + net::json_quote(last_error) +
                                         "}\n");
}

void PublishQueue::save_item(const QueueItem& item) const {
    write_text(item.dir / "item.json",
               "{\"chart_id\": " + net::json_quote(item.chart_id) +
                   ", \"enqueued_unix\": " + std::to_string(item.enqueued_unix) +
                   ", \"attempts\": " + std::to_string(item.attempts) +
                   ", \"last_error\": " + net::json_quote(item.last_error) + "}\n");
}

bool PublishQueue::was_sent(const std::string& chart_id) const {
    std::ifstream in(root_ / "sent.log", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const auto a = line.find('\t');
        if (a == std::string::npos) continue;
        const auto b = line.find('\t', a + 1);
        if (line.compare(a + 1, b == std::string::npos ? std::string::npos : b - a - 1, chart_id) ==
            0) {
            return true;
        }
    }
    return false;
}

std::vector<QueueItem> PublishQueue::pending() const {
    return items_in(root_ / "pending");
}

std::vector<QueueItem> PublishQueue::refused() const {
    return items_in(root_ / "refused");
}

std::string PublishQueue::enqueue(const fs::path& package, std::int64_t now, std::string& error) {
    fs::path src = package;
    if (src.filename().empty()) src = src.parent_path();  // "dir/"
    const std::string id = src.filename().string();
    error.clear();
    if (!is_chart_id(id)) {
        error = id + ": not a chart package (directory named <time>_<station>_<kHz>kHz)";
        return "";
    }
    const auto meta = net::parse_json(read_text(src / "meta.json"));
    if (!meta) {
        error = id + ": no readable meta.json";
        return "";
    }
    if (string_of(*meta, "chart_id") != id) {
        error = id + ": meta.json is for another chart";
        return "";
    }
    const Json* sel = meta->get("selection");
    const Json* pub = sel ? sel->get("publish") : nullptr;
    if (!pub || !pub->is(Json::Type::kBool) || !pub->boolean) {
        error = id + ": not publishable (selection rules, PUB-020 to PUB-026)";
        return "";
    }
    for (const auto& f : package_files()) {
        std::error_code ec;
        if (f.required && !fs::is_regular_file(src / f.name, ec)) {
            error = id + ": " + f.name + " missing (packages made with --no-encode are not sent)";
            return "";
        }
    }
    if (was_sent(id)) return "already sent";
    const fs::path dest = root_ / "pending" / id;
    if (fs::exists(dest)) return "already queued";

    const fs::path tmp = root_ / "pending" / (".incoming-" + id);
    remove_quietly(tmp);
    fs::create_directories(tmp);
    for (const auto& f : package_files()) {
        std::error_code ec;
        if (fs::is_regular_file(src / f.name, ec)) fs::copy_file(src / f.name, tmp / f.name);
    }
    QueueItem item;
    item.chart_id = id;
    item.dir = tmp;
    item.enqueued_unix = now;
    save_item(item);
    fs::rename(tmp, dest);
    remove_quietly(root_ / "refused" / id);  // queued again by hand after a refusal
    log(now, id + ": queued");
    return "queued";
}

RoundReport PublishQueue::run(const Clock& now, const Sender& send, bool force) {
    RoundReport rep;
    std::int64_t t = now();
    int fails = failures();
    std::int64_t next = next_try_unix();
    std::string last_error;
    if (const auto j = net::parse_json(read_text(root_ / "state.json"))) {
        last_error = string_of(*j, "last_error");
    }

    std::vector<QueueItem> items;
    for (auto& item : pending()) {
        if (was_sent(item.chart_id)) {  // sent, then stopped before it was removed
            remove_quietly(item.dir);
            continue;
        }
        if (t - item.enqueued_unix >= policy_.give_up_after_s) {
            remove_quietly(item.dir);
            log(t, item.chart_id + ": dropped, not sent after " +
                       std::to_string(policy_.give_up_after_s / 3600) + " h in the queue (" +
                       std::to_string(item.attempts) + " tries; last: " +
                       (item.last_error.empty() ? "none" : item.last_error) + ")");
            ++rep.dropped;
            continue;
        }
        items.push_back(std::move(item));
    }
    if (items.empty()) {
        if (fails != 0 || next != 0) save_state(0, 0, last_error);
        return rep;
    }
    if (!force && t < next) {
        rep.waited = true;
        rep.next_try_unix = next;
        return rep;
    }

    for (auto& item : items) {
        const SendResult r = send(item);
        t = now();
        ++item.attempts;
        if (r.status == SendStatus::kSent) {
            append_line(root_ / "sent.log",
                        utc_text(t) + "\t" + item.chart_id + "\t" + r.detail + "\t" + r.url);
            remove_quietly(item.dir);
            log(t, item.chart_id + ": sent (" + r.detail + ", try " +
                       std::to_string(item.attempts) + ")" + (r.url.empty() ? "" : " " + r.url));
            fails = 0;
            next = 0;
            ++rep.sent;
        } else if (r.status == SendStatus::kRefused) {
            item.last_error = r.detail;
            save_item(item);
            const fs::path dest = root_ / "refused" / item.chart_id;
            remove_quietly(dest);
            fs::rename(item.dir, dest);
            log(t, item.chart_id + ": refused by the site, kept in refused/: " + r.detail);
            fails = 0;
            next = 0;
            ++rep.refused;
        } else {
            item.last_error = r.detail;
            save_item(item);
            ++fails;
            next = t + retry_delay_s(fails, policy_);
            last_error = r.detail;
            log(t, item.chart_id + ": not sent, " + r.detail + "; next try at " + utc_text(next));
            rep.stopped = true;
            rep.next_try_unix = next;
            break;
        }
    }
    save_state(fails, next, rep.stopped ? last_error : std::string());
    return rep;
}

void PublishQueue::write_status(const std::string& site, std::int64_t now, bool active) const {
    // Last line of sent.log.
    std::string last;
    {
        std::ifstream in(root_ / "sent.log", std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) last = line;
        }
    }
    std::string last_sent = "null";
    if (!last.empty()) {
        std::vector<std::string> f;
        std::stringstream ss(last);
        std::string part;
        while (std::getline(ss, part, '\t')) f.push_back(part);
        f.resize(4);
        last_sent = "{\"utc\": " + net::json_quote(f[0]) +
                    ", \"chart_id\": " + net::json_quote(f[1]) +
                    ", \"url\": " + net::json_quote(f[3]) + "}";
    }
    std::string last_error;
    if (const auto j = net::parse_json(read_text(root_ / "state.json"))) {
        last_error = string_of(*j, "last_error");
    }
    const std::int64_t next = next_try_unix();
    write_text(root_ / "status.json",
               "{\"active\": " + std::string(active ? "true" : "false") + ", \"site\": " +
                   net::json_quote(site) + ", \"updated_unix\": " + std::to_string(now) +
                   ", \"updated_utc\": " + net::json_quote(utc_text(now)) +
                   ", \"pending\": " + std::to_string(pending().size()) +
                   ", \"refused\": " + std::to_string(refused().size()) +
                   ", \"failures\": " + std::to_string(failures()) +
                   ", \"next_try_utc\": " + (next > 0 ? net::json_quote(utc_text(next)) : "null") +
                   ", \"last_error\": " + net::json_quote(last_error) +
                   ", \"last_sent\": " + last_sent + "}\n");
}

bool read_credentials(const fs::path& file, Credentials& out, std::string& error) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        error = "no credentials file " + file.string();
        return false;
    }
#ifndef _WIN32
    const auto perms = fs::status(file, ec).permissions();
    if ((perms & (fs::perms::group_all | fs::perms::others_all)) != fs::perms::none) {
        error = file.string() + " is readable by other users: chmod 600 it (PUB-063)";
        return false;
    }
#endif
    std::string line = read_text(file);
    const auto eol = line.find_first_of("\r\n");
    if (eol != std::string::npos) line.resize(eol);
    const auto colon = line.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= line.size()) {
        error = file.string() + ": expected one line \"login:application password\"";
        return false;
    }
    out.login = line.substr(0, colon);
    out.password = line.substr(colon + 1);
    return true;
}

}  // namespace owfax::vitrine
