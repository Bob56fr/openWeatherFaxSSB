// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// owfax-vitrine enqueue | send | queue: publication of the chart packages to the
// WordPress plugin (docs/VITRINE-WEB.md § 4.5, lot 5).
//
// Sending goes through the curl program (HTTPS, outgoing only: VIT-001, SEC-001). Its
// whole configuration, the password included, is written on curl's standard input:
// the password never appears on a command line (ps) nor in a journal (PUB-063).

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "owfax/net/json.hpp"
#include "owfax/vitrine/publish.hpp"

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
constexpr const char* kPipeWrite = "wb";  // no CR added to the configuration
#else
constexpr const char* kPipeWrite = "w";
#endif

namespace fs = std::filesystem;
using namespace owfax;
using namespace owfax::vitrine;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

std::int64_t now_unix() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string shell_quote(const std::string& s) {
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
#endif
}

/// Quoted string of a curl configuration file.
std::string curl_quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

struct Site {
    std::string base;  ///< "https://example.org" (no trailing slash)
    bool http_allowed = false;
};

/// HTTPS only (PUB-060, application passwords need it), except to this computer.
std::optional<Site> parse_site(std::string url, std::string& error) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    Site s;
    s.base = url;
    if (url.rfind("https://", 0) == 0) return s;
    if (url.rfind("http://", 0) == 0) {
        const std::string host = url.substr(7, url.find_first_of(":/", 7) - 7);
        if (host == "localhost" || host == "127.0.0.1" || url.rfind("http://[::1]", 0) == 0) {
            s.http_allowed = true;  // a test WordPress on this computer
            return s;
        }
    }
    error = "the site must be an https:// address (" + url + ")";
    return std::nullopt;
}

struct CurlAnswer {
    int http = 0;
    std::string body;
    std::string error;  ///< curl's message when the exchange failed
};

/// One HTTP exchange through curl, configured on its standard input.
CurlAnswer curl_call(const std::string& curl, const fs::path& work, const Site& site,
                     const std::string& path, const Credentials& cred,
                     const std::vector<std::string>& forms, int max_time_s) {
    fs::create_directories(work);
    const fs::path body = work / "answer.txt";
    const fs::path code = work / "http_code.txt";
    const fs::path err = work / "curl_error.txt";
    for (const auto& p : {body, code, err}) {
        std::error_code ec;
        fs::remove(p, ec);
    }
    std::string cmd = shell_quote(curl) + " -K - > " + shell_quote(code.string()) + " 2> " +
                      shell_quote(err.string());
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd.exe strips the outer quotes
#endif
    std::string cfg;
    cfg += "url = " + curl_quote(site.base + "/wp-json/owfax/v1" + path) + "\n";
    cfg += "user = " + curl_quote(cred.login + ":" + cred.password) + "\n";
    cfg += std::string("proto = ") + (site.http_allowed ? "\"=http,https\"" : "\"=https\"") + "\n";
    cfg += "silent\nshow-error\n";
    cfg += "connect-timeout = 30\nmax-time = " + std::to_string(max_time_s) + "\n";
    cfg += "user-agent = \"owfax-vitrine\"\n";
    cfg += "output = " + curl_quote(body.generic_string()) + "\n";
    cfg += "write-out = \"%{http_code}\"\n";
    for (const auto& f : forms) cfg += "form = " + curl_quote(f) + "\n";

    CurlAnswer a;
    std::FILE* pipe = popen(cmd.c_str(), kPipeWrite);
    if (!pipe) {
        a.error = "cannot run " + curl;
        return a;
    }
    std::fwrite(cfg.data(), 1, cfg.size(), pipe);
    const int rc = pclose(pipe);
    a.body = read_text(body);
    a.http = std::atoi(read_text(code).c_str());
    if (rc != 0 || a.http == 0) {
        std::string e = read_text(err);
        while (!e.empty() && (e.back() == '\n' || e.back() == '\r')) e.pop_back();
        a.error = e.empty() ? "curl failed (" + std::to_string(rc) + "; is curl installed?)" : e;
        a.http = 0;
    }
    return a;
}

