// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors
//
// Compressor: greedy LZ77 over a 32 KiB window (hash chains, bounded search), then
// one dynamic-Huffman deflate block per 64 Ki symbols, code lengths limited to 15 bits
// (7 for the code-length code). Every Huffman tree is complete (at least two codes),
// which all decoders accept. Decompressor: canonical Huffman decoding of stored, fixed
// and dynamic blocks with full bounds checks.

#include "owfax/image/zlib.hpp"

#include <algorithm>
#include <array>
#include <queue>
#include <utility>

#include "owfax/image/gray_image.hpp"

namespace owfax {
namespace {

constexpr std::array<std::uint16_t, 29> kLengthBase = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                       15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                       67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistBase = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistExtra = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                                     4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                                     9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr std::array<std::uint8_t, 19> kClOrder = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                   11, 4,  12, 3, 13, 2, 14, 1, 15};

constexpr std::size_t kWindow = 32768;
constexpr std::size_t kMinMatch = 3;
constexpr std::size_t kMaxMatch = 258;
constexpr int kMaxChain = 48;
constexpr std::size_t kHashBits = 15;
constexpr std::size_t kBlockSymbols = 65536;

// ---------------------------------------------------------------- compressor

struct Symbol {
    std::uint16_t litlen;  ///< literal byte, or match length 3..258
    std::uint16_t dist;    ///< 0 for a literal
};

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& out) : out_(out) {}
    void put(std::uint32_t value, int bits) {
        acc_ |= static_cast<std::uint64_t>(value) << n_;
        n_ += bits;
        while (n_ >= 8) {
            out_.push_back(static_cast<std::uint8_t>(acc_ & 0xFFU));
            acc_ >>= 8;
            n_ -= 8;
        }
    }
    /// Huffman codes are sent most significant bit first.
    void put_code(std::uint32_t code, int bits) {
        std::uint32_t r = 0;
        for (int i = 0; i < bits; ++i) r |= ((code >> i) & 1U) << (bits - 1 - i);
        put(r, bits);
    }
    void flush() {
        if (n_ > 0) out_.push_back(static_cast<std::uint8_t>(acc_ & 0xFFU));
        acc_ = 0;
        n_ = 0;
    }

private:
    std::vector<std::uint8_t>& out_;
    std::uint64_t acc_ = 0;
    int n_ = 0;
};

std::size_t length_code(std::size_t len) {
    std::size_t c = 0;
    while (c + 1 < kLengthBase.size() && kLengthBase[c + 1] <= len) ++c;
    return c;
}

std::size_t dist_code(std::size_t dist) {
    std::size_t c = 0;
    while (c + 1 < kDistBase.size() && kDistBase[c + 1] <= dist) ++c;
    return c;
}

/// Huffman code lengths for `freq`, at most `max_bits`. Frequencies are halved until
/// the tree fits (always terminates: equal weights give a balanced tree).
std::vector<std::uint8_t> code_lengths(std::vector<std::uint32_t> freq, int max_bits) {
    const std::size_t n = freq.size();
    std::vector<std::uint8_t> len(n, 0);
    while (true) {
        std::vector<std::size_t> used;
        for (std::size_t i = 0; i < n; ++i) {
            if (freq[i] > 0) used.push_back(i);
        }
        std::fill(len.begin(), len.end(), std::uint8_t{0});
        if (used.size() == 1) {
            len[used[0]] = 1;
            return len;
        }
        if (used.empty()) return len;
        // Nodes 0..n-1 are leaves; internal nodes follow.
        std::vector<std::size_t> parent(2 * n, 0);
        using Item = std::pair<std::uint64_t, std::size_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        for (std::size_t i : used) pq.emplace(freq[i], i);
        std::size_t next = n;
        while (pq.size() > 1) {
            const Item a = pq.top();
            pq.pop();
            const Item b = pq.top();
            pq.pop();
            parent[a.second] = next;
            parent[b.second] = next;
            pq.emplace(a.first + b.first, next);
            ++next;
        }
        const std::size_t root = next - 1;
        int deepest = 0;
        for (std::size_t i : used) {
            int d = 0;
            for (std::size_t k = i; k != root; k = parent[k]) ++d;
            len[i] = static_cast<std::uint8_t>(d);
            deepest = std::max(deepest, d);
        }
        if (deepest <= max_bits) return len;
        for (std::size_t i : used) freq[i] = std::max<std::uint32_t>(1, freq[i] / 2);
    }
}

