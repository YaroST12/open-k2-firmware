// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for UTF-8 decoding, glyph lookup and the per-row text renderer, using the real fonts.
#include "test_util.h"
#include "k2/ui/text.hpp"
#include "k2/ui/font_aa.hpp"
#include "k2/ui/blend.hpp"

using namespace k2;

static void test_utf8() {
    std::string_view s = "A\xC2\xB0\xCE\xA9\xE2\x86\x92z";   // A, degree, Omega, right arrow, z
    size_t i = 0;
    CHECK_EQ(next_codepoint(s, i), 'A');    CHECK_EQ(i, 1);
    CHECK_EQ(next_codepoint(s, i), 0xB0);   CHECK_EQ(i, 3);
    CHECK_EQ(next_codepoint(s, i), 0x3A9);  CHECK_EQ(i, 5);
    CHECK_EQ(next_codepoint(s, i), 0x2192); CHECK_EQ(i, 8);
    CHECK_EQ(next_codepoint(s, i), 'z');    CHECK_EQ(i, 9);
    // Invalid or truncated sequences decode to '?' and always advance.
    std::string_view bad = "\xFF\xC2";
    i = 0;
    CHECK_EQ(next_codepoint(bad, i), '?');  CHECK_EQ(i, 1);
    CHECK_EQ(next_codepoint(bad, i), '?');  CHECK_EQ(i, 2);
}

static void test_lookup() {
    CHECK_EQ(find_glyph(font_16, 'A') != nullptr, 1);
    CHECK_EQ(find_glyph(font_16, 0xB0) != nullptr, 1);     // degree
    CHECK_EQ(find_glyph(font_16, 0x3A9) != nullptr, 1);    // Omega
    CHECK_EQ(find_glyph(font_16, 0x2192) != nullptr, 1);   // arrow
    // The 12 px set (footer, pills, panel) carries degree, Omega and the vertical flow arrows.
    CHECK_EQ(find_glyph(font_12, 0xB0) != nullptr, 1);
    CHECK_EQ(find_glyph(font_12, 0x3A9) != nullptr, 1);
    CHECK_EQ(find_glyph(font_12, 0x2191) != nullptr, 1);
    CHECK_EQ(find_glyph(font_12, 0x2193) != nullptr, 1);
    CHECK_EQ(find_glyph(font_12, 0x2192) != nullptr, 1);   // the ramp labels ("PPS 11→8V")
    CHECK_EQ(find_glyph(font_12, 0x2192) != find_glyph(font_12, '?'), 1);
    // Missing code point falls back to '?'.
    CHECK_EQ(find_glyph(font_16, 0x4E2D) == find_glyph(font_16, '?'), 1);
    // The 48 px set is digits only: a letter falls back to nothing.
    CHECK_EQ(find_glyph(font_48, '8') != nullptr, 1);
    CHECK_EQ(find_glyph(font_48, 'V') == nullptr, 1);
    CHECK_EQ(text_width(font_16, "ab\xC2\xB0"), 3 * font_16.adv);
    CHECK_EQ(text_width(font_48, "20.03"), 5 * font_48.adv);
}

static int count_ink_rows(const Font &f, std::string_view s, int *first, int *last) {
    uint16_t lut[16];
    make_blend_lut(0xFFFF, 0x0000, lut);
    uint16_t row[240];
    int n = 0;
    *first = -1; *last = -1;
    for (int r = 0; r < f.height; r++) {
        int w = render_row(f, s, r, lut, row, 240);
        CHECK_EQ(w, text_width(f, s));
        bool ink = false;
        for (int x = 0; x < w; x++) if (row[x] != 0) ink = true;
        if (ink) { n++; if (*first < 0) *first = r; *last = r; }
    }
    return n;
}

