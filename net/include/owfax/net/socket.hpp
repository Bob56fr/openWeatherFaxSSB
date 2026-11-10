// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Minimal TCP sockets (IPv4) over Winsock (Windows 7+) and BSD sockets (Linux,
// Android). Every wait has a timeout, so that threads can always be stopped.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace owfax::net {

#if defined(_WIN32)
using native_socket = std::uintptr_t;  // SOCKET
#else
using native_socket = int;
#endif

/// Result of TcpSocket::receive(): bytes read (> 0), or one of these.
enum : long { kClosed = 0, kError = -1, kTimeout = -2 };

class TcpSocket {
public:
    TcpSocket() noexcept;
    explicit TcpSocket(native_socket s) noexcept;
    ~TcpSocket();
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    /// Connects to an IPv4 address ("127.0.0.1"). Throws std::runtime_error.
    static TcpSocket connect(const std::string& ipv4, int port);

    bool valid() const noexcept;
    /// Sends everything (blocking, bounded by the send timeout). False on error.
    bool send_all(const void* data, std::size_t size);
    bool send_all(const std::string& data) { return send_all(data.data(), data.size()); }
    /// Waits up to `timeout_ms` for data, then reads what is available.
    long receive(void* buffer, std::size_t size, int timeout_ms);
    /// Gives up on a send that blocks longer than this (a stalled client).
    void set_send_timeout(int timeout_ms);
    /// Graceful close: no more sending, then discard what the client still sends for
    /// up to `timeout_ms`, so that it reads the whole answer instead of a reset.
    void finish(int timeout_ms);
    void close() noexcept;

private:
    native_socket s_;
};

/// Listening socket on an IPv4 address ("0.0.0.0" = every interface).
class TcpListener {
public:
    /// `port` 0 picks a free port (see port()). Throws std::runtime_error.
    TcpListener(const std::string& ipv4, int port);
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    int port() const noexcept { return port_; }
    /// Waits up to `timeout_ms` for a client. Invalid socket on timeout.
    /// `peer_ipv4` receives the client address in network byte order.
    TcpSocket accept(int timeout_ms, std::uint32_t& peer_ipv4);

private:
    native_socket s_;
    int port_ = 0;
};

/// Dotted text of an IPv4 address in network byte order.
std::string ipv4_text(std::uint32_t address_be);

/// True for the addresses of a local network only (spec NFR-006): loopback,
/// 10/8, 172.16/12, 192.168/16 and link-local 169.254/16.
bool is_local_ipv4(std::uint32_t address_be) noexcept;

}  // namespace owfax::net