/// Canonical codes (RFC 1951 §3.2.2).
std::vector<std::uint32_t> canonical_codes(const std::vector<std::uint8_t>& len) {
    std::array<std::uint32_t, 16> count{};
    for (auto l : len) ++count[l];
    count[0] = 0;
    std::array<std::uint32_t, 16> next{};
    std::uint32_t code = 0;
    for (std::size_t bits = 1; bits < 16; ++bits) {
        code = (code + count[bits - 1]) << 1;
        next[bits] = code;
    }
    std::vector<std::uint32_t> codes(len.size(), 0);
    for (std::size_t i = 0; i < len.size(); ++i) {
        if (len[i] != 0) codes[i] = next[len[i]]++;
    }
    return codes;
}

/// Run-length coding of the code lengths with symbols 16, 17, 18 (RFC 1951 §3.2.7).
/// Each entry: symbol, extra-bit value.
std::vector<std::pair<std::uint8_t, std::uint8_t>> rle_lengths(
    const std::vector<std::uint8_t>& lens) {
    std::vector<std::pair<std::uint8_t, std::uint8_t>> out;
    std::size_t i = 0;
    while (i < lens.size()) {
        const std::uint8_t v = lens[i];
        std::size_t run = 1;
        while (i + run < lens.size() && lens[i + run] == v) ++run;
        if (v == 0 && run >= 3) {
            std::size_t left = run;
            while (left >= 11) {
                const std::size_t k = std::min<std::size_t>(left, 138);
                out.emplace_back(18, static_cast<std::uint8_t>(k - 11));
                left -= k;
            }
            if (left >= 3) {
                out.emplace_back(17, static_cast<std::uint8_t>(left - 3));
                left = 0;
            }
            for (; left > 0; --left) out.emplace_back(0, 0);
        } else if (v != 0 && run >= 4) {
            out.emplace_back(v, 0);
            std::size_t left = run - 1;
            while (left >= 3) {
                const std::size_t k = std::min<std::size_t>(left, 6);
                out.emplace_back(16, static_cast<std::uint8_t>(k - 3));
                left -= k;
            }
            for (; left > 0; --left) out.emplace_back(v, 0);
        } else {
            for (std::size_t k = 0; k < run; ++k) out.emplace_back(v, 0);
        }
        i += run;
    }
    return out;
}

