// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/net/http_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

#include "owfax/net/encoding.hpp"

namespace owfax::net {
namespace {

constexpr std::size_t kMaxHeaderBytes = 8192;
constexpr std::size_t kMaxBodyBytes = 65536;
constexpr int kRequestTimeoutMs = 10000;
constexpr int kPollMs = 200;
constexpr int kSendTimeoutMs = 10000;
constexpr std::size_t kMaxClientFrame = 4096;  // the client only sends control frames

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

bool has_token(const std::string& value, const std::string& token) {
    return lower(value).find(token) != std::string::npos;
}

/// Parses the request head (request line + headers, without the final blank line).
/// Returns 0 if valid, or the HTTP status to answer.
int parse_head(const std::string& head, Request& r) {
    std::size_t pos = head.find("\r\n");
    const std::string line = head.substr(0, pos);
    const auto sp1 = line.find(' ');
    const auto sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == sp1) return 400;
    r.method = line.substr(0, sp1);
    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string version = line.substr(sp2 + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return 400;
    if (target.empty() || target[0] != '/') return 400;
    const auto q = target.find('?');
    const auto path = url_decode(target.substr(0, q));
    if (!path) return 400;
    r.path = *path;
    if (q != std::string::npos) r.query = target.substr(q + 1);
    while (pos != std::string::npos && pos + 2 < head.size()) {
        const std::size_t next = head.find("\r\n", pos + 2);
        const std::string h =
            head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
        pos = next;
        const auto colon = h.find(':');
        if (colon == std::string::npos || colon == 0) return 400;
        r.headers[lower(h.substr(0, colon))] = trim(h.substr(colon + 1));
    }
    if (r.method != "GET" && r.method != "HEAD" && r.method != "POST" && r.method != "PUT" &&
        r.method != "DELETE") {
        return 405;
    }
    return 0;
}

std::string head_text(const Response& resp, std::size_t length, bool close) {
    std::string h =
        "HTTP/1.1 " + std::to_string(resp.status) + " " + status_text(resp.status) + "\r\n";
    if (!resp.content_type.empty()) h += "Content-Type: " + resp.content_type + "\r\n";
    h += "Content-Length: " + std::to_string(length) + "\r\n";
    h += "X-Content-Type-Options: nosniff\r\n";
    for (const auto& [k, v] : resp.headers) h += k + ": " + v + "\r\n";
    if (close) h += "Connection: close\r\n";
    h += "\r\n";
    return h;
}

}  // namespace

std::string Request::header(const std::string& lower_name) const {
    const auto it = headers.find(lower_name);
    return it == headers.end() ? std::string() : it->second;
}

Response Response::json(std::string body, int status) {
    Response r;
    r.status = status;
    r.body = std::move(body);
    r.headers.emplace_back("Cache-Control", "no-store");
    return r;
}

Response Response::text(int status, std::string message) {
    Response r;
    r.status = status;
    r.content_type = "text/plain; charset=utf-8";
    r.body = std::move(message) + "\n";
    return r;
}

const char* status_text(int status) {
    switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 408: return "Request Timeout";
        case 426: return "Upgrade Required";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------------

WebSocket::WebSocket(TcpSocket& socket, const std::atomic<bool>& stop)
    : socket_(socket), stop_(stop) {}

void WebSocket::post(std::string text) {
    if (!open_.load()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(text));
        while (queue_.size() > kMaxQueued) {
            queue_.pop_front();
            ++lost_;
        }
    }
    cv_.notify_one();
}

bool WebSocket::send_frame(std::uint8_t opcode, const std::string& payload) {
    std::string f;
    f += static_cast<char>(0x80U | opcode);  // FIN, unmasked (server to client)
    const std::size_t n = payload.size();
    if (n < 126) {
        f += static_cast<char>(n);
    } else if (n <= 0xFFFF) {
        f += static_cast<char>(126);
        f += static_cast<char>((n >> 8) & 0xFFU);
        f += static_cast<char>(n & 0xFFU);
    } else {
        f += static_cast<char>(127);
        for (int i = 7; i >= 0; --i) {
            f += static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xFFU);
        }
    }
    f += payload;
    return socket_.send_all(f);
}

