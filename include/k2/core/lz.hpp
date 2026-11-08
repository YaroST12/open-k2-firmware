// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// LZ4 block codec for small buffers (black-box captures, <= 64 KB). Static memory only: the
// compressor keeps a 512-byte hash table on the stack, the decompressor needs nothing. The
// stream is the standard LZ4 block format (token, literals, 16-bit offset, match length), so any
// LZ4 block decoder reads it. Both directions are bounds-checked against the given spans.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::lz {

inline constexpr std::size_t kError = ~std::size_t{0};   // decompress: corrupt stream or output too small

// Worst-case compressed size of `n` bytes.
constexpr std::size_t bound(std::size_t n) { return n + n / 255 + 16; }

namespace detail {
inline constexpr std::size_t kMinMatch = 4, kLastLiterals = 5, kMfLimit = 12;
inline constexpr unsigned kHashBits = 8;

constexpr std::uint32_t load32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
constexpr unsigned hash(std::uint32_t v) { return (v * 2654435761u) >> (32 - kHashBits); }

// Writes a length extension (the part past the 15 held in the token nibble).
inline bool put_ext(std::span<std::uint8_t> out, std::size_t& o, std::size_t v) {
    for (; v >= 255; v -= 255) { if (o >= out.size()) return false; out[o++] = 255; }
    if (o >= out.size()) return false;
    out[o++] = std::uint8_t(v);
    return true;
}
}  // namespace detail

// Compresses `in` into `out`. Returns the compressed size, or 0 when `out` is too small (also
// returned for an empty input). `out` of bound(in.size()) bytes never fails.
inline std::size_t compress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
    using namespace detail;
    const std::size_t n = in.size();
    if (n == 0 || n > 0xFFFF) return 0;
    std::array<std::uint16_t, 1u << kHashBits> table{};   // position + 1; 0 = empty
    std::size_t ip = 0, anchor = 0, o = 0;

    auto emit = [&](std::size_t lit_end, std::size_t match_len, std::size_t offset) -> bool {
        const std::size_t lit = lit_end - anchor;
        if (o >= out.size()) return false;
        const std::size_t tok = o++;
        out[tok] = std::uint8_t((lit >= 15 ? 15 : lit) << 4);
        if (lit >= 15 && !put_ext(out, o, lit - 15)) return false;
        if (o + lit > out.size()) return false;
        for (std::size_t i = 0; i < lit; i++) out[o++] = in[anchor + i];
        if (match_len == 0) return true;
        if (o + 2 > out.size()) return false;
        out[o++] = std::uint8_t(offset);
        out[o++] = std::uint8_t(offset >> 8);
        const std::size_t m = match_len - kMinMatch;
        out[tok] = std::uint8_t(out[tok] | (m >= 15 ? 15 : m));
        return m < 15 || put_ext(out, o, m - 15);
    };

    if (n >= kMfLimit + 1) {
        const std::size_t limit = n - kMfLimit;        // last position a match may start at
        while (ip < limit) {
            const std::uint32_t seq = load32(&in[ip]);
            std::uint16_t& slot = table[hash(seq)];
            const std::size_t cand = slot ? std::size_t(slot) - 1 : n;
            slot = std::uint16_t(ip + 1);
            if (cand < ip && ip - cand <= 0xFFFF && load32(&in[cand]) == seq) {
                std::size_t len = kMinMatch;
                const std::size_t max_len = n - kLastLiterals - ip;
                while (len < max_len && in[cand + len] == in[ip + len]) len++;
                if (!emit(ip, len, ip - cand)) return 0;
                ip += len;
                anchor = ip;
                continue;
            }
            ip++;
        }
    }
    return emit(n, 0, 0) ? o : 0;   // the last sequence holds literals only
}

// Decompresses `in` into `out`. Returns the decoded size, or kError for a corrupt stream or an
// output that is too small. Never reads or writes outside the spans.
inline std::size_t decompress(std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
    std::size_t ip = 0, op = 0;
    auto ext = [&](std::size_t& v) -> bool {
        for (;;) {
            if (ip >= in.size()) return false;
            const std::uint8_t b = in[ip++];
            v += b;
            if (b != 255) return true;
            if (v > 0x1000000) return false;
        }
    };
    while (ip < in.size()) {
        const std::uint8_t tok = in[ip++];
        std::size_t lit = tok >> 4;
        if (lit == 15 && !ext(lit)) return kError;
        if (lit > in.size() - ip || lit > out.size() - op) return kError;
        for (std::size_t i = 0; i < lit; i++) out[op++] = in[ip++];
        if (ip == in.size()) break;                      // the final sequence has no match
        if (in.size() - ip < 2) return kError;
        const std::size_t offset = std::size_t(in[ip]) | std::size_t(in[ip + 1]) << 8;
        ip += 2;
        std::size_t len = tok & 15;
        if (len == 15 && !ext(len)) return kError;
        len += detail::kMinMatch;
        if (offset == 0 || offset > op || len > out.size() - op) return kError;
        for (std::size_t i = 0; i < len; i++, op++) out[op] = out[op - offset];   // may overlap
    }
    return op;
}

}  // namespace k2::lz