void write_block(BitWriter& bw, const std::vector<Symbol>& syms, bool last) {
    std::vector<std::uint32_t> lit_freq(286, 0);
    std::vector<std::uint32_t> dist_freq(30, 0);
    for (const auto& s : syms) {
        if (s.dist == 0) {
            ++lit_freq[s.litlen];
        } else {
            ++lit_freq[257 + length_code(s.litlen)];
            ++dist_freq[dist_code(s.dist)];
        }
    }
    lit_freq[256] = 1;  // end of block
    // Complete trees only: at least two codes in each.
    if (std::count_if(lit_freq.begin(), lit_freq.end(), [](auto f) { return f > 0; }) < 2) {
        lit_freq[0] = std::max<std::uint32_t>(lit_freq[0], 1);
    }
    if (std::count_if(dist_freq.begin(), dist_freq.end(), [](auto f) { return f > 0; }) < 2) {
        dist_freq[0] = std::max<std::uint32_t>(dist_freq[0], 1);
        dist_freq[1] = std::max<std::uint32_t>(dist_freq[1], 1);
    }
    const auto lit_len = code_lengths(lit_freq, 15);
    const auto dist_len = code_lengths(dist_freq, 15);
    const auto lit_code = canonical_codes(lit_len);
    const auto dist_code_v = canonical_codes(dist_len);

    std::size_t hlit = 286;
    while (hlit > 257 && lit_len[hlit - 1] == 0) --hlit;
    std::size_t hdist = 30;
    while (hdist > 1 && dist_len[hdist - 1] == 0) --hdist;
    std::vector<std::uint8_t> all(lit_len.begin(), lit_len.begin() + static_cast<long>(hlit));
    all.insert(all.end(), dist_len.begin(), dist_len.begin() + static_cast<long>(hdist));
    const auto rle = rle_lengths(all);
    std::vector<std::uint32_t> cl_freq(19, 0);
    for (const auto& r : rle) ++cl_freq[r.first];
    if (std::count_if(cl_freq.begin(), cl_freq.end(), [](auto f) { return f > 0; }) < 2) {
        cl_freq[cl_freq[0] > 0 ? 1 : 0] = 1;
    }
    const auto cl_len = code_lengths(cl_freq, 7);
    const auto cl_code = canonical_codes(cl_len);
    std::size_t hclen = 19;
    while (hclen > 4 && cl_len[kClOrder[hclen - 1]] == 0) --hclen;

    bw.put(last ? 1U : 0U, 1);
    bw.put(2, 2);  // dynamic Huffman
    bw.put(static_cast<std::uint32_t>(hlit - 257), 5);
    bw.put(static_cast<std::uint32_t>(hdist - 1), 5);
    bw.put(static_cast<std::uint32_t>(hclen - 4), 4);
    for (std::size_t i = 0; i < hclen; ++i) bw.put(cl_len[kClOrder[i]], 3);
    for (const auto& r : rle) {
        bw.put_code(cl_code[r.first], cl_len[r.first]);
        if (r.first == 16) bw.put(r.second, 2);
        if (r.first == 17) bw.put(r.second, 3);
        if (r.first == 18) bw.put(r.second, 7);
    }
    for (const auto& s : syms) {
        if (s.dist == 0) {
            bw.put_code(lit_code[s.litlen], lit_len[s.litlen]);
            continue;
        }
        const std::size_t lc = length_code(s.litlen);
        bw.put_code(lit_code[257 + lc], lit_len[257 + lc]);
        if (kLengthExtra[lc] > 0) {
            bw.put(static_cast<std::uint32_t>(s.litlen - kLengthBase[lc]), kLengthExtra[lc]);
        }
        const std::size_t dc = dist_code(s.dist);
        bw.put_code(dist_code_v[dc], dist_len[dc]);
        if (kDistExtra[dc] > 0) {
            bw.put(static_cast<std::uint32_t>(s.dist - kDistBase[dc]), kDistExtra[dc]);
        }
    }
    bw.put_code(lit_code[256], lit_len[256]);
}

// -------------------------------------------------------------- decompressor

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : p_(data), size_(size) {}
    /// Reads `n` bits (LSB first); sets the error flag past the end.
    std::uint32_t bits(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (byte_ >= size_) {
                error = true;
                return 0;
            }
            v |= static_cast<std::uint32_t>((p_[byte_] >> bit_) & 1U) << i;
            if (++bit_ == 8) {
                bit_ = 0;
                ++byte_;
            }
        }
        return v;
    }
    void align() {
        if (bit_ != 0) {
            bit_ = 0;
            ++byte_;
        }
    }
    std::size_t position() const { return byte_; }
    void skip(std::size_t n) { byte_ += n; }
    bool error = false;

private:
    const std::uint8_t* p_;
    std::size_t size_;
    std::size_t byte_ = 0;
    int bit_ = 0;
};

struct Huffman {
    std::array<std::uint16_t, 16> count{};
    std::vector<std::uint16_t> symbol;
};

/// False if the lengths over-subscribe the code space.
bool build(Huffman& h, const std::uint8_t* len, std::size_t n) {
    h.count.fill(0);
    for (std::size_t i = 0; i < n; ++i) ++h.count[len[i]];
    if (h.count[0] == n) {
        h.symbol.clear();
        return true;
    }
    int left = 1;
    for (std::size_t b = 1; b < 16; ++b) {
        left = left * 2 - h.count[b];
        if (left < 0) return false;
    }
    std::array<std::uint16_t, 16> offs{};
    for (std::size_t b = 1; b < 15; ++b) {
        offs[b + 1] = static_cast<std::uint16_t>(offs[b] + h.count[b]);
    }
    h.symbol.assign(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (len[i] != 0) h.symbol[offs[len[i]]++] = static_cast<std::uint16_t>(i);
    }
    return true;
}