/// Handles the frames received from the client. False when the connection ends.
bool WebSocket::handle_input() {
    while (input_.size() >= 2) {
        const auto b0 = static_cast<std::uint8_t>(input_[0]);
        const auto b1 = static_cast<std::uint8_t>(input_[1]);
        const std::uint8_t opcode = b0 & 0x0FU;
        const bool masked = (b1 & 0x80U) != 0;
        std::size_t len = b1 & 0x7FU;
        std::size_t pos = 2;
        if (!masked) return false;  // RFC 6455: client frames are always masked
        if (len == 126) {
            if (input_.size() < 4) return true;
            len = (static_cast<std::size_t>(static_cast<std::uint8_t>(input_[2])) << 8) |
                  static_cast<std::uint8_t>(input_[3]);
            pos = 4;
        } else if (len == 127) {
            return false;  // far beyond what a client may send here
        }
        if (len > kMaxClientFrame) return false;
        if (input_.size() < pos + 4 + len) return true;  // incomplete
        std::string payload = input_.substr(pos + 4, len);
        for (std::size_t i = 0; i < len; ++i) {
            payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^
                                           static_cast<std::uint8_t>(input_[pos + (i % 4)]));
        }
        input_.erase(0, pos + 4 + len);
        if (opcode == 0x8) {  // close: echo it, then end
            send_frame(0x8, payload.substr(0, 2));
            return false;
        }
        if (opcode == 0x9 && !send_frame(0xA, payload)) return false;  // ping → pong
        // Text, binary and pong frames from the client are ignored.
    }
    return true;
}

void WebSocket::run() {
    char buf[1024];
    while (!stop_.load()) {
        std::deque<std::string> out;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (queue_.empty()) cv_.wait_for(lock, std::chrono::milliseconds(50));
            out.swap(queue_);
        }
        bool ok = true;
        for (const auto& m : out) {
            if (!send_frame(0x1, m)) {
                ok = false;
                break;
            }
        }
        if (!ok) break;
        const long n = socket_.receive(buf, sizeof buf, 0);
        if (n == kClosed || n == kError) break;
        if (n > 0) {
            input_.append(buf, static_cast<std::size_t>(n));
            if (input_.size() > 4 * kMaxClientFrame || !handle_input()) break;
        }
    }
    if (stop_.load()) send_frame(0x8, std::string("\x03\xE9", 2));  // 1001 going away
    open_.store(false);
}

// ---------------------------------------------------------------------------------

HttpServer::HttpServer(Options options, Handler handler)
    : options_(std::move(options)),
      handler_(std::move(handler)),
      listener_(options_.bind, options_.port) {
    acceptor_ = std::thread([this] { accept_loop(); });
}

HttpServer::~HttpServer() {
    stop();
}

int HttpServer::port() const noexcept {
    return listener_.port();
}

void HttpServer::stop() {
    stop_.store(true);
    if (acceptor_.joinable()) acceptor_.join();
    std::vector<Connection> all;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        all.swap(connections_);
    }
    for (auto& c : all) {
        if (c.thread.joinable()) c.thread.join();
    }
}

