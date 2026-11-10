// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Small encodings needed by the local HTTP/WebSocket server (spec ARC-002, DSP-007).

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace owfax::net {

/// SHA-1 digest (RFC 3174). Only used for the WebSocket handshake, never for security.
std::array<std::uint8_t, 20> sha1(std::string_view data);

/// Standard base64 with padding (RFC 4648).
std::string base64_encode(const std::uint8_t* data, std::size_t size);
std::string base64_encode(std::string_view data);

/// Sec-WebSocket-Accept value for a Sec-WebSocket-Key (RFC 6455 §4.2.2).
std::string websocket_accept(std::string_view key);

/// Percent-decoding of a URL path ("%2F" → "/"). Empty optional if malformed or if
/// the result holds a NUL byte.
std::optional<std::string> url_decode(std::string_view in);

}  // namespace owfax::net
