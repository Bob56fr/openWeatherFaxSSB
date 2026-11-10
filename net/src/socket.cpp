// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/net/socket.hpp"

#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace owfax::net {
namespace {

#if defined(_WIN32)
constexpr native_socket kInvalid = static_cast<native_socket>(INVALID_SOCKET);

void startup() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    });
}

SOCKET raw(native_socket s) {
    return static_cast<SOCKET>(s);
}

void close_raw(native_socket s) {
    closesocket(raw(s));
}

/// Waits for `s` to become readable. 1 ready, 0 timeout, -1 error.
int wait_readable(native_socket s, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(raw(s), &set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int r = select(0, &set, nullptr, nullptr, &tv);
    return r > 0 ? 1 : (r == 0 ? 0 : -1);
}
#else
constexpr native_socket kInvalid = -1;

void startup() {}

int raw(native_socket s) {
    return s;
}

void close_raw(native_socket s) {
    ::close(s);
}

int wait_readable(native_socket s, int timeout_ms) {
    pollfd p{};
    p.fd = s;
    p.events = POLLIN;
    const int r = ::poll(&p, 1, timeout_ms);
    return r > 0 ? 1 : (r == 0 ? 0 : -1);
}
#endif

std::runtime_error socket_error(const std::string& what) {
    return std::runtime_error(what);
}

}  // namespace

TcpSocket::TcpSocket() noexcept : s_(kInvalid) {}

TcpSocket::TcpSocket(native_socket s) noexcept : s_(s) {}

TcpSocket::~TcpSocket() {
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : s_(other.s_) {
    other.s_ = kInvalid;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        s_ = other.s_;
        other.s_ = kInvalid;
    }
    return *this;
}

TcpSocket TcpSocket::connect(const std::string& ipv4, int port) {
    startup();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, ipv4.c_str(), &addr.sin_addr) != 1) {
        throw socket_error("invalid IPv4 address " + ipv4);
    }
    const auto s = static_cast<native_socket>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s == kInvalid) throw socket_error("socket() failed");
    TcpSocket sock(s);
    if (::connect(raw(s), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        throw socket_error("cannot connect to " + ipv4 + ":" + std::to_string(port));
    }
    return sock;
}

bool TcpSocket::valid() const noexcept {
    return s_ != kInvalid;
}

bool TcpSocket::send_all(const void* data, std::size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        const int chunk = static_cast<int>(size > 65536 ? 65536 : size);
#if defined(_WIN32)
        const int n = ::send(raw(s_), p, chunk, 0);
#elif defined(MSG_NOSIGNAL)
        const auto n = ::send(raw(s_), p, static_cast<std::size_t>(chunk), MSG_NOSIGNAL);
#else
        const auto n = ::send(raw(s_), p, static_cast<std::size_t>(chunk), 0);
#endif
        if (n <= 0) return false;
        p += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

long TcpSocket::receive(void* buffer, std::size_t size, int timeout_ms) {
    const int ready = wait_readable(s_, timeout_ms);
    if (ready == 0) return kTimeout;
    if (ready < 0) return kError;
    const int chunk = static_cast<int>(size > 65536 ? 65536 : size);
#if defined(_WIN32)
    const int n = ::recv(raw(s_), static_cast<char*>(buffer), chunk, 0);
#else
    const auto n = ::recv(raw(s_), buffer, static_cast<std::size_t>(chunk), 0);
#endif
    if (n < 0) return kError;
    return static_cast<long>(n);
}

void TcpSocket::set_send_timeout(int timeout_ms) {
#if defined(_WIN32)
    const DWORD v = static_cast<DWORD>(timeout_ms);
    setsockopt(raw(s_), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
#else
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(raw(s_), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

void TcpSocket::finish(int timeout_ms) {
    if (s_ == kInvalid) return;
#if defined(_WIN32)
    ::shutdown(raw(s_), SD_SEND);
#else
    ::shutdown(raw(s_), SHUT_WR);
#endif
    char buf[1024];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const long n = receive(buf, sizeof buf, 50);
        if (n == kClosed || n == kError) break;
    }
    close();
}

void TcpSocket::close() noexcept {
    if (s_ != kInvalid) {
        close_raw(s_);
        s_ = kInvalid;
    }
}

TcpListener::TcpListener(const std::string& ipv4, int port) : s_(kInvalid) {
    startup();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, ipv4.c_str(), &addr.sin_addr) != 1) {
        throw socket_error("invalid IPv4 address " + ipv4);
    }
    s_ = static_cast<native_socket>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s_ == kInvalid) throw socket_error("socket() failed");
#if !defined(_WIN32)
    // Restart without waiting for TIME_WAIT. (On Windows SO_REUSEADDR would let
    // another program steal the port: not set.)
    const int one = 1;
    setsockopt(raw(s_), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (::bind(raw(s_), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(raw(s_), 16) != 0) {
        close_raw(s_);
        s_ = kInvalid;
        throw socket_error("cannot listen on " + ipv4 + ":" + std::to_string(port) +
                           " (port in use?)");
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int len = sizeof bound;
#else
    socklen_t len = sizeof bound;
#endif
    getsockname(raw(s_), reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.sin_port);
}

TcpListener::~TcpListener() {
    if (s_ != kInvalid) close_raw(s_);
}

TcpSocket TcpListener::accept(int timeout_ms, std::uint32_t& peer_ipv4) {
    if (wait_readable(s_, timeout_ms) != 1) return TcpSocket();
    sockaddr_in peer{};
#if defined(_WIN32)
    int len = sizeof peer;
#else
    socklen_t len = sizeof peer;
#endif
    const auto c =
        static_cast<native_socket>(::accept(raw(s_), reinterpret_cast<sockaddr*>(&peer), &len));
    if (c == kInvalid) return TcpSocket();
    peer_ipv4 = peer.sin_addr.s_addr;
    return TcpSocket(c);
}

std::string ipv4_text(std::uint32_t address_be) {
    const auto* b = reinterpret_cast<const unsigned char*>(&address_be);
    return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
           std::to_string(b[3]);
}

bool is_local_ipv4(std::uint32_t address_be) noexcept {
    const auto* b = reinterpret_cast<const unsigned char*>(&address_be);
    if (b[0] == 127 || b[0] == 10) return true;
    if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return true;
    if (b[0] == 192 && b[1] == 168) return true;
    if (b[0] == 169 && b[1] == 254) return true;
    return false;
}

}  // namespace owfax::net
