// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Grayscale PNG (spec IMG-003): 8-bit lossless, or 4-bit (16 levels) for display.
// Each scanline gets the PNG filter (None, Sub, Up, Average, Paeth) with the smallest
// sum of absolute residuals, then the image is deflate-compressed (owfax/image/zlib.hpp).
// Real received charts carry FM noise in every grey level (~5 bits of entropy per
// pixel): lossless gains only ~1.3x there (zlib level 9 does ~5 % better), clean
// drawings ~12x; the 16-level version gains ~3.5x on real charts.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "owfax/image/gray_image.hpp"
#include "owfax/image/zlib.hpp"

namespace owfax {
namespace detail {

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1U)));
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    return ~crc;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    while (size > 0) {
        const std::size_t n = size < 5552 ? size : 5552;  // no overflow before the modulo
        for (std::size_t i = 0; i < n; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521U;
        b %= 65521U;
        data += n;
        size -= n;
    }
    return (b << 16) | a;
}

}  // namespace detail

namespace {

std::uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<std::uint8_t>(a);
    if (pb <= pc) return static_cast<std::uint8_t>(b);
    return static_cast<std::uint8_t>(c);
}

/// Filters one scanline (bpp = 1). `prev` is the previous raw scanline (zeros at top).
void filter_row(int type, const std::uint8_t* cur, const std::uint8_t* prev, std::size_t w,
                std::uint8_t* out) {
    for (std::size_t x = 0; x < w; ++x) {
        const int a = x > 0 ? cur[x - 1] : 0;
        const int b = prev[x];
        const int c = x > 0 ? prev[x - 1] : 0;
        int pred = 0;
        switch (type) {
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: pred = paeth(a, b, c); break;
            default: pred = 0;
        }
        out[x] = static_cast<std::uint8_t>((cur[x] - pred) & 0xFF);
    }
}

void unfilter_row(int type, std::uint8_t* cur, const std::uint8_t* prev, std::size_t w) {
    for (std::size_t x = 0; x < w; ++x) {
        const int a = x > 0 ? cur[x - 1] : 0;
        const int b = prev[x];
        const int c = x > 0 ? prev[x - 1] : 0;
        int pred = 0;
        switch (type) {
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: pred = paeth(a, b, c); break;
            default: pred = 0;
        }
        cur[x] = static_cast<std::uint8_t>((cur[x] + pred) & 0xFF);
    }
}

void put_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

std::uint32_t get_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

void put_chunk(std::vector<std::uint8_t>& out, const char* type,
               const std::vector<std::uint8_t>& body) {
    put_be32(out, static_cast<std::uint32_t>(body.size()));
    const std::size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    put_be32(out, detail::crc32(out.data() + crc_start, out.size() - crc_start));
}

}  // namespace

static std::uint8_t to_4bit(std::uint8_t v) {
    return static_cast<std::uint8_t>((v * 15 + 127) / 255);  // nearest of 16 levels
}

std::vector<std::uint8_t> encode_png(const GrayImage& image, int bit_depth) {
    if (image.width == 0 || image.height == 0) throw std::runtime_error("write_png: empty image");
    if (bit_depth != 8 && bit_depth != 4) throw std::invalid_argument("PNG bit depth: 8 or 4");
    const std::size_t w = image.width;
    const std::size_t row_bytes = bit_depth == 8 ? w : (w + 1) / 2;
    // Packed scanlines (two pixels per byte at 4 bits, high nibble first).
    std::vector<std::uint8_t> packed(row_bytes * image.height, 0);
    for (std::size_t y = 0; y < image.height; ++y) {
        const std::uint8_t* src = &image.pixels[y * w];
        std::uint8_t* dst = &packed[y * row_bytes];
        if (bit_depth == 8) {
            std::copy(src, src + w, dst);
        } else {
            for (std::size_t x = 0; x < w; ++x) {
                const int shift = x % 2 == 0 ? 4 : 0;
                dst[x / 2] = static_cast<std::uint8_t>(dst[x / 2] | (to_4bit(src[x]) << shift));
            }
        }
    }
    // PNG filters work on bytes, with the previous byte as left neighbour (bpp = 1).
    std::vector<std::uint8_t> filtered((row_bytes + 1) * image.height);
    std::vector<std::uint8_t> zeros(row_bytes, 0);
    std::vector<std::uint8_t> candidate(row_bytes);
    for (std::size_t y = 0; y < image.height; ++y) {
        const std::uint8_t* cur = &packed[y * row_bytes];
        const std::uint8_t* prev = y > 0 ? &packed[(y - 1) * row_bytes] : zeros.data();
        std::uint8_t* out = &filtered[y * (row_bytes + 1)];
        std::uint64_t best = ~std::uint64_t{0};
        for (int type = 0; type < 5; ++type) {
            filter_row(type, cur, prev, row_bytes, candidate.data());
            std::uint64_t cost = 0;
            for (std::uint8_t v : candidate) {
                cost += static_cast<std::uint64_t>(v < 128 ? v : 256 - v);
            }
            if (cost < best) {
                best = cost;
                out[0] = static_cast<std::uint8_t>(type);
                std::copy(candidate.begin(), candidate.end(), out + 1);
            }
        }
    }
    std::vector<std::uint8_t> ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(image.width));
    put_be32(ihdr, static_cast<std::uint32_t>(image.height));
    // Bit depth, grayscale, deflate, adaptive filters, no interlace.
    ihdr.insert(ihdr.end(), {static_cast<std::uint8_t>(bit_depth), 0, 0, 0, 0});

    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    put_chunk(out, "IHDR", ihdr);
    put_chunk(out, "IDAT", zlib_compress(filtered.data(), filtered.size()));
    put_chunk(out, "IEND", {});
    return out;
}

