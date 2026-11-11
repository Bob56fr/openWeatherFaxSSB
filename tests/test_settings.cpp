// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Skipper settings and password (spec UI-002, UI-004, NFR-006): crypto test vectors,
// JSON reader, settings file, and the whole flow through the HTTP API.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "owfax/net/api.hpp"
#include "owfax/net/crypto.hpp"
#include "owfax/net/http_server.hpp"
#include "owfax/net/json.hpp"
#include "owfax/net/settings.hpp"
#include "owfax/net/socket.hpp"
#include "owfax_test.hpp"

namespace fs = std::filesystem;
using namespace owfax::net;

namespace {

std::string hex(const std::array<std::uint8_t, 32>& d) {
    return to_hex(d.data(), d.size());
}

struct Reply {
    int status = 0;
    std::string body;
};

Reply call(int port, const std::string& method, const std::string& path, const std::string& body,
           const std::string& token = "") {
    TcpSocket s = TcpSocket::connect("127.0.0.1", port);
    std::string req = method + " " + path +
                      " HTTP/1.1\r\nHost: test\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\n";
    if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
    req += "\r\n" + body;
    s.send_all(req);
    std::string data;
    char buf[4096];
    for (int i = 0; i < 300; ++i) {
        const long n = s.receive(buf, sizeof buf, 100);
        if (n == kClosed || n == kError) break;
        if (n > 0) data.append(buf, static_cast<std::size_t>(n));
    }
    Reply r;
    const auto end = data.find("\r\n\r\n");
    if (end == std::string::npos || data.size() < 12) return r;
    r.status = std::stoi(data.substr(9, 3));
    r.body = data.substr(end + 4);
    return r;
}

std::string token_of(const std::string& body) {
    const auto j = parse_json(body);
    const Json* t = j ? j->get("token") : nullptr;
    return t && t->is(Json::Type::kString) ? t->string : std::string();
}

double version_of(const std::string& body) {
    const auto j = parse_json(body);
    const Json* v = j ? j->get("version") : nullptr;
    return v ? v->number : -1.0;
}

}  // namespace