/// Decodes one symbol; -1 on error.
int decode(BitReader& br, const Huffman& h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (std::size_t len = 1; len < 16; ++len) {
        code |= static_cast<int>(br.bits(1));
        if (br.error) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[static_cast<std::size_t>(index + (code - first))];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

bool inflate_codes(BitReader& br, const Huffman& lit, const Huffman& dist,
                   std::vector<std::uint8_t>& out, std::size_t max_output) {
    while (true) {
        const int sym = decode(br, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= max_output) return false;
            out.push_back(static_cast<std::uint8_t>(sym));
            continue;
        }
        if (sym == 256) return true;
        const auto lc = static_cast<std::size_t>(sym - 257);
        if (lc >= kLengthBase.size()) return false;
        const std::size_t len = kLengthBase[lc] + br.bits(kLengthExtra[lc]);
        const int ds = decode(br, dist);
        if (ds < 0 || ds >= 30) return false;
        const auto dc = static_cast<std::size_t>(ds);
        const std::size_t d = kDistBase[dc] + br.bits(kDistExtra[dc]);
        if (br.error || d > out.size() || out.size() + len > max_output) return false;
        const std::size_t from = out.size() - d;
        for (std::size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
    }
}

}  // namespace

std::vector<std::uint8_t> zlib_compress(const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> out = {0x78, 0x9C};
    BitWriter bw(out);
    std::vector<std::int32_t> head(std::size_t{1} << kHashBits, -1);
    std::vector<std::int32_t> prev(kWindow, -1);
    auto hash = [&](std::size_t i) {
        return ((static_cast<std::size_t>(data[i]) << 10) ^
                (static_cast<std::size_t>(data[i + 1]) << 5) ^ data[i + 2]) &
               ((std::size_t{1} << kHashBits) - 1);
    };
    auto insert = [&](std::size_t i) {
        if (i + kMinMatch > size) return;
        const std::size_t h = hash(i);
        prev[i % kWindow] = head[h];
        head[h] = static_cast<std::int32_t>(i);
    };
    std::vector<Symbol> syms;
    syms.reserve(kBlockSymbols);
    std::size_t i = 0;
    while (i < size) {
        std::size_t best_len = 0;
        std::size_t best_dist = 0;
        if (i + kMinMatch <= size) {
            std::int32_t cand = head[hash(i)];
            const std::size_t max_len = std::min(kMaxMatch, size - i);
            for (int chain = 0; cand >= 0 && chain < kMaxChain; ++chain) {
                const auto c = static_cast<std::size_t>(cand);
                if (i - c > kWindow - 1) break;
                if (data[c + best_len] == data[i + best_len]) {
                    std::size_t l = 0;
                    while (l < max_len && data[c + l] == data[i + l]) ++l;
                    if (l > best_len) {
                        best_len = l;
                        best_dist = i - c;
                        if (l == max_len) break;
                    }
                }
                const std::int32_t p = prev[c % kWindow];
                if (p >= cand) break;  // stale entry from an older window position
                cand = p;
            }
        }
        if (best_len >= kMinMatch) {
            syms.push_back(
                {static_cast<std::uint16_t>(best_len), static_cast<std::uint16_t>(best_dist)});
            for (std::size_t k = 0; k < best_len; ++k) insert(i + k);
            i += best_len;
        } else {
            syms.push_back({data[i], 0});
            insert(i);
            ++i;
        }
        if (syms.size() >= kBlockSymbols) {
            write_block(bw, syms, false);
            syms.clear();
        }
    }
    write_block(bw, syms, true);
    bw.flush();
    const std::uint32_t adler = detail::adler32(data, size);
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>((adler >> s) & 0xFFU));
    return out;
}

