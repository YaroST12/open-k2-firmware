// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A minimal QR reader for the tests: format bits -> mask -> zig-zag codeword read -> byte-mode payload.
// It does not run the Reed-Solomon correction (a clean matrix needs none), so a test that decodes through it
// proves the symbol layout (finder-relative placement, mask, interleave) independently of the encoder under test.
// Supports versions 1..12, error-correction level L or M, one byte-mode or alphanumeric segment.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace qrdec {

// dark(x, y) is true for a dark module; `n` is the symbol width.
using Dark = std::function<bool(int, int)>;

struct Result {
    bool ok = false;
    int version = 0;
    int ecl = -1;       // 0 L, 1 M, 2 Q, 3 H
    int mask = -1;
    std::string text;
};

inline bool is_function(int v, int n, int x, int y) {
    if (x < 9 && y < 9) return true;                 // top-left finder + format
    if (x >= n - 8 && y < 9) return true;            // top-right
    if (x < 9 && y >= n - 8) return true;            // bottom-left
    if (x == 6 || y == 6) return true;               // timing
    if (v >= 7) {                                     // version info blocks
        if (x >= n - 11 && y < 6) return true;
        if (y >= n - 11 && x < 6) return true;
    }
    if (v >= 2) {                                     // alignment patterns
        static const int pos[13][8] = {
            {0}, {0}, {6, 18, 0}, {6, 22, 0}, {6, 26, 0}, {6, 30, 0}, {6, 34, 0},
            {6, 22, 38, 0}, {6, 24, 42, 0}, {6, 26, 46, 0}, {6, 28, 50, 0}, {6, 30, 54, 0}, {6, 32, 58, 0}};
        const int* p = pos[v];
        int cnt = 0;
        while (p[cnt]) cnt++;
        for (int i = 0; i < cnt; i++)
            for (int j = 0; j < cnt; j++) {
                if ((i == 0 && j == 0) || (i == 0 && j == cnt - 1) || (i == cnt - 1 && j == 0)) continue;
                if (x >= p[i] - 2 && x <= p[i] + 2 && y >= p[j] - 2 && y <= p[j] + 2) return true;
            }
    }
    return false;
}

inline bool mask_bit(int m, int x, int y) {
    switch (m) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

inline Result decode(int n, const Dark& dark) {
    Result r;
    if (n < 21 || (n - 17) % 4 != 0) return r;
    const int v = (n - 17) / 4;
    if (v > 12) return r;
    // Format word (15 bits, XOR 0x5412): first copy around the top-left finder.
    unsigned fmt = 0;
    const int fx[15] = {8, 8, 8, 8, 8, 8, 8, 8, 7, 5, 4, 3, 2, 1, 0};
    const int fy[15] = {0, 1, 2, 3, 4, 5, 7, 8, 8, 8, 8, 8, 8, 8, 8};
    for (int i = 0; i < 15; i++) fmt |= (dark(fx[i], fy[i]) ? 1u : 0u) << i;
    fmt ^= 0x5412;
    const int ecbits = (fmt >> 13) & 3;       // 1 L, 0 M, 3 Q, 2 H
    r.mask = (fmt >> 10) & 7;
    r.ecl = ecbits == 1 ? 0 : ecbits == 0 ? 1 : ecbits == 3 ? 2 : 3;
    r.version = v;
    // Raw bits in zig-zag order from the bottom-right, skipping function modules.
    std::vector<int> bits;
    bool up = true;
    for (int x = n - 1; x > 0; x -= 2) {
        if (x == 6) x = 5;
        for (int k = 0; k < n; k++) {
            const int y = up ? n - 1 - k : k;
            for (int dx = 0; dx < 2; dx++) {
                const int xx = x - dx;
                if (is_function(v, n, xx, y)) continue;
                bits.push_back((dark(xx, y) ? 1 : 0) ^ (mask_bit(r.mask, xx, y) ? 1 : 0));
            }
        }
        up = !up;
    }
    // Data codewords are interleaved block by block; deinterleave for the version / level in use.
    // Total codewords and EC codewords per block, blocks (g1 blocks of len1, g2 blocks of len1+1), levels L and M.
    struct Blk { int total, ec, b1, b2; };
    static const Blk L[13] = {{0, 0, 0, 0}, {26, 7, 1, 0}, {44, 10, 1, 0}, {70, 15, 1, 0}, {100, 20, 1, 0}, {134, 26, 1, 0},
                              {172, 18, 2, 0}, {196, 20, 2, 0}, {242, 24, 2, 0}, {292, 30, 2, 0}, {346, 18, 2, 2},
                              {404, 20, 4, 0}, {466, 24, 2, 2}};
    static const Blk M[13] = {{0, 0, 0, 0}, {26, 10, 1, 0}, {44, 16, 1, 0}, {70, 26, 1, 0}, {100, 18, 2, 0}, {134, 24, 2, 0},
                              {172, 16, 4, 0}, {196, 18, 4, 0}, {242, 22, 2, 2}, {292, 22, 3, 2}, {346, 26, 4, 1},
                              {404, 30, 1, 4}, {466, 22, 6, 2}};
    if (r.ecl > 1) return r;
    const Blk b = (r.ecl == 0 ? L : M)[v];
    const int blocks = b.b1 + b.b2;
    const int data_total = b.total - b.ec * blocks;
    const int len1 = data_total / blocks;     // bytes in a short block
    std::vector<int> cw(static_cast<size_t>(b.total));
    for (int i = 0; i < b.total && static_cast<size_t>(i * 8 + 7) < bits.size(); i++) {
        int c = 0;
        for (int k = 0; k < 8; k++) c = c << 1 | bits[static_cast<size_t>(i * 8 + k)];
        cw[static_cast<size_t>(i)] = c;
    }
    std::vector<int> data;
    {
        std::vector<std::vector<int>> blk(static_cast<size_t>(blocks));
        int idx = 0;
        for (int i = 0; i <= len1; i++)
            for (int j = 0; j < blocks; j++) {
                const int len = len1 + (j >= b.b1 ? 1 : 0);
                if (i < len) blk[static_cast<size_t>(j)].push_back(cw[static_cast<size_t>(idx++)]);
            }
        for (auto& bl : blk) data.insert(data.end(), bl.begin(), bl.end());
    }
    // One byte-mode segment: 0100, count (8 bits up to v9, 16 from v10), bytes.
    size_t pos = 0;
    auto take = [&](int n_bits) {
        int val = 0;
        for (int i = 0; i < n_bits; i++, pos++) {
            const size_t byte = pos / 8;
            const int bit = byte < data.size() ? (data[byte] >> (7 - pos % 8)) & 1 : 0;
            val = val << 1 | bit;
        }
        return val;
    };
    const int mode = take(4);
    if (mode == 4) {
        const int count = take(v >= 10 ? 16 : 8);
        for (int i = 0; i < count; i++) r.text.push_back(static_cast<char>(take(8)));
    } else if (mode == 2) {                           // alphanumeric: pairs in 11 bits, a last single in 6
        static const char kAlnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
        const int count = take(v >= 10 ? 11 : 9);
        for (int i = 0; i + 1 < count; i += 2) {
            const int pair = take(11);
            r.text.push_back(kAlnum[pair / 45 % 45]);
            r.text.push_back(kAlnum[pair % 45]);
        }
        if (count % 2) r.text.push_back(kAlnum[take(6) % 45]);
    } else {
        return r;
    }
    r.ok = true;
    return r;
}

}  // namespace qrdec