OWFAX_TEST(crypto_test_vectors) {
    // FIPS 180-4 examples.
    OWFAX_CHECK(hex(sha256("abc")) ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    OWFAX_CHECK(hex(sha256("")) ==
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    OWFAX_CHECK(hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // RFC 4231 test case 1.
    OWFAX_CHECK(hex(hmac_sha256(std::string(20, '\x0b'), "Hi There")) ==
                "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    // PBKDF2-HMAC-SHA256 (RFC 7914 §11 / common test vectors).
    auto k = pbkdf2_sha256("password", "salt", 1, 32);
    OWFAX_CHECK(to_hex(k.data(), k.size()) ==
                "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    k = pbkdf2_sha256("password", "salt", 4096, 32);
    OWFAX_CHECK(to_hex(k.data(), k.size()) ==
                "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    const auto r1 = secure_random(32);
    const auto r2 = secure_random(32);
    OWFAX_CHECK(r1.size() == 32 && r1 != r2);
    OWFAX_CHECK(constant_time_equal("abc", "abc") && !constant_time_equal("abc", "abd") &&
                !constant_time_equal("abc", "abcd"));
    OWFAX_CHECK(from_hex("0aFf").size() == 2 && from_hex("0g").empty() && from_hex("abc").empty());
}

OWFAX_TEST(json_reader) {
    const auto j =
        parse_json(R"({"a": [1, -2.5e1, true, null], "s": "x\"\u00e9\ud83d\ude00", "o": {}})");
    OWFAX_CHECK(j.has_value());
    if (j) {
        const Json* a = j->get("a");
        OWFAX_CHECK(a && a->array.size() == 4 && a->array[1].number == -25.0 &&
                    a->array[2].boolean);
        const Json* s = j->get("s");
        OWFAX_CHECK(s && s->string == "x\"\xc3\xa9\xf0\x9f\x98\x80");
    }
    for (const char* bad : {"", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "tru", "\"\\x\"", "1 2",
                            "\"\\ud800\"", "{\"a\":1,}"}) {
        OWFAX_CHECK(!parse_json(bad).has_value());
    }
    std::string deep(100, '[');
    OWFAX_CHECK(!parse_json(deep + std::string(100, ']')).has_value());  // bounded nesting
    OWFAX_CHECK(json_quote("a\"b\n") == "\"a\\\"b\\n\"");
}

OWFAX_TEST(settings_file_versions_and_password) {
    const fs::path dir = fs::temp_directory_path() / "owfax_test_settings";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "owfaxd.json").string();
    StationSettings defaults;
    defaults.label = "DWD";
    defaults.shift_hz = 850;
    {
        SettingsStore st(path, defaults);
        std::string err;
        OWFAX_CHECK(st.load(err));  // no file yet
        OWFAX_CHECK(!st.password_set() && st.version() == 1);
        bool threw = false;
        try {
            st.set_password("short");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        OWFAX_CHECK(threw);
        st.set_password("bon vent 2026");
        StationSettings s = st.settings();
        s.label = "GYA";
        s.shift_hz = 800;
        OWFAX_CHECK(st.update(s, 1));
        OWFAX_CHECK(!st.update(s, 1));  // stale version: refused (UI-004)
        OWFAX_CHECK(st.version() == 2);
    }
    {
        std::ifstream in(path);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        OWFAX_CHECK(text.find("bon vent") == std::string::npos);  // never in clear
        OWFAX_CHECK(text.find("pbkdf2-sha256") != std::string::npos);
    }
    SettingsStore again(path, defaults);
    std::string err;
    OWFAX_CHECK(again.load(err));
    OWFAX_CHECK(again.version() == 2 && again.settings().label == "GYA" &&
                again.settings().shift_hz == 800);
    OWFAX_CHECK(again.password_set() && again.check_password("bon vent 2026") &&
                !again.check_password("bon vent 2025"));

    std::string why;
    const auto bad = settings_from_json(*parse_json(R"({"label": "a/b"})"), defaults, why);
    OWFAX_CHECK(!bad && !why.empty());
    OWFAX_CHECK(!settings_from_json(*parse_json(R"({"shift_hz": 50})"), defaults, why));
    OWFAX_CHECK(!settings_from_json(*parse_json(R"({"enhanced": "yes"})"), defaults, why));
    {
        std::ofstream out(path);
        out << "{not json";
    }
    SettingsStore broken(path, defaults);
    OWFAX_CHECK(!broken.load(err) && !err.empty());
    fs::remove_all(dir);
}

OWFAX_TEST(settings_api_flow) {
    const fs::path dir = fs::temp_directory_path() / "owfax_test_settings_api";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // A settings file placed in the charts directory must never be served.
    SettingsStore store((dir / "owfaxd.json").string());
    std::string err;
    store.load(err);
    int applied = 0;
    StationSettings last;
    ApiConfig cfg;
    cfg.charts_dir = dir.string();
    cfg.settings = &store;
    cfg.running_input = "alsa:default";
    cfg.on_settings = [&](const StationSettings& s) {
        ++applied;
        last = s;
    };
    Api api(cfg);
    HttpServer::Options opt;
    opt.bind = "127.0.0.1";
    opt.port = 0;
    HttpServer server(opt, api.handler());
    const int port = server.port();

    auto r = call(port, "GET", "/api/v1/settings", "");
    OWFAX_CHECK(r.status == 200 && r.body.find("\"password_set\": false") != std::string::npos);
    OWFAX_CHECK(call(port, "POST", "/api/v1/session", R"({"password": "x"})").status == 409);
    OWFAX_CHECK(call(port, "POST", "/api/v1/password", R"({"new": "court"})").status == 400);

    // First setup: create the password, which also logs in.
    r = call(port, "POST", "/api/v1/password", R"({"new": "skipper-2026"})");
    OWFAX_CHECK(r.status == 200);
    const std::string token = token_of(r.body);
    OWFAX_CHECK(token.size() == 64);
    // Afterwards nobody can replace it without the session and the current password.
    OWFAX_CHECK(call(port, "POST", "/api/v1/password", R"({"new": "pirate-2026"})").status == 401);
    OWFAX_CHECK(call(port, "POST", "/api/v1/password",
                     R"({"current": "faux", "new": "pirate-2026"})", token)
                    .status == 401);

    // Changing settings needs the session and the current version.
    r = call(port, "GET", "/api/v1/settings", "", token);
    OWFAX_CHECK(r.body.find("\"logged_in\": true") != std::string::npos);
    const double v = version_of(r.body);
    const std::string change =
        R"({"version": )" + std::to_string(static_cast<int>(v)) +
        R"(, "settings": {"label": "DDH3", "shift_hz": 850, "input": "alsa:hw:1"}})";
    OWFAX_CHECK(call(port, "PUT", "/api/v1/settings", change).status == 401);
    OWFAX_CHECK(call(port, "PUT", "/api/v1/settings", change, "0" + token.substr(1)).status == 401);
    r = call(port, "PUT", "/api/v1/settings", change, token);
    OWFAX_CHECK(r.status == 200 && applied == 1 && last.label == "DDH3" && last.shift_hz == 850);
    OWFAX_CHECK(r.body.find("\"restart_required\": true") != std::string::npos);
    OWFAX_CHECK(call(port, "PUT", "/api/v1/settings", change, token).status == 409);  // stale
    OWFAX_CHECK(call(port, "PUT", "/api/v1/settings",
                     R"({"version": 2, "settings": {"shift_hz": 5000}})", token)
                    .status == 400);
    OWFAX_CHECK(applied == 1);

    // The settings file is not a chart.
    OWFAX_CHECK(call(port, "GET", "/api/v1/charts", "").body.find("pbkdf2") == std::string::npos);
    OWFAX_CHECK(call(port, "GET", "/api/v1/charts/owfaxd", "").status == 404);
    // Read-only endpoints refuse changes.
    OWFAX_CHECK(call(port, "POST", "/api/v1/info", "{}").status == 405);
    OWFAX_CHECK(call(port, "DELETE", "/api/v1/charts", "").status == 405);

    // Login, then brute-force protection: 5 failures lock out for 30 s.
    OWFAX_CHECK(
        token_of(call(port, "POST", "/api/v1/session", R"({"password": "skipper-2026"})").body)
            .size() == 64);
    for (int i = 0; i < 5; ++i) {
        OWFAX_CHECK(call(port, "POST", "/api/v1/session", R"({"password": "faux"})").status == 401);
    }
    OWFAX_CHECK(call(port, "POST", "/api/v1/session", R"({"password": "skipper-2026"})").status ==
                429);

    // Logout ends the session.
    OWFAX_CHECK(call(port, "DELETE", "/api/v1/session", "", token).status == 200);
    OWFAX_CHECK(call(port, "PUT", "/api/v1/settings", change, token).status == 401);
    server.stop();
    fs::remove_all(dir);
}