/// What the host accepts (GET /limits), for PUB-064.
struct HostLimits {
    std::int64_t max_upload = 0;
    std::int64_t max_post = 0;
};

std::optional<HostLimits> fetch_limits(const std::string& curl, const fs::path& work,
                                       const Site& site, const Credentials& cred) {
    const auto a = curl_call(curl, work, site, "/limits", cred, {}, 60);
    if (a.http != 200) return std::nullopt;
    const auto j = net::parse_json(a.body);
    if (!j) return std::nullopt;
    HostLimits l;
    if (const auto* v = j->get("max_upload_bytes"); v && v->is(net::Json::Type::kNumber)) {
        l.max_upload = static_cast<std::int64_t>(v->number);
    }
    if (const auto* v = j->get("max_post_bytes"); v && v->is(net::Json::Type::kNumber)) {
        l.max_post = static_cast<std::int64_t>(v->number);
    }
    return l;
}

void usage() {
    std::fputs(
        "Usage:\n"
        "  owfax-vitrine enqueue --queue <dir> <package dir>...\n"
        "  owfax-vitrine send --queue <dir> --site <https://site> --credentials <file>\n"
        "                     [--loop] [--now] [--curl <path>]\n"
        "  owfax-vitrine queue --queue <dir>\n"
        "\n"
        "enqueue  copies publishable packages into the publication queue\n"
        "send     sends the queue to the WordPress plugin, oldest chart first; on a\n"
        "         failure waits 1 min, then twice as long each time (6 h at most); a\n"
        "         package waiting for 48 h is dropped. --loop keeps going (service),\n"
        "         --now ignores the waiting delay once.\n"
        "queue    shows what waits, what was refused, the last sends\n"
        "\n"
        "The credentials file holds one line \"login:application password\" of the\n"
        "WordPress account with the role owfax publisher (chmod 600).\n",
        stderr);
}

int cmd_enqueue(const fs::path& queue, const std::vector<std::string>& packages) {
    PublishQueue q(queue);
    int bad = 0;
    for (const auto& p : packages) {
        std::string err;
        const std::string r = q.enqueue(p, now_unix(), err);
        if (r.empty()) {
            std::fprintf(stderr, "owfax-vitrine: %s\n", err.c_str());
            ++bad;
        } else {
            fs::path name(p);
            if (name.filename().empty()) name = name.parent_path();  // "dir/"
            std::printf("%s: %s\n", name.filename().string().c_str(), r.c_str());
        }
    }
    return bad ? 1 : 0;
}

int cmd_queue(const fs::path& queue) {
    PublishQueue q(queue);
    const auto pending = q.pending();
    std::printf("Waiting: %zu\n", pending.size());
    for (const auto& i : pending) {
        std::printf("  %s  queued %s, %d tries%s%s\n", i.chart_id.c_str(),
                    utc_text(i.enqueued_unix).c_str(), i.attempts, i.last_error.empty() ? "" : ": ",
                    i.last_error.c_str());
    }
    if (q.next_try_unix() > 0) std::printf("Next try: %s\n", utc_text(q.next_try_unix()).c_str());
    const auto refused = q.refused();
    std::printf("Refused by the site (kept in %s): %zu\n", (queue / "refused").string().c_str(),
                refused.size());
    for (const auto& i : refused)
        std::printf("  %s  %s\n", i.chart_id.c_str(), i.last_error.c_str());
    std::ifstream sent(queue / "sent.log");
    std::vector<std::string> lines;
    for (std::string l; std::getline(sent, l);) lines.push_back(l);
    std::printf("Sent: %zu\n", lines.size());
    for (std::size_t i = lines.size() > 5 ? lines.size() - 5 : 0; i < lines.size(); ++i) {
        std::printf("  %s\n", lines[i].c_str());
    }
    return 0;
}

