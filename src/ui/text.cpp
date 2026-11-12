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
    if (cp - 0x20 < f.dense) return &f.glyphs[cp - 0x20];       // the leading run is indexed directly
    const size_t sparse = f.count - f.dense;
    size_t lo = 0, hi = sparse;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (f.codepoints[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    return (lo < sparse && f.codepoints[lo] == cp) ? &f.glyphs[f.dense + lo] : nullptr;
}

uint32_t g_missing_glyphs = 0;

namespace {

// A reader over the run-length coded pixels of one glyph group (token format: font_aa.hpp). It is
// the whole decoder state: a byte pointer, the nibble phase and the run being handed out, so a
// row is decoded in one pass with no buffer.
struct Pix {
    const uint8_t *p;
    uint8_t zmax;
    bool lo = false;     // the next nibble is the low one of *p
    uint8_t lvl = 0;     // level of the current run
    uint8_t left = 0;    // pixels of the current run not handed out yet

    uint8_t nibble() {
        if (!lo) { lo = true; return *p >> 4; }
        lo = false;
        return *p++ & 0x0F;
    }
    void token() {
        const uint8_t t = nibble();
        if (t) { lvl = t; left = 1; return; }
        const uint8_t n = nibble();
        if (n < zmax) { lvl = 0; left = n + 1; }
        else { lvl = 15; left = n - zmax + 3; }
    }
    uint8_t next() {
        if (!left) token();
        left--;
        return lvl;
    }
    void skip(int n) {
        while (n > 0) {
            if (!left) token();
            const int t = left < n ? left : n;
            left = static_cast<uint8_t>(left - t);
            n -= t;
        }
    }
};

// A reader placed on pixel (gx, gy) of a glyph: add up the lengths of the 8-row groups above gy, then
// scan the tokens of the rows of its group that precede it (at most 7 rows plus gx pixels).
Pix open_at(const Font &f, const Glyph &g, int gx, int gy) {
    const uint8_t *len = f.bitmap + g.offset;
    const uint8_t *group = len + ((g.h + 7) >> 3) - 1;   // the length bytes come first
    for (int k = gy >> 3; k > 0; k--) group += *len++;
    Pix px{group, f.zmax};
    px.skip((gy & 7) * g.w + gx);
    return px;
}

}  // namespace

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

int render_row_span(const Font &f, std::string_view s, int row, const uint16_t (&lut)[16],
                    uint16_t *out, int x0, int count) {
    const int width = text_width(f, s);
    const int x1 = x0 + count < width ? x0 + count : width;   // one past the last column served
    const int n = x1 > x0 ? x1 - x0 : 0;
    for (int x = 0; x < n; x++) out[x] = lut[0];

    int pen = 0;
    for (size_t i = 0; i < s.size() && pen < x1;) {
        const Glyph *g = find_glyph(f, next_codepoint(s, i));
        if (g) {
            // The ink box starts (ascent - top) rows below the top of the line box.
            int gy = row - (f.ascent - g->top);
            if (gy >= 0 && gy < g->h) {
                // The columns of the ink box inside the window [x0, x1); the decoder starts on the first.
                const int gx0 = x0 - pen - g->left > 0 ? x0 - pen - g->left : 0;
                const int gx1 = x1 - pen - g->left < g->w ? x1 - pen - g->left : g->w;
                if (gx0 < gx1) {
                    Pix px = open_at(f, *g, gx0, gy);
                    uint16_t *dst = out + (pen + g->left - x0);
                    for (int gx = gx0; gx < gx1; gx++) {
                        const uint8_t l = px.next();
                        if (l) dst[gx] = lut[l];   // glyph ink lies inside its cell: no overlap to merge
                    }
                }
            }
        }
        pen += f.adv;
    }
    return n;
}

int render_row(const Font &f, std::string_view s, int row, const uint16_t (&lut)[16],
               uint16_t *out, int max_px) {
    return render_row_span(f, s, row, lut, out, 0, max_px);
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
        // One column of the box, top to bottom: each 8-row group is its own stream, entered at row start.
        const uint8_t *len = f.bitmap + g->offset;
        const uint8_t *group = len + ((g->h + 7) >> 3) - 1;
        Pix px{group, f.zmax};
        for (int gy = 0; gy < g->h; gy++) {
            if (gy && !(gy & 7)) { group += *len++; px = Pix{group, f.zmax}; }
            px.skip(gx);
            const uint8_t l = px.next();
            px.skip(g->w - gx - 1);
            const int y = f.ascent - g->top + gy;
            if (l && y >= 0 && y < f.height) out[y] = lut[l];
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
