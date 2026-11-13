// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Local HTTP API and WebSocket live stream: encodings, address filter, and a real
// server on a free port of 127.0.0.1, queried over TCP.

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "owfax/image/gray_image.hpp"
#include "owfax/net/api.hpp"
#include "owfax/net/encoding.hpp"
#include "owfax/net/http_server.hpp"
#include "owfax/net/socket.hpp"
#include "owfax_test.hpp"

namespace fs = std::filesystem;
using namespace owfax::net;

namespace {

std::uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) {
    const unsigned char bytes[4] = {static_cast<unsigned char>(a), static_cast<unsigned char>(b),
                                    static_cast<unsigned char>(c), static_cast<unsigned char>(d)};
    std::uint32_t v = 0;
    std::memcpy(&v, bytes, 4);
    return v;  // network byte order
}

std::string hex(const std::array<std::uint8_t, 20>& d) {
    static const char* const k = "0123456789abcdef";
    std::string s;
    for (auto b : d) {
        s += k[b >> 4];
        s += k[b & 15];
    }
    return s;
}

struct Reply {
    int status = 0;
    std::string head;
    std::string body;
};

/// Sends raw request text and reads the whole answer (the server closes).
Reply raw_request(int port, const std::string& text) {
    TcpSocket s = TcpSocket::connect("127.0.0.1", port);
    s.send_all(text);
    std::string data;
    char buf[4096];
    for (int i = 0; i < 200; ++i) {
        const long n = s.receive(buf, sizeof buf, 100);
        if (n == kClosed || n == kError) break;
        if (n > 0) data.append(buf, static_cast<std::size_t>(n));
    }
    Reply r;
    const auto end = data.find("\r\n\r\n");
    if (end == std::string::npos) return r;
    r.head = data.substr(0, end);
    r.body = data.substr(end + 4);
    if (r.head.size() > 12) r.status = std::stoi(r.head.substr(9, 3));
    return r;
}

Reply get(int port, const std::string& path, const std::string& method = "GET") {
    return raw_request(port, method + " " + path + " HTTP/1.1\r\nHost: test\r\n\r\n");
}

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

/// Reads one unmasked server frame; returns its opcode and payload.
bool read_frame(TcpSocket& s, std::string& pending, int& opcode, std::string& payload) {
    char buf[4096];
    for (int i = 0; i < 100; ++i) {
        if (pending.size() >= 2) {
            const auto b1 = static_cast<std::uint8_t>(pending[1]);
            std::size_t len = b1 & 0x7FU;
            std::size_t pos = 2;
            if (len == 126 && pending.size() >= 4) {
                len = (static_cast<std::size_t>(static_cast<std::uint8_t>(pending[2])) << 8) |
                      static_cast<std::uint8_t>(pending[3]);
                pos = 4;
            }
            if (len != 126 && pending.size() >= pos + len) {
                opcode = static_cast<std::uint8_t>(pending[0]) & 0x0F;
                payload = pending.substr(pos, len);
                pending.erase(0, pos + len);
                return true;
            }
        }
        const long n = s.receive(buf, sizeof buf, 100);
        if (n == kClosed || n == kError) return false;
        if (n > 0) pending.append(buf, static_cast<std::size_t>(n));
    }
    return false;
}

}  // namespace

