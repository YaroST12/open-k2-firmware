// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Run-length coding of RGB565 pixel streams for the screenshot transfer. Pure.
//
// Token byte t, then pixels as little-endian u16:
//   0x00..0x7F  run:     (t + 1) copies of the one pixel that follows       (1..128, 3 bytes)
//   0x80..0xFF  literal: (t - 0x7F) distinct pixels follow                  (1..128, 1 + 2n bytes)
// The encoder emits literals of at most kMaxLiteral pixels so that a token (at most 55 bytes)
// always fits one stream-frame chunk (56 bytes) and tokens are never split across chunks.
#pragma once
#include <cstddef>
#include <cstdint>

namespace k2 {

constexpr size_t kRleMaxRun = 128;
constexpr size_t kRleMaxLiteral = 27;

// Upper bound of the encoded size of n pixels (all pixels different).
constexpr size_t rle565_worst_case(size_t n) { return 2 * n + (n + kRleMaxLiteral - 1) / kRleMaxLiteral; }

class Rle565Encoder {
public:
    void reset(const uint16_t *px, size_t count) { px_ = px; n_ = count; pos_ = 0; }
    bool done() const { return pos_ >= n_; }
    // Writes whole tokens into out (at most cap bytes, cap >= 55). Returns the byte count.
    size_t next(uint8_t *out, size_t cap);

private:
    const uint16_t *px_ = nullptr;
    size_t n_ = 0, pos_ = 0;
};

// Decodes a complete RLE565 stream. Returns false on a malformed or oversize stream; n_out is
// the number of pixels written (exactly the expected count for a valid strip).
bool rle565_decode(const uint8_t *in, size_t len, uint16_t *out, size_t cap, size_t &n_out);

}  // namespace k2
