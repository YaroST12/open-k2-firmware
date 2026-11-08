// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/rle565.hpp"

namespace k2 {

size_t Rle565Encoder::next(uint8_t *out, size_t cap) {
    size_t w = 0;
    while (pos_ < n_) {
        // Run length at pos_ (equal pixels), capped.
        size_t run = 1;
        while (pos_ + run < n_ && run < kRleMaxRun && px_[pos_ + run] == px_[pos_]) run++;
        size_t need, take;
        if (run >= 2) {
            take = run;
            need = 3;
        } else {
            // Literal: extend until a run of three or more starts, or the cap.
            take = 1;
            while (take < kRleMaxLiteral && pos_ + take < n_) {
                const size_t i = pos_ + take;
                if (i + 2 < n_ && px_[i] == px_[i + 1] && px_[i] == px_[i + 2]) break;
                take++;
            }
            need = 1 + 2 * take;
        }
        if (w + need > cap) break;
        if (run >= 2) {
            out[w++] = static_cast<uint8_t>(take - 1);
            out[w++] = static_cast<uint8_t>(px_[pos_] & 0xFF);
            out[w++] = static_cast<uint8_t>(px_[pos_] >> 8);
        } else {
            out[w++] = static_cast<uint8_t>(0x80 + take - 1);
            for (size_t k = 0; k < take; k++) {
                out[w++] = static_cast<uint8_t>(px_[pos_ + k] & 0xFF);
                out[w++] = static_cast<uint8_t>(px_[pos_ + k] >> 8);
            }
        }
        pos_ += take;
    }
    return w;
}

bool rle565_decode(const uint8_t *in, size_t len, uint16_t *out, size_t cap, size_t &n_out) {
    size_t i = 0, n = 0;
    n_out = 0;
    while (i < len) {
        const uint8_t t = in[i++];
        const size_t count = (t & 0x7F) + 1;
        if (n + count > cap) return false;
        if (t < 0x80) {
            if (i + 2 > len) return false;
            const uint16_t v = static_cast<uint16_t>(in[i] | (in[i + 1] << 8));
            i += 2;
            for (size_t k = 0; k < count; k++) out[n++] = v;
        } else {
            if (i + 2 * count > len) return false;
            for (size_t k = 0; k < count; k++, i += 2) out[n++] = static_cast<uint16_t>(in[i] | (in[i + 1] << 8));
        }
    }
    n_out = n;
    return true;
}

}  // namespace k2