static void test_render() {
    uint16_t lut[16];
    make_blend_lut(0xFFFF, 0x0000, lut);
    uint16_t row[240];

    // A space is pure background on every row.
    for (int r = 0; r < font_16.height; r++) {
        int w = render_row(font_16, " ", r, lut, row, 240);
        CHECK_EQ(w, font_16.adv);
        for (int x = 0; x < w; x++) CHECK_EQ(row[x], 0);
    }

    // Ink stays inside the line box and 'H' touches both cap height and baseline.
    int first, last;
    int rows = count_ink_rows(font_16, "H", &first, &last);
    CHECK_EQ(rows > 8, 1);
    CHECK_EQ(first >= 0, 1);
    CHECK_EQ(last, font_16.ascent - 1);                    // last ink row sits on the baseline

    // The 48 px '8' has solid ink and, for an anti-aliased (4 bpp) face, blended edge pixels.
    bool saw_fg = false, saw_edge = false;
    for (int r = 0; r < font_48.height; r++) {
        int w = render_row(font_48, "8", r, lut, row, 240);
        for (int x = 0; x < w; x++) {
            if (row[x] == 0xFFFF) saw_fg = true;
            if (row[x] != 0 && row[x] != 0xFFFF) saw_edge = true;   // a blended pixel
        }
    }
    CHECK_EQ(saw_fg, 1);
    CHECK_EQ(saw_edge, font_48.bpp == 4);

    // Clipping: the row never writes more than max_px pixels.
    uint16_t guard[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    int w = render_row(font_16, "WWWW", 4, lut, guard, 5);
    CHECK_EQ(w, 5);
    CHECK_EQ(guard[5], 1);
    CHECK_EQ(guard[7], 1);

    // Glyphs sit at fixed pen positions: the second 'I' starts exactly one advance later.
    uint16_t a[240], b[240];
    render_row(font_16, "II", 6, lut, a, 240);
    render_row(font_16, "I", 6, lut, b, 240);
    for (int x = 0; x < font_16.adv; x++) CHECK_EQ(a[x], b[x]);
    for (int x = 0; x < font_16.adv; x++) CHECK_EQ(a[font_16.adv + x], b[x]);
}

// ---- rotated text: transposed streaming -------------------------------------------------------

static void test_rot_source_corners() {
    const int w = 60, h = 17;   // "BK-RBT" at 10 px advance, 16 px font line
    // Ccw: text runs bottom-to-top, letter tops on the left. First window row is the text end.
    CHECK_EQ(rot_source(TextDir::Ccw, w, h, 0, 0).x, 59);   CHECK_EQ(rot_source(TextDir::Ccw, w, h, 0, 0).y, 0);
    CHECK_EQ(rot_source(TextDir::Ccw, w, h, 16, 0).x, 59);  CHECK_EQ(rot_source(TextDir::Ccw, w, h, 16, 0).y, 16);
    CHECK_EQ(rot_source(TextDir::Ccw, w, h, 0, 59).x, 0);   CHECK_EQ(rot_source(TextDir::Ccw, w, h, 0, 59).y, 0);
    CHECK_EQ(rot_source(TextDir::Ccw, w, h, 16, 59).x, 0);  CHECK_EQ(rot_source(TextDir::Ccw, w, h, 16, 59).y, 16);
    // Cw: text runs top-to-bottom, letter tops on the right.
    CHECK_EQ(rot_source(TextDir::Cw, w, h, 0, 0).x, 0);     CHECK_EQ(rot_source(TextDir::Cw, w, h, 0, 0).y, 16);
    CHECK_EQ(rot_source(TextDir::Cw, w, h, 16, 0).x, 0);    CHECK_EQ(rot_source(TextDir::Cw, w, h, 16, 0).y, 0);
    CHECK_EQ(rot_source(TextDir::Cw, w, h, 16, 59).x, 59);  CHECK_EQ(rot_source(TextDir::Cw, w, h, 16, 59).y, 0);
    // Both are bijections of the box onto the window.
    for (TextDir d : {TextDir::Ccw, TextDir::Cw}) {
        static int seen[60][17];
        for (auto &r : seen) for (int &v : r) v = 0;
        for (int sy = 0; sy < w; sy++)
            for (int sx = 0; sx < h; sx++) { SrcPos p = rot_source(d, w, h, sx, sy); seen[p.x][p.y]++; }
        int bad = 0;
        for (auto &r : seen) for (int v : r) bad += v != 1;
        CHECK_EQ(bad, 0);
    }
}

// The rotated window must equal the unrotated render (render_row) read through rot_source.
static void check_rotated_equals_rotated_bitmap(const char *text, TextDir d) {
    const Font &f = font_16;
    uint16_t lut[16];
    make_blend_lut(0xFFFF, 0x0000, lut);
    const int w = text_width(f, text), h = f.height;
    static uint16_t bmp[17][240];
    for (int y = 0; y < h; y++) render_row(f, text, y, lut, bmp[y], w);

    int mismatches = 0, ink = 0;
    uint16_t row[32];
    for (int sy = 0; sy < w; sy++) {
        render_rotated_row(f, text, d, sy, lut, row);
        for (int sx = 0; sx < h; sx++) {
            SrcPos p = rot_source(d, w, h, sx, sy);
            if (row[sx] != bmp[p.y][p.x]) mismatches++;
            if (row[sx] != lut[0]) ink++;
        }
    }
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(ink > 20, 1);
}

static void test_rotated_matches_bitmap() {
    for (TextDir d : {TextDir::Ccw, TextDir::Cw}) {
        check_rotated_equals_rotated_bitmap("BK\xC2\xB7RBT", d);
        check_rotated_equals_rotated_bitmap("PG>", d);
        check_rotated_equals_rotated_bitmap("RSC", d);
    }
}

static void test_rotated_orientation_of_a_glyph() {
    // 'I' is a vertical bar with serifs: rotated CCW its tall axis lies along the window's
    // width. Its top (small y in the line box) must be on the left (sx = 0 side).
    const Font &f = font_16;
    uint16_t lut[16];
    make_blend_lut(0xFFFF, 0x0000, lut);
    int ink_left = 0, ink_right = 0;
    uint16_t row[32];
    for (int sy = 0; sy < f.adv; sy++) {
        render_rotated_row(f, "I", TextDir::Ccw, sy, lut, row);
        for (int sx = 0; sx < f.height; sx++) if (row[sx] != lut[0]) (sx < f.height / 2 ? ink_left : ink_right)++;
    }
    CHECK_EQ(ink_left > 0 && ink_right > 0, 1);
    // 'A' has ink in its cap-height rows only: in the Ccw window the first column(s) (the line
    // top, above the cap height) are empty, the ascent line sits at sx = ascent - top.
    const Glyph *g = find_glyph(f, 'A');
    int first_ink = 99;
    for (int sy = 0; sy < f.adv; sy++) {
        render_rotated_row(f, "A", TextDir::Ccw, sy, lut, row);
        for (int sx = 0; sx < f.height; sx++) if (row[sx] != lut[0] && sx < first_ink) first_ink = sx;
    }
    CHECK_EQ(first_ink, f.ascent - g->top);
}

static void test_missing_glyph_count() {
    // The 48 px font only has digits: letters are counted as missing glyphs, supported text is not.
    uint16_t lut[16] = {}; uint16_t row[256];
    g_missing_glyphs = 0;
    render_row(font_48, "12.5", 10, lut, row, 256);
    CHECK_EQ(g_missing_glyphs, 0u);
    render_row(font_48, "QC2", 10, lut, row, 256);
    CHECK_EQ(g_missing_glyphs > 0, 1);
    g_missing_glyphs = 0;
    render_row(font_24, "QC2 SVOOC65 AUTO-DETECT", 10, lut, row, 256);
    CHECK_EQ(g_missing_glyphs, 0u);
}


// ---- glyph table sanity (the shipped fonts, whatever source they were generated from) -----------

static bool has_all(const Font &f, std::string_view chars) {
    for (size_t i = 0; i < chars.size();) {
        const uint32_t cp = next_codepoint(chars, i);
        bool found = false;
        for (uint16_t k = 0; k < f.count; k++) found |= f.codepoints[k] == cp;
        if (!found) return false;
    }
    return true;
}

static void test_font_tables() {
    // Every code point the UI draws exists in the face that draws it, with the layout's advance.
    const char *ascii = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
    const std::string_view extra12 = "\xC2\xB7\xC2\xB0\xC3\x97\xCE\xA9\xE2\x86\x91\xE2\x86\x93";            // . deg x Omega up down
    const std::string_view extra16 = "\xC2\xB7\xC2\xB0\xC3\x97\xCE\xA9\xE2\x86\x90\xE2\x86\x91\xE2\x86\x92\xE2\x86\x93";
    CHECK_EQ(has_all(font_12, ascii), 1);   CHECK_EQ(has_all(font_12, extra12), 1);
    CHECK_EQ(has_all(font_16, ascii), 1);   CHECK_EQ(has_all(font_16, extra16), 1);
    CHECK_EQ(has_all(font_24, ascii), 1);   CHECK_EQ(has_all(font_24, extra16), 1);
    CHECK_EQ(has_all(font_48, "0123456789.- "), 1);
    CHECK_EQ(font_12.adv, 6);  CHECK_EQ(font_16.adv, 8);  CHECK_EQ(font_24.adv, 12);  CHECK_EQ(font_48.adv, 24);
    // Line boxes stay within the nominal size (the 48 px digits sit on the baseline).
    CHECK_EQ(font_12.height <= 12, 1);  CHECK_EQ(font_16.height <= 16, 1);
    CHECK_EQ(font_24.height <= 24, 1);  CHECK_EQ(font_48.height <= 32, 1);
    for (const Font *f : {&font_12, &font_16, &font_24, &font_48}) {
        CHECK_EQ(f->bpp == 1 || f->bpp == 4, 1);
        for (uint16_t k = 0; k < f->count; k++) {
            const Glyph &g = f->glyphs[k];
            if (k) CHECK_EQ(f->codepoints[k] > f->codepoints[k - 1], 1);            // sorted: binary search
            CHECK_EQ(g.adv, f->adv);                                                // monospace
            CHECK_EQ(g.left >= 0 && g.left + g.w <= f->adv, 1);                     // ink inside its cell
            CHECK_EQ(g.top <= f->ascent && g.h - g.top <= f->descent, 1);          // ink inside the line box
        }
    }
}

// ---- 4 bpp and 1 bpp glyph decoding on hand-built fonts ----------------------------------------

namespace {
// A 3x2 glyph, coverage levels row0 = 0 5 15, row1 = 10 0 7; two pixels per byte, high nibble first,
// row stride (w + 1) / 2 = 2 bytes. Pen advance 4, box starts one pixel right of the pen.
const uint8_t kBmp4[] = {0x05, 0xF0, 0xA0, 0x70};
const uint16_t kCp[] = {'A'};
const Glyph kGlyph[] = {{0, 3, 2, 1, 2, 4}};   // offset, w, h, left, top, adv
const Font kF4 = {kBmp4, kCp, kGlyph, 1, 4, 2, 0, 2, 2, 4};
// Same shape at 1 bpp: row0 = 0 1 1, row1 = 1 0 1, one byte per row.
const uint8_t kBmp1[] = {0x60, 0xA0};
const Font kF1 = {kBmp1, kCp, kGlyph, 1, 4, 2, 0, 2, 2, 1};
}  // namespace

static void test_four_bpp_blend() {
    // Expected values are blend565() results worked out by hand for each coverage level.
    uint16_t lut[16], row[4];
    make_blend_lut(0xFFFF, 0x0000, lut);
    CHECK_EQ(render_row(kF4, "A", 0, lut, row, 4), 4);
    CHECK_EQ(row[0], 0x0000);   // left of the box: background
    CHECK_EQ(row[1], 0x0000);   // level 0
    CHECK_EQ(row[2], 0x52AA);   // level 5
    CHECK_EQ(row[3], 0xFFFF);   // level 15
    render_row(kF4, "A", 1, lut, row, 4);
    CHECK_EQ(row[1], 0xAD55);   // level 10
    CHECK_EQ(row[2], 0x0000);   // level 0
    CHECK_EQ(row[3], 0x73AE);   // level 7
}

static void test_four_bpp_blends_against_cell_background() {
    // Edges blend toward the cell's own colour (a highlighted row), never toward black.
    uint16_t lut[16], row[4];
    make_blend_lut(0xF800, 0x001F, lut);       // red on blue
    render_row(kF4, "A", 0, lut, row, 4);
    CHECK_EQ(row[0], 0x001F);                  // untouched pixels are the cell colour
    CHECK_EQ(row[1], 0x001F);                  // level 0 stays the cell colour
    CHECK_EQ(row[2], 0x5015);                  // level 5
    CHECK_EQ(row[3], 0xF800);                  // level 15
    make_blend_lut(0xFFE0, 0x2965, lut);       // yellow on dark grey
    render_row(kF4, "A", 1, lut, row, 4);
    CHECK_EQ(row[1], 0xB5C2);                  // level 10
    CHECK_EQ(row[3], 0x8C63);                  // level 7
}

static void test_four_bpp_column_matches_row() {
    uint16_t lut[16], col[2], row[4];
    make_blend_lut(0xFFFF, 0x0000, lut);
    for (int c = 0; c < 4; c++) {
        render_column(kF4, "A", c, lut, col);
        for (int r = 0; r < 2; r++) {
            render_row(kF4, "A", r, lut, row, 4);
            CHECK_EQ(col[r], row[c]);
        }
    }
}

static void test_one_bpp_still_decodes() {
    uint16_t lut[16], row[4];
    make_blend_lut(0xFFFF, 0x0000, lut);
    render_row(kF1, "A", 0, lut, row, 4);
    CHECK_EQ(row[1], 0x0000);  CHECK_EQ(row[2], 0xFFFF);  CHECK_EQ(row[3], 0xFFFF);
    render_row(kF1, "A", 1, lut, row, 4);
    CHECK_EQ(row[1], 0xFFFF);  CHECK_EQ(row[2], 0x0000);  CHECK_EQ(row[3], 0xFFFF);
}

int main() {
    test_missing_glyph_count();
    test_rot_source_corners();
    test_rotated_matches_bitmap();
    test_rotated_orientation_of_a_glyph();
    test_utf8();
    test_lookup();
    test_render();
    test_font_tables();
    test_four_bpp_blend();
    test_four_bpp_blends_against_cell_background();
    test_four_bpp_column_matches_row();
    test_one_bpp_still_decodes();
    return test_report("test_text");
}