OWFAX_TEST(net_encodings) {
    OWFAX_CHECK(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    OWFAX_CHECK(hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    OWFAX_CHECK(base64_encode("") == "");
    OWFAX_CHECK(base64_encode("f") == "Zg==");
    OWFAX_CHECK(base64_encode("fo") == "Zm8=");
    OWFAX_CHECK(base64_encode("foo") == "Zm9v");
    OWFAX_CHECK(base64_encode("foobar") == "Zm9vYmFy");
    // RFC 6455 §1.3 example.
    OWFAX_CHECK(websocket_accept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    OWFAX_CHECK(url_decode("/a%20b%2Fc").value_or("") == "/a b/c");
    OWFAX_CHECK(!url_decode("/a%2"));
    OWFAX_CHECK(!url_decode("/a%zz"));
    OWFAX_CHECK(!url_decode("/a%00b"));
}

OWFAX_TEST(net_local_addresses_only) {
    OWFAX_CHECK(is_local_ipv4(ip(127, 0, 0, 1)));
    OWFAX_CHECK(is_local_ipv4(ip(192, 168, 1, 19)));
    OWFAX_CHECK(is_local_ipv4(ip(10, 3, 2, 1)));
    OWFAX_CHECK(is_local_ipv4(ip(172, 16, 0, 1)) && is_local_ipv4(ip(172, 31, 255, 1)));
    OWFAX_CHECK(is_local_ipv4(ip(169, 254, 7, 7)));
    OWFAX_CHECK(!is_local_ipv4(ip(172, 32, 0, 1)));
    OWFAX_CHECK(!is_local_ipv4(ip(8, 8, 8, 8)));
    OWFAX_CHECK(!is_local_ipv4(ip(100, 64, 0, 1)));
    OWFAX_CHECK(ipv4_text(ip(192, 168, 1, 19)) == "192.168.1.19");
}

OWFAX_TEST(net_safe_names) {
    OWFAX_CHECK(is_safe_name("20261110T0735Z_GYA_4610kHz"));
    OWFAX_CHECK(is_safe_name("dwd-pinneberg"));
    OWFAX_CHECK(!is_safe_name(""));
    OWFAX_CHECK(!is_safe_name(".."));
    OWFAX_CHECK(!is_safe_name(".hidden"));
    OWFAX_CHECK(!is_safe_name("a..b"));
    OWFAX_CHECK(!is_safe_name("a/b"));
    OWFAX_CHECK(!is_safe_name("a\\b"));
    OWFAX_CHECK(!is_safe_name("c:x"));
}

OWFAX_TEST(net_api_serves_charts_stations_and_errors) {
    const fs::path root = fs::temp_directory_path() / "owfax_test_api";
    fs::remove_all(root);
    fs::create_directories(root / "charts");
    fs::create_directories(root / "catalog");
    write_file(root / "charts" / "20261110T0723Z_GYA.json", "{\"stem\": \"20261110T0723Z_GYA\"}\n");
    write_file(root / "charts" / "20261110T0735Z_GYA.json", "{\"stem\": \"20261110T0735Z_GYA\"}\n");
    write_file(root / "charts" / "20261110T0735Z_GYA.png", std::string("\x89PNG raw", 8));
    write_file(root / "charts" / "20261110T0735Z_GYA_enh.png", std::string("\x89PNG enh", 8));
    write_file(root / "charts" / "status.json", "{\"state\": \"file\"}\n");
    write_file(root / "secret.json", "{\"secret\": 1}\n");
    write_file(root / "catalog" / "dwd-pinneberg.json", "{\"id\": \"dwd-pinneberg\"}\n");

    ApiConfig cfg;
    cfg.charts_dir = (root / "charts").string();
    cfg.catalog_dir = (root / "catalog").string();
    cfg.input = "test input";
    write_file(root / "journal.log", "first line\nsecond line\n");
    cfg.journal_path = (root / "journal.log").string();
    cfg.diagnostics = [] { return std::string("{\"host\": {\"tier\": \"discovery\"}}"); };
    Api api(cfg);
    api.set_status("{\"state\": \"listening\"}");
    HttpServer::Options opt;
    opt.bind = "127.0.0.1";
    opt.port = 0;
    HttpServer server(opt, api.handler());
    const int port = server.port();
    OWFAX_CHECK(port > 0);

    auto r = get(port, "/api/v1/info");
    OWFAX_CHECK(r.status == 200);
    OWFAX_CHECK(r.body.find("\"source_url\"") != std::string::npos);  // AGPL §13
    OWFAX_CHECK(r.body.find("\"receive_only\": true") != std::string::npos);
    OWFAX_CHECK(r.body.find("test input") != std::string::npos);

    r = get(port, "/api/v1/diagnostics");
    OWFAX_CHECK(r.status == 200 && r.body.find("discovery") != std::string::npos);
    r = get(port, "/api/v1/journal");
    OWFAX_CHECK(r.status == 200 && r.body.find("second line") != std::string::npos);
    OWFAX_CHECK(r.head.find("Content-Disposition: attachment") != std::string::npos);

    r = get(port, "/api/v1/status");
    OWFAX_CHECK(r.status == 200 && r.body.find("listening") != std::string::npos);
    OWFAX_CHECK(r.head.find("Cache-Control: no-store") != std::string::npos);

    r = get(port, "/api/v1/charts");
    OWFAX_CHECK(r.status == 200);
    const auto newest = r.body.find("0735Z");
    const auto oldest = r.body.find("0723Z");
    OWFAX_CHECK(newest != std::string::npos && oldest != std::string::npos && newest < oldest);
    OWFAX_CHECK(r.body.find("\"file\"") == std::string::npos);  // status.json is not a chart
    r = get(port, "/api/v1/charts?limit=1");
    OWFAX_CHECK(r.body.find("0735Z") != std::string::npos);
    OWFAX_CHECK(r.body.find("0723Z") == std::string::npos);

    r = get(port, "/api/v1/charts/20261110T0735Z_GYA");
    OWFAX_CHECK(r.status == 200 && r.body.find("0735Z") != std::string::npos);
    r = get(port, "/api/v1/charts/20261110T0735Z_GYA/raw.png");
    OWFAX_CHECK(r.status == 200 && r.body == std::string("\x89PNG raw", 8));
    OWFAX_CHECK(r.head.find("Content-Type: image/png") != std::string::npos);
    r = get(port, "/api/v1/charts/20261110T0735Z_GYA/enhanced.png");
    OWFAX_CHECK(r.status == 200 && r.body == std::string("\x89PNG enh", 8));
    OWFAX_CHECK(get(port, "/api/v1/charts/20261110T0735Z_GYA/live.png").status == 404);
    OWFAX_CHECK(get(port, "/api/v1/charts/20261110T0735Z_GYA/other.png").status == 404);
    OWFAX_CHECK(get(port, "/api/v1/charts/nope").status == 404);

    // No way out of the charts directory.
    for (const char* p : {"/api/v1/charts/..%2Fsecret", "/api/v1/charts/%2E%2E%2Fsecret",
                          "/api/v1/charts/..", "/api/v1/charts/status"}) {
        r = get(port, p);
        OWFAX_CHECK(r.status == 404 && r.body.find("secret\": 1") == std::string::npos);
    }

    // Light display version (16 grey levels), made once and cached next to the original.
    owfax::GrayImage chart(181, 40, 255);
    for (std::size_t i = 0; i < chart.pixels.size(); ++i) {
        chart.pixels[i] = static_cast<std::uint8_t>((i * 37) % 256);
    }
    owfax::write_png((root / "charts" / "20261110T0800Z_DWD.png").string(), chart);
    r = get(port, "/api/v1/charts/20261110T0800Z_DWD/raw.png?levels=16");
    OWFAX_CHECK(r.status == 200 && r.body.size() > 25 && r.body[24] == 4);
    OWFAX_CHECK(fs::exists(root / "charts" / "20261110T0800Z_DWD.l16.png"));
    const std::string first = r.body;
    r = get(port, "/api/v1/charts/20261110T0800Z_DWD/raw.png?levels=16");
    OWFAX_CHECK(r.status == 200 && r.body == first);
    r = get(port, "/api/v1/charts/20261110T0800Z_DWD/raw.png");
    OWFAX_CHECK(r.status == 200 && r.body[24] == 8);
    OWFAX_CHECK(get(port, "/api/v1/charts/20261110T0800Z_DWD/raw.png?levels=7").status == 400);
    r = get(port, "/api/v1/charts/20261110T0800Z_DWD/raw.png?levels=16&width=360");
    OWFAX_CHECK(r.status == 200 && r.body[24] == 4);  // narrower than 360: kept as is
    OWFAX_CHECK(fs::exists(root / "charts" / "20261110T0800Z_DWD.l16.w360.png"));
    OWFAX_CHECK(get(port, "/api/v1/charts/20261110T0800Z_DWD/raw.png?width=100").status == 400);
    OWFAX_CHECK(get(port, "/api/v1/charts/20261110T0800Z_DWD/enhanced.png?levels=16").status ==
                404);

    r = get(port, "/api/v1/stations");
    OWFAX_CHECK(r.status == 200 && r.body.find("dwd-pinneberg") != std::string::npos);
    OWFAX_CHECK(get(port, "/api/v1/stations/dwd-pinneberg").status == 200);
    OWFAX_CHECK(get(port, "/api/v1/stations/../secret").status == 404);

    OWFAX_CHECK(get(port, "/api/v1/nope").status == 404);
    OWFAX_CHECK(get(port, "/api/v2/info").status == 404);
    r = get(port, "/");
    OWFAX_CHECK(r.status == 200 && r.body.find("code source") != std::string::npos);  // UI-006

    r = get(port, "/api/v1/info", "HEAD");
    OWFAX_CHECK(r.status == 200 && r.body.empty());
    OWFAX_CHECK(get(port, "/api/v1/info", "POST").status == 411);  // no Content-Length
    OWFAX_CHECK(raw_request(port, "nonsense\r\n\r\n").status == 400);
    OWFAX_CHECK(
        raw_request(port, "GET /api/v1/info HTTP/1.1\r\nX: " + std::string(9000, 'a') + "\r\n\r\n")
            .status == 431);
    // The live endpoint without an upgrade is refused.
    OWFAX_CHECK(get(port, "/api/v1/live").status == 426);
    server.stop();
    fs::remove_all(root);
}

OWFAX_TEST(net_api_serves_vitrine_packages_by_range) {
    const fs::path root = fs::temp_directory_path() / "owfax_test_vitrine";
    fs::remove_all(root);
    const std::string older = "20261110T0945Z_DDK6_13882.5kHz";
    const std::string newer = "20261110T1007Z_DDK6_13882.5kHz";
    fs::create_directories(root / "pkg" / older);
    fs::create_directories(root / "pkg" / newer);
    fs::create_directories(root / "pkg" / "not-a-package");
    write_file(root / "pkg" / older / "meta.json", "{\"chart_id\": \"" + older + "\"}\n");
    write_file(root / "pkg" / newer / "meta.json", "{\"chart_id\": \"" + newer + "\"}\n");
    write_file(root / "pkg" / "not-a-package" / "meta.json", "{\"chart_id\": \"intruder\"}\n");
    write_file(root / "pkg" / newer / "audio.m4a", "0123456789");
    write_file(root / "pkg" / newer / "raw.webp", "RIFFwebp");
    write_file(root / "pkg" / newer / "notes.txt", "private");

    ApiConfig cfg;
    cfg.vitrine_dir = (root / "pkg").string();
    Api api(cfg);
    HttpServer::Options opt;
    opt.bind = "127.0.0.1";
    opt.port = 0;
    HttpServer server(opt, api.handler());
    const int port = server.port();
    const std::string audio = "/api/v1/vitrine/" + newer + "/audio.m4a";
    auto ranged = [&](const std::string& range) {
        return raw_request(
            port, "GET " + audio + " HTTP/1.1\r\nHost: test\r\nRange: " + range + "\r\n\r\n");
    };

    auto r = get(port, "/api/v1/vitrine");
    OWFAX_CHECK(r.status == 200);
    const auto n = r.body.find("1007Z"), o = r.body.find("0945Z");
    OWFAX_CHECK(n != std::string::npos && o != std::string::npos && n < o);
    OWFAX_CHECK(r.body.find("intruder") == std::string::npos);
    r = get(port, "/api/v1/vitrine?limit=1");
    OWFAX_CHECK(r.body.find("1007Z") != std::string::npos);
    OWFAX_CHECK(r.body.find("0945Z") == std::string::npos);

    r = get(port, audio);
    OWFAX_CHECK(r.status == 200 && r.body == "0123456789");
    OWFAX_CHECK(r.head.find("Content-Type: audio/mp4") != std::string::npos);
    OWFAX_CHECK(r.head.find("Accept-Ranges: bytes") != std::string::npos);
    r = get(port, "/api/v1/vitrine/" + newer + "/raw.webp");
    OWFAX_CHECK(r.status == 200 && r.head.find("Content-Type: image/webp") != std::string::npos);

    // Ranges, as browsers ask for them to seek in the sound.
    r = ranged("bytes=2-5");
    OWFAX_CHECK(r.status == 206 && r.body == "2345");
    OWFAX_CHECK(r.head.find("Content-Range: bytes 2-5/10") != std::string::npos);
    OWFAX_CHECK(r.head.find("Content-Length: 4") != std::string::npos);
    r = ranged("bytes=0-");
    OWFAX_CHECK(r.status == 206 && r.body == "0123456789");
    r = ranged("bytes=7-99");
    OWFAX_CHECK(r.status == 206 && r.body == "789");
    r = ranged("bytes=-3");
    OWFAX_CHECK(r.status == 206 && r.body == "789");
    r = ranged("bytes=10-");
    OWFAX_CHECK(r.status == 416 && r.head.find("Content-Range: bytes */10") != std::string::npos);
    r = ranged("bytes=0-1,4-5");  // several ranges: the whole file
    OWFAX_CHECK(r.status == 200 && r.body == "0123456789");
    r = ranged("bytes=5-2");  // malformed: the whole file
    OWFAX_CHECK(r.status == 200 && r.body == "0123456789");
    r = raw_request(port, "HEAD " + audio + " HTTP/1.1\r\nHost: test\r\nRange: bytes=0-1\r\n\r\n");
    OWFAX_CHECK(r.status == 206 && r.body.empty());

    // Only the package files, only from package directories.
    OWFAX_CHECK(get(port, "/api/v1/vitrine/" + newer + "/notes.txt").status == 404);
    OWFAX_CHECK(get(port, "/api/v1/vitrine/" + newer + "/enh.webp").status == 404);
    OWFAX_CHECK(get(port, "/api/v1/vitrine/not-a-package/meta.json").status == 404);
    OWFAX_CHECK(get(port, "/api/v1/vitrine/..%2F" + newer + "/meta.json").status == 404);
    OWFAX_CHECK(get(port, "/api/v1/vitrine/" + newer).status == 404);
    server.stop();

    // Without --vitrine there is nothing to serve.
    Api none(ApiConfig{});
    HttpServer server2(opt, none.handler());
    OWFAX_CHECK(get(server2.port(), "/api/v1/vitrine").status == 404);
    server2.stop();
    fs::remove_all(root);
}

OWFAX_TEST(net_websocket_live_stream) {
    Api api(ApiConfig{});
    api.set_status("{\"state\": \"receiving\"}");
    HttpServer::Options opt;
    opt.bind = "127.0.0.1";
    opt.port = 0;
    HttpServer server(opt, api.handler());

    TcpSocket s = TcpSocket::connect("127.0.0.1", server.port());
    s.send_all(
        "GET /api/v1/live HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n");
    std::string data;
    char buf[2048];
    for (int i = 0; i < 50 && data.find("\r\n\r\n") == std::string::npos; ++i) {
        const long n = s.receive(buf, sizeof buf, 100);
        if (n > 0) data.append(buf, static_cast<std::size_t>(n));
    }
    const auto end = data.find("\r\n\r\n");
    OWFAX_CHECK(end != std::string::npos);
    if (end == std::string::npos) return;
    OWFAX_CHECK(data.rfind("HTTP/1.1 101", 0) == 0);
    OWFAX_CHECK(data.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
                std::string::npos);
    std::string pending = data.substr(end + 4);

    int opcode = 0;
    std::string msg;
    OWFAX_CHECK(read_frame(s, pending, opcode, msg));  // greeting: current status
    OWFAX_CHECK(opcode == 1 && msg.find("\"type\": \"status\"") != std::string::npos);
    OWFAX_CHECK(msg.find("receiving") != std::string::npos);

    // Registered before the greeting is queued.
    OWFAX_CHECK(api.live_clients() == 1);
    const std::string big(3000, 'x');  // a live line is ~2.4 kB of base64
    api.publish("{\"type\": \"line\", \"pixels\": \"" + big + "\"}");
    OWFAX_CHECK(read_frame(s, pending, opcode, msg));
    OWFAX_CHECK(opcode == 1 && msg.find(big) != std::string::npos);

    // Masked close frame from the client; the server answers with a close.
    const std::string close_frame("\x88\x82\x01\x02\x03\x04\x02\xEA", 8);
    s.send_all(close_frame);
    OWFAX_CHECK(read_frame(s, pending, opcode, msg));
    OWFAX_CHECK(opcode == 8);
    for (int i = 0; i < 100 && api.live_clients() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    OWFAX_CHECK(api.live_clients() == 0);
    server.stop();
}