void write_png(const std::string& path, const GrayImage& image, int bit_depth) {
    const auto out = encode_png(image, bit_depth);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) throw std::runtime_error("cannot write " + path);
}

GrayImage decode_png(const std::uint8_t* data, std::size_t size) {
    static const std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (size < 8 || !std::equal(kSig, kSig + 8, data)) throw std::runtime_error("not a PNG file");
    std::size_t pos = 8;
    std::size_t w = 0;
    std::size_t h = 0;
    int depth = 0;
    std::vector<std::uint8_t> idat;
    bool end = false;
    while (!end) {
        if (pos + 12 > size) throw std::runtime_error("truncated PNG");
        const std::size_t len = get_be32(data + pos);
        if (len > size - pos - 12) throw std::runtime_error("truncated PNG chunk");
        const std::uint8_t* type = data + pos + 4;
        const std::uint8_t* body = data + pos + 8;
        if (get_be32(body + len) != detail::crc32(type, len + 4)) {
            throw std::runtime_error("PNG chunk CRC mismatch");
        }
        const std::string t(reinterpret_cast<const char*>(type), 4);
        if (t == "IHDR") {
            if (len != 13) throw std::runtime_error("bad PNG header");
            w = get_be32(body);
            h = get_be32(body + 4);
            depth = body[8];
            if ((depth != 8 && depth != 4) || body[9] != 0 || body[10] != 0 || body[11] != 0 ||
                body[12] != 0) {
                throw std::runtime_error("only 8- or 4-bit grayscale non-interlaced PNG");
            }
            if (w == 0 || h == 0 || w > 100000 || h > 100000) {
                throw std::runtime_error("unsupported PNG size");
            }
        } else if (t == "IDAT") {
            idat.insert(idat.end(), body, body + len);
        } else if (t == "IEND") {
            end = true;
        }
        pos += len + 12;
    }
    if (w == 0) throw std::runtime_error("PNG without header");
    const std::size_t row_bytes = depth == 8 ? w : (w + 1) / 2;
    const std::size_t expected = (row_bytes + 1) * h;
    auto raw = zlib_decompress(idat.data(), idat.size(), expected);
    if (!raw || raw->size() != expected) throw std::runtime_error("bad PNG image data");
    std::vector<std::uint8_t> rows(row_bytes * h);
    std::vector<std::uint8_t> zeros(row_bytes, 0);
    for (std::size_t y = 0; y < h; ++y) {
        const std::uint8_t* row = &(*raw)[y * (row_bytes + 1)];
        if (row[0] > 4) throw std::runtime_error("bad PNG filter");
        std::uint8_t* cur = &rows[y * row_bytes];
        std::copy(row + 1, row + 1 + row_bytes, cur);
        unfilter_row(row[0], cur, y > 0 ? &rows[(y - 1) * row_bytes] : zeros.data(), row_bytes);
    }
    GrayImage img(w, h, 0);
    for (std::size_t y = 0; y < h; ++y) {
        const std::uint8_t* src = &rows[y * row_bytes];
        std::uint8_t* dst = &img.pixels[y * w];
        if (depth == 8) {
            std::copy(src, src + w, dst);
        } else {
            for (std::size_t x = 0; x < w; ++x) {
                const unsigned v = (src[x / 2] >> (x % 2 == 0 ? 4 : 0)) & 0x0FU;
                dst[x] = static_cast<std::uint8_t>(v * 17U);  // 0..15 to 0..255
            }
        }
    }
    return img;
}

GrayImage read_png(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    return decode_png(data.data(), data.size());
}

}  // namespace owfax
