// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The vendor name pool's 6-bit coding (tools/gen_vendors.py packs it): symbols run LSB first, 0 ends a name,
// 1-26 a-z, 27-36 0-9, 37-40 space - _ . ; decoding gives capitals. Pure code over the pool, host-tested.
#pragma once
#include <cstdint>

#include "k2/pd/vendor_name.hpp"

namespace k2::pd::vendor_pack {

inline constexpr char kAlpha[] = "\0ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.";   // index = code
inline constexpr unsigned kSymbols = sizeof kAlpha - 1;                            // codes 0..40

// The pool carries one pad byte, so reading two bytes at the last symbol stays inside it.
constexpr unsigned symbol(const std::uint8_t* pool, unsigned bit) {
    const unsigned i = bit >> 3;
    return ((pool[i] | (pool[i + 1] << 8)) >> (bit & 7)) & 63u;
}

// Decode the name at bit offset `bit` into `out` (kVendorMax + 1 bytes; nullptr just skips it); returns the bit offset
// after its terminator. A code outside the alphabet ends the name too.
constexpr unsigned unpack(const std::uint8_t* pool, unsigned bit, char* out) {
    unsigned n = 0;
    for (;;) {
        const unsigned s = symbol(pool, bit);
        bit += 6;
        if (s == 0 || s >= kSymbols) break;
        if (out && n < kVendorMax) out[n++] = kAlpha[s];
    }
    if (out) out[n] = 0;
    return bit;
}

}  // namespace k2::pd::vendor_pack
