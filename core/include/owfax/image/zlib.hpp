// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Dependency-free zlib / deflate (RFC 1950, RFC 1951) for PNG images (spec IMG-003):
// a compressor (LZ77 + dynamic Huffman blocks) and a decompressor, used to read PNG
// files back and to test the compressor.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace owfax {

/// zlib stream of `data` (deflate with dynamic Huffman codes, Adler-32 trailer).
std::vector<std::uint8_t> zlib_compress(const std::uint8_t* data, std::size_t size);

/// Decompresses a zlib stream. Empty optional if malformed, if the Adler-32 does not
/// match, or if the output would exceed `max_output` bytes.
std::optional<std::vector<std::uint8_t>> zlib_decompress(const std::uint8_t* data, std::size_t size,
                                                         std::size_t max_output);

}  // namespace owfax