int cmd_send(const fs::path& queue, const Site& site, const Credentials& cred,
             const std::string& curl, bool loop, bool now_flag) {
    PublishQueue q(queue);
    q.on_log = [](const std::string& line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    const fs::path work = queue / ".send";
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    q.log(now_unix(), "publication to " + site.base + " active (account " + cred.login + ")");

    bool force = now_flag;
    do {
        std::optional<HostLimits> limits;
        bool limits_asked = false;
        const auto sender = [&](const QueueItem& item) -> SendResult {
            if (!limits_asked) {  // once per round, only when something is sent
                limits = fetch_limits(curl, work, site, cred);
                limits_asked = true;
            }
            std::vector<std::string> forms;
            std::int64_t total = 0;
            for (const auto& f : package_files()) {
                const fs::path p = item.dir / f.name;
                std::error_code ec;
                if (!fs::is_regular_file(p, ec)) continue;
                const auto size = static_cast<std::int64_t>(fs::file_size(p, ec));
                total += size;
                if (limits && limits->max_upload > 0 && size > limits->max_upload) {
                    SendResult r;
                    r.status = SendStatus::kRefused;
                    r.detail = std::string(f.name) + " is " + std::to_string(size) +
                               " bytes, the host accepts " + std::to_string(limits->max_upload) +
                               " (upload_max_filesize)";
                    return r;
                }
                forms.push_back(std::string(f.field) + "=@\"" + p.generic_string() + "\"");
            }
            if (limits && limits->max_post > 0 && total > limits->max_post) {
                SendResult r;
                r.status = SendStatus::kRefused;
                r.detail = "package of " + std::to_string(total) + " bytes, the host accepts " +
                           std::to_string(limits->max_post) + " (post_max_size)";
                return r;
            }
            const auto a = curl_call(curl, work, site, "/charts", cred, forms, 900);
            return classify_answer(a.http, a.body, a.error);
        };
        const auto rep = q.run(now_unix, sender, force);
        if (rep.waited && !loop) {
            std::printf("Nothing tried: next try at %s (--now to try at once)\n",
                        utc_text(rep.next_try_unix).c_str());
        }
        force = false;
        q.write_status(site.base, now_unix(), loop);
        for (int s = 0; loop && s < 60 && !g_stop; ++s) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } while (loop && !g_stop);
    if (loop) {
        q.log(now_unix(), "publication stopped");
        q.write_status(site.base, now_unix(), false);
    }
    std::error_code ec;
    fs::remove_all(work, ec);
    return 0;
}

}  // namespace

int publish_main(int argc, char** argv) {
    const std::string cmd = argv[1];
    std::string queue, site_url, cred_file, curl = "curl";
    bool loop = false, now_flag = false;
    std::vector<std::string> packages;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        bool ok = true;
        if (a == "--queue")
            ok = value(queue);
        else if (a == "--site")
            ok = value(site_url);
        else if (a == "--credentials")
            ok = value(cred_file);
        else if (a == "--curl")
            ok = value(curl);
        else if (a == "--loop")
            loop = true;
        else if (a == "--now")
            now_flag = true;
        else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (!a.empty() && a[0] != '-' && cmd == "enqueue")
            packages.push_back(a);
        else
            ok = false;
        if (!ok) {
            std::fprintf(stderr, "owfax-vitrine: bad option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (queue.empty()) {
        std::fputs("owfax-vitrine: --queue is required\n", stderr);
        return 2;
    }
    try {
        if (cmd == "enqueue") return cmd_enqueue(queue, packages);
        if (cmd == "queue") return cmd_queue(queue);
        std::string err;
        const auto site = parse_site(site_url, err);
        if (!site) {
            std::fprintf(stderr, "owfax-vitrine: %s\n", err.c_str());
            return 2;
        }
        Credentials cred;
        if (!read_credentials(cred_file, cred, err)) {
            std::fprintf(stderr, "owfax-vitrine: %s\n", err.c_str());
            return 2;
        }
        return cmd_send(queue, *site, cred, curl, loop, now_flag);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "owfax-vitrine: %s\n", e.what());
        return 1;
    }
}
