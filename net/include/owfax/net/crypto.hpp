// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Password protection of the settings (spec UI-002, NFR-006): SHA-256 (FIPS 180-4),
// HMAC-SHA256 (RFC 2104), PBKDF2-HMAC-SHA256 (RFC 8018), and random bytes from the
// operating system for salts and session tokens. Dependency-free, checked against
// the published test vectors.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace owfax::net {

std::array<std::uint8_t, 32> sha256(std::string_view data);

std::array<std::uint8_t, 32> hmac_sha256(std::string_view key, std::string_view data);

/// Derived key of `length` bytes.
std::vector<std::uint8_t> pbkdf2_sha256(std::string_view password, std::string_view salt,
                                        std::uint32_t iterations, std::size_t length);

/// Cryptographically secure random bytes from the operating system. Throws
/// std::runtime_error if the system source is unavailable (never falls back to a
/// predictable generator).
std::vector<std::uint8_t> secure_random(std::size_t count);

std::string to_hex(const std::uint8_t* data, std::size_t size);
std::vector<std::uint8_t> from_hex(std::string_view hex);  ///< empty if malformed

/// Comparison whose duration does not depend on where the inputs differ.
bool constant_time_equal(std::string_view a, std::string_view b) noexcept;

}  // namespace owfax::net