void HttpServer::accept_loop() {
    while (!stop_.load()) {
        std::uint32_t peer = 0;
        TcpSocket s = listener_.accept(kPollMs, peer);
        {
            // Reap finished connections.
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = connections_.begin(); it != connections_.end();) {
                if (it->done->load()) {
                    it->thread.join();
                    it = connections_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!s.valid()) continue;
        const bool allowed = !options_.local_only || is_local_ipv4(peer);
        std::lock_guard<std::mutex> lock(mutex_);
        if (connections_.size() >= options_.max_connections) {
            Response busy = Response::text(503, "too many connections");
            s.set_send_timeout(1000);
            s.send_all(head_text(busy, busy.body.size(), true) + busy.body);
            continue;
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        Connection c;
        c.done = done;
        c.thread = std::thread([this, sock = std::move(s), peer, allowed, done]() mutable {
            serve(std::move(sock), ipv4_text(peer), allowed);
            done->store(true);
        });
        connections_.push_back(std::move(c));
    }
}

void HttpServer::serve(TcpSocket socket, std::string peer, bool allowed) {
    socket.set_send_timeout(kSendTimeoutMs);
    auto reply = [&](const Response& r, bool head_only) {
        socket.send_all(head_text(r, r.body.size(), true) + (head_only ? "" : r.body));
        socket.finish(1000);
    };
    // Read the request head.
    std::string data;
    char buf[2048];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
    std::size_t end = std::string::npos;
    while ((end = data.find("\r\n\r\n")) == std::string::npos) {
        if (data.size() > kMaxHeaderBytes) {
            reply(Response::text(431, "request head too large"), false);
            return;
        }
        if (stop_.load() || std::chrono::steady_clock::now() > deadline) {
            reply(Response::text(408, "request timeout"), false);
            return;
        }
        const long n = socket.receive(buf, sizeof buf, kPollMs);
        if (n == kClosed || n == kError) return;
        if (n > 0) data.append(buf, static_cast<std::size_t>(n));
    }
    if (end > kMaxHeaderBytes) {
        reply(Response::text(431, "request head too large"), false);
        return;
    }
    if (!allowed) {
        reply(Response::text(403, "only clients on the local network are served"), false);
        return;
    }
    Request req;
    req.peer = std::move(peer);
    if (const int status = parse_head(data.substr(0, end), req); status != 0) {
        reply(Response::text(status, status_text(status)), false);
        return;
    }
    // Request body (settings, login): Content-Length required, small.
    if (req.method == "POST" || req.method == "PUT" || req.method == "DELETE") {
        const std::string cl = req.header("content-length");
        if (cl.empty() && req.method != "DELETE") {
            reply(Response::text(411, "Content-Length required"), false);
            return;
        }
        char* stop = nullptr;
        const unsigned long long len = cl.empty() ? 0ULL : std::strtoull(cl.c_str(), &stop, 10);
        if (!cl.empty() && (stop == nullptr || *stop != '\0')) {
            reply(Response::text(400, "bad Content-Length"), false);
            return;
        }
        if (len > kMaxBodyBytes) {
            reply(Response::text(413, "request body too large"), false);
            return;
        }
        req.body = data.substr(end + 4);
        while (req.body.size() < len) {
            if (stop_.load() || std::chrono::steady_clock::now() > deadline) {
                reply(Response::text(408, "request timeout"), false);
                return;
            }
            const long n = socket.receive(buf, sizeof buf, kPollMs);
            if (n == kClosed || n == kError) return;
            if (n > 0) req.body.append(buf, static_cast<std::size_t>(n));
        }
        req.body.resize(static_cast<std::size_t>(len));
    }
    const bool head_only = req.method == "HEAD";
    Response resp;
    try {
        resp = handler_(req);
    } catch (const std::exception& e) {
        resp = Response::text(500, e.what());
    }
    if (!resp.websocket) {
        reply(resp, head_only);
        return;
    }
    // WebSocket upgrade (RFC 6455 §4.2).
    const std::string key = req.header("sec-websocket-key");
    if (!has_token(req.header("upgrade"), "websocket") ||
        !has_token(req.header("connection"), "upgrade") || key.empty() ||
        req.header("sec-websocket-version") != "13") {
        Response r = Response::text(426, "WebSocket upgrade expected");
        r.headers.emplace_back("Sec-WebSocket-Version", "13");
        reply(r, head_only);
        return;
    }
    const std::string handshake =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        websocket_accept(key) + "\r\n\r\n";
    if (!socket.send_all(handshake)) return;
    WebSocket ws(socket, stop_);
    resp.websocket(ws);
}

}  // namespace owfax::net
