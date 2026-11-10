// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Minimal HTTP/1.1 server with WebSocket push, for the local web interface
// (spec ARC-002, DSP-007). Deliberately small and read-only for now: GET and HEAD
// only, bounded request size and time, a fixed number of connections, and clients
// accepted from local network addresses only (NFR-006). One thread per connection.
//
// Written for the project rather than taken from a library so that it builds on
// every target of spec §2.1, including Windows 7 and MinGW.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "owfax/net/socket.hpp"

namespace owfax::net {

struct Request {
    std::string method;                          ///< "GET" or "HEAD".
    std::string path;                            ///< Percent-decoded, starts with '/'.
    std::string query;                           ///< Raw text after '?', if any.
    std::map<std::string, std::string> headers;  ///< Names in lower case.
    std::string peer;                            ///< Client IPv4 address.

    std::string header(const std::string& lower_name) const;
};

class WebSocket;

struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    /// Set to accept a WebSocket upgrade: called on the connection thread once the
    /// handshake is done, it returns when the client leaves or the server stops.
    std::function<void(WebSocket&)> websocket;

    static Response json(std::string body, int status = 200);
    static Response text(int status, std::string message);
};

/// Server side of a WebSocket (RFC 6455): text messages from server to client.
/// post() may be called from any thread; the connection thread does the sending,
/// so a slow client never blocks the caller.
class WebSocket {
public:
    WebSocket(TcpSocket& socket, const std::atomic<bool>& stop);

    /// Queues a text message. When more than kMaxQueued are waiting (client too slow),
    /// the oldest are dropped and lost() counts them.
    void post(std::string text);
    /// Runs the connection until the client closes, an error, or the server stops.
    void run();
    bool open() const noexcept { return open_.load(); }
    std::size_t lost() const noexcept { return lost_.load(); }

    static constexpr std::size_t kMaxQueued = 512;

private:
    bool send_frame(std::uint8_t opcode, const std::string& payload);
    bool handle_input();

    TcpSocket& socket_;
    const std::atomic<bool>& stop_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::string input_;
    std::atomic<bool> open_{true};
    std::atomic<std::size_t> lost_{0};
};

class HttpServer {
public:
    using Handler = std::function<Response(const Request&)>;

    struct Options {
        std::string bind = "0.0.0.0";  ///< IPv4 address to listen on.
        int port = 8080;               ///< 0 = any free port (tests).
        std::size_t max_connections = 32;
        bool local_only = true;  ///< Refuse clients outside the local network (NFR-006).
    };

    /// Starts listening and serving at once. Throws std::runtime_error.
    HttpServer(Options options, Handler handler);
    ~HttpServer();  ///< Stops and joins every thread.
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    int port() const noexcept;
    void stop();

private:
    struct Connection {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    void accept_loop();
    void serve(TcpSocket socket, std::string peer, bool allowed);

    Options options_;
    Handler handler_;
    TcpListener listener_;
    std::atomic<bool> stop_{false};
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<Connection> connections_;
};

/// Reason phrase of an HTTP status code.
const char* status_text(int status);

}  // namespace owfax::net