std::optional<std::vector<std::uint8_t>> zlib_decompress(const std::uint8_t* data, std::size_t size,
                                                         std::size_t max_output) {
    if (size < 6) return std::nullopt;
    const unsigned cmf = data[0];
    const unsigned flg = data[1];
    if ((cmf & 0x0FU) != 8 || (cmf * 256U + flg) % 31U != 0 || (flg & 0x20U) != 0) {
        return std::nullopt;  // not deflate, bad check, or preset dictionary
    }
    BitReader br(data + 2, size - 6);
    std::vector<std::uint8_t> out;
    bool last = false;
    while (!last) {
        last = br.bits(1) == 1;
        const std::uint32_t type = br.bits(2);
        if (br.error) return std::nullopt;
        if (type == 0) {
            br.align();
            const std::size_t p = br.position();
            if (p + 4 > size - 6) return std::nullopt;
            const std::uint8_t* s = data + 2 + p;
            const std::size_t len = s[0] | (static_cast<std::size_t>(s[1]) << 8);
            const std::size_t nlen = s[2] | (static_cast<std::size_t>(s[3]) << 8);
            if ((len ^ 0xFFFFU) != nlen || p + 4 + len > size - 6) return std::nullopt;
            if (out.size() + len > max_output) return std::nullopt;
            out.insert(out.end(), s + 4, s + 4 + len);
            br.skip(4 + len);
        } else if (type == 1 || type == 2) {
            std::array<std::uint8_t, 320> lens{};
            std::size_t nlit = 288;
            std::size_t ndist = 30;
            if (type == 1) {
                for (std::size_t k = 0; k < 144; ++k) lens[k] = 8;
                for (std::size_t k = 144; k < 256; ++k) lens[k] = 9;
                for (std::size_t k = 256; k < 280; ++k) lens[k] = 7;
                for (std::size_t k = 280; k < 288; ++k) lens[k] = 8;
                for (std::size_t k = 0; k < 30; ++k) lens[288 + k] = 5;
            } else {
                nlit = br.bits(5) + 257;
                ndist = br.bits(5) + 1;
                const std::size_t ncl = br.bits(4) + 4;
                if (br.error || nlit > 286 || ndist > 30) return std::nullopt;
                std::array<std::uint8_t, 19> cl{};
                for (std::size_t k = 0; k < ncl; ++k) {
                    cl[kClOrder[k]] = static_cast<std::uint8_t>(br.bits(3));
                }
                Huffman clh;
                if (!build(clh, cl.data(), 19)) return std::nullopt;
                std::size_t k = 0;
                while (k < nlit + ndist) {
                    const int sym = decode(br, clh);
                    if (sym < 0) return std::nullopt;
                    std::size_t rep = 0;
                    std::uint8_t val = 0;
                    if (sym < 16) {
                        lens[k++] = static_cast<std::uint8_t>(sym);
                        continue;
                    }
                    if (sym == 16) {
                        if (k == 0) return std::nullopt;
                        val = lens[k - 1];
                        rep = 3 + br.bits(2);
                    } else if (sym == 17) {
                        rep = 3 + br.bits(3);
                    } else {
                        rep = 11 + br.bits(7);
                    }
                    if (br.error || k + rep > nlit + ndist) return std::nullopt;
                    for (; rep > 0; --rep) lens[k++] = val;
                }
                // Dist lengths follow the literal/length ones; move them to lens[288…]
                // (the ranges may overlap, hence the copy).
                std::array<std::uint8_t, 30> d{};
                std::copy(lens.begin() + static_cast<long>(nlit),
                          lens.begin() + static_cast<long>(nlit + ndist), d.begin());
                std::copy(d.begin(), d.begin() + static_cast<long>(ndist), lens.begin() + 288);
            }
            Huffman lit;
            Huffman dist;
            if (!build(lit, lens.data(), nlit) || !build(dist, lens.data() + 288, ndist)) {
                return std::nullopt;
            }
            if (!inflate_codes(br, lit, dist, out, max_output)) return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    const std::uint8_t* t = data + size - 4;
    const std::uint32_t adler = (static_cast<std::uint32_t>(t[0]) << 24) |
                                (static_cast<std::uint32_t>(t[1]) << 16) |
                                (static_cast<std::uint32_t>(t[2]) << 8) | t[3];
    if (adler != detail::adler32(out.data(), out.size())) return std::nullopt;
    return out;
}

}  // namespace owfax
