// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/text.hpp"

namespace k2 {

uint32_t next_codepoint(std::string_view s, size_t &i) {
    const auto b0 = static_cast<uint8_t>(s[i++]);
    if (b0 < 0x80) return b0;
    int extra;
    uint32_t cp;
    if ((b0 & 0xE0) == 0xC0)      { extra = 1; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07; }
    else return '?';
    for (int k = 0; k < extra; k++) {
        if (i >= s.size() || (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (static_cast<uint8_t>(s[i++]) & 0x3F);
    }
    return cp;
}

static const Glyph *lookup(const Font &f, uint32_t cp) {
    if (cp > 0xFFFF) return nullptr;
    size_t lo = 0, hi = f.count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (f.codepoints[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    return (lo < f.count && f.codepoints[lo] == cp) ? &f.glyphs[lo] : nullptr;
}

uint32_t g_missing_glyphs = 0;

// Coverage level 0..15 (index into the blend LUT) of pixel (gx, gy) of a glyph. 4 bpp rows hold two
// pixels per byte, left pixel in the high nibble; 1 bpp rows hold eight, leftmost in bit 7, and ink
// is full coverage (font_aa.hpp).
inline uint8_t level(const Font &f, const Glyph &g, int gx, int gy) {
    if (f.bpp == 4) {
        const uint8_t b = f.bitmap[g.offset + gy * ((g.w + 1) / 2) + (gx >> 1)];
        return (gx & 1) ? (b & 0x0F) : (b >> 4);
    }
    const uint8_t b = f.bitmap[g.offset + gy * ((g.w + 7) / 8) + (gx >> 3)];
    return ((b << (gx & 7)) & 0x80) ? 15 : 0;
}

const Glyph *find_glyph(const Font &f, uint32_t cp) {
    const Glyph *g = lookup(f, cp);
    if (g) return g;
    g_missing_glyphs++;
    return lookup(f, '?');
}

int text_width(const Font &f, std::string_view s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) { next_codepoint(s, i); n++; }
    return n * f.adv;
}

int render_row(const Font &f, std::string_view s, int row, const uint16_t (&lut)[16],
               uint16_t *out, int max_px) {
    int width = text_width(f, s);
    int n = width < max_px ? width : max_px;
    for (int x = 0; x < n; x++) out[x] = lut[0];

    int pen = 0;
    for (size_t i = 0; i < s.size() && pen < n;) {
        const Glyph *g = find_glyph(f, next_codepoint(s, i));
        if (g) {
            // The ink box starts (ascent - top) rows below the top of the line box.
            int gy = row - (f.ascent - g->top);
            if (gy >= 0 && gy < g->h) {
                for (int gx = 0; gx < g->w; gx++) {
                    int x = pen + g->left + gx;
                    if (x < 0 || x >= n) continue;
                    const uint8_t l = level(f, *g, gx, gy);
                    if (l) out[x] = lut[l];   // glyph ink lies inside its cell: no overlap to merge
                }
            }
        }
        pen += f.adv;
    }
    return n;
}

int render_column(const Font &f, std::string_view s, int col, const uint16_t (&lut)[16], uint16_t *out) {
    for (int y = 0; y < f.height; y++) out[y] = lut[0];
    if (col < 0) return f.height;
    size_t i = 0;
    for (int idx = col / f.adv; ; idx--) {
        if (i >= s.size()) return f.height;
        uint32_t cp = next_codepoint(s, i);
        if (idx > 0) continue;
        const Glyph *g = find_glyph(f, cp);
        if (!g) return f.height;
        int gx = col - (col / f.adv) * f.adv - g->left;
        if (gx < 0 || gx >= g->w) return f.height;
        for (int gy = 0; gy < g->h; gy++) {
            int y = f.ascent - g->top + gy;
            if (y < 0 || y >= f.height) continue;
            const uint8_t l = level(f, *g, gx, gy);
            if (l) out[y] = lut[l];
        }
        return f.height;
    }
}

void render_rotated_row(const Font &f, std::string_view s, TextDir d, int sy, const uint16_t (&lut)[16],
                        uint16_t *out) {
    const int text_w = text_width(f, s);
    // Window row sy shows source column sx-independent: Ccw -> column text_w-1-sy read top-down,
    // Cw -> column sy read bottom-up (rot_source gives the per-pixel form of the same mapping).
    const SrcPos p0 = rot_source(d, text_w, f.height, 0, sy);
    render_column(f, s, p0.x, lut, out);
    if (d == TextDir::Cw) {
        for (int a = 0, b = f.height - 1; a < b; a++, b--) { uint16_t t = out[a]; out[a] = out[b]; out[b] = t; }
    }
}

}  // namespace k2
