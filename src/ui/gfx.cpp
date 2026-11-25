// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/gfx.hpp"
#include "k2/core/hot.h"
#include "k2/core/strpack.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui_viewport.hpp"

#include "k2/drivers/lcd.hpp"

namespace k2 {

namespace { constexpr int kMaxLineH = 32; }

K2_HOT void gfx_fill(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (x < 0) { w = static_cast<int16_t>(w + x); x = 0; }
    if (y < 0) { h = static_cast<int16_t>(h + y); y = 0; }
    if (x + w > s.w) w = static_cast<int16_t>(s.w - x);
    if (y + h > s.h) h = static_cast<int16_t>(s.h - y);
    if (w <= 0 || h <= 0) return;
    lcd::fill_rect(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y),
                     static_cast<uint16_t>(w), static_cast<uint16_t>(h), color);
}

K2_HOT Rect gfx_native_rect(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h) {
    if (x < 0 || y < 0) return {0, 0, 0, 0};
    if (x + w > s.w) w = static_cast<int16_t>(s.w - x);
    if (y + h > s.h) h = static_cast<int16_t>(s.h - y);
    if (w <= 0 || h <= 0) return {0, 0, 0, 0};
    return ContentViewport::native_rect(
        {static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y), w, h});
}

K2_HOT void gfx_fill_panel(const Rect &r, uint16_t color) {
    if (r.w <= 0 || r.h <= 0) return;
    ContentViewport::Pause unscaled;   // the box is already in panel pixels: draw it directly
    lcd::fill_rect(static_cast<uint16_t>(r.x), static_cast<uint16_t>(r.y),
                   static_cast<uint16_t>(r.w), static_cast<uint16_t>(r.h), color);
}

K2_HOT void gfx_text(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text,
              uint16_t fg, uint16_t bg, int16_t cell_offset K2_TRUNC_DEF) {
    if (x < 0 || y < 0 || x >= s.w || y >= s.h || text.empty()) return;
    char dec[80];
    text = strpack::unpack(text, dec);
    int px = text_width(f, text);
    int room = s.w - x - cell_offset;
    // Landscape stretches origins but not glyphs: text anchored on the mapped right edge may start in the last source columns
    // yet still end inside the panel region, so the room is what the stretched span leaves.
    if (px > room && ContentViewport::scaled())
        room = ContentViewport::mapped_rect({static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y),
                                             static_cast<int16_t>(s.w - x), 1}).w - cell_offset;
    if (px > room) {
        K2_TRUNC_NOTE(trunc::Kind::Clip, text, room);
        px = room;
    }
    int rows = f.height;
    if (rows > s.h - y) rows = s.h - y;
    const Rect native = ContentViewport::native_rect(
        {static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y),
         static_cast<int16_t>(px), static_cast<int16_t>(rows)}, cell_offset);
    px = native.w;
    rows = native.h;
    if (px <= 0 || rows <= 0) return;
    ContentViewport::Pause unscaled_glyphs;
    int cr0 = 0, cr1 = rows;
    {   // a strip capture keeps only a few rows and re-renders the screen per strip: skip rows outside it
        uint16_t band_y = static_cast<uint16_t>(native.y), band_h = static_cast<uint16_t>(rows);
        if (!lcd::clip_to_band(band_y, band_h)) return;
        cr0 = static_cast<int>(band_y) - native.y;
        cr1 = cr0 + static_cast<int>(band_h);
    }

    uint16_t lut[16];
    make_blend_lut(fg, bg, lut);
    // A row is rendered and streamed in windows of kChunk pixels: consecutive stream_pixels calls continue the
    // open window, so a full-width row buffer (480 B of every paint's stack) is not needed.
    constexpr int kChunk = 80;
    uint16_t line[kChunk];

    lcd::stream_begin(static_cast<uint16_t>(native.x), static_cast<uint16_t>(native.y + cr0),
                     static_cast<uint16_t>(native.x + px - 1), static_cast<uint16_t>(native.y + cr1 - 1));
    for (int r = cr0; r < cr1; r++) {
        for (int x0 = 0; x0 < px; x0 += kChunk) {
            const int n = render_row_span(f, text, r, lut, line, x0, kChunk < px - x0 ? kChunk : px - x0);
            if (n <= 0) break;
            lcd::stream_pixels({line, static_cast<std::size_t>(n)});
        }
    }
    lcd::stream_end();
    K2_TEXT_DRAWN(f, text, s.x + x, s.y + y, native.x, native.y, px, rows, -1);
}

K2_HOT void gfx_text_rot(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text,
                  uint16_t fg, uint16_t bg, TextDir dir K2_TRUNC_DEF) {
    if (text.empty()) return;
    char dec[80];
    text = strpack::unpack(text, dec);
    const int tw = text_width(f, text);
    if (f.height > kMaxLineH || x < 0 || y < 0 || x + f.height > s.w || y + tw > s.h) {
        K2_TRUNC_NOTE(trunc::Kind::Clip, text, s.h - y);   // nothing is drawn at all
        return;
    }
    int sy0 = 0, sy1 = tw;
    {   // as in gfx_text: skip rotated rows outside the capture strip
        uint16_t band_y = static_cast<uint16_t>(s.y + y), band_h = static_cast<uint16_t>(tw);
        if (!lcd::clip_to_band(band_y, band_h)) return;
        sy0 = static_cast<int>(band_y) - (s.y + y);
        sy1 = sy0 + static_cast<int>(band_h);
    }
    uint16_t lut[16];
    make_blend_lut(fg, bg, lut);
    uint16_t col[kMaxLineH];

    lcd::stream_begin(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y + sy0),
                     static_cast<uint16_t>(s.x + x + f.height - 1), static_cast<uint16_t>(s.y + y + sy1 - 1));
    for (int sy = sy0; sy < sy1; sy++) {
        render_rotated_row(f, text, dir, sy, lut, col);
        lcd::stream_pixels({col, static_cast<std::size_t>(f.height)});
    }
    lcd::stream_end();
    K2_TEXT_DRAWN(f, text, s.x + x, s.y + y, s.x + x, s.y + y, f.height, tw, static_cast<int>(dir));
}

namespace {
// One diff key per cell: ASCII as is, a wider glyph folded into 0x80..0xFF (the fonts' few, the arrows and the degree sign,
// fold apart).
K2_HOT char cell_key(uint32_t cp) { return static_cast<char>(cp < 0x80 ? cp : 0x80 | ((cp ^ (cp >> 7)) & 0x7F)); }
constexpr char kBlanks[FieldBase::kMaxChars + 1] = "                                ";
static_assert(sizeof kBlanks == FieldBase::kMaxChars + 1);
}  // namespace

// Cells, not bytes: a two-byte glyph (the degree sign) takes one cell, so right-aligned text still ends on the last one.
K2_HOT void field_draw_buf(const Surface &s, const Font &f, int16_t x, int16_t y, uint8_t chars, Align a,
                           uint16_t fg, uint16_t bg, bool &valid, char *cur,
                           std::string_view text K2_TRUNC_DEF) {
    if (chars > FieldBase::kMaxChars) chars = FieldBase::kMaxChars;
    char next[FieldBase::kMaxChars];
    int len = 0;
    size_t bytes = 0;
    bool ascii = true;
    while (bytes < text.size() && len < chars) {
        const uint32_t cp = next_codepoint(text, bytes);
        ascii = ascii && cp < 0x80;
        next[len++] = cell_key(cp);
    }
    if (bytes < text.size())
        K2_TRUNC_NOTE(trunc::Kind::Cut, text, static_cast<int>(chars) * f.adv);   // a field never wraps or grows
    text = text.substr(0, bytes);
    const int pad = a == Align::Right ? chars - len : 0;
    for (int i = len - 1; i >= 0 && pad; i--) next[pad + i] = next[i];
    for (int i = 0; i < pad; i++) next[i] = ' ';
    for (int i = pad + len; i < chars; i++) next[i] = ' ';

    int first = 0, last = chars - 1;
    if (valid) {
        while (first <= last && next[first] == cur[first]) first++;
        while (last >= first && next[last] == cur[last]) last--;
        if (first > last) return;                       // nothing changed
    }
    for (int i = 0; i < chars; i++) cur[i] = next[i];
    valid = true;
    K2_FIELD_SCOPE(static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(&f)) * 31u + static_cast<unsigned>((s.x + x) * 257 + s.y + y));
    if (ascii) {
        gfx_text(s, f, x, y,
                 std::string_view(next + first, static_cast<size_t>(last - first + 1)), fg, bg,
                 static_cast<int16_t>(first * f.adv) K2_TRUNC_ARG);
        return;
    }
    // a wider glyph: the keys are not the bytes, so the blanks and the text's own bytes each go to their cells
    auto blanks = [&](int c0, int c1) K2_HOT {
        if (c0 <= c1)
            gfx_text(s, f, x, y, std::string_view(kBlanks, static_cast<size_t>(c1 - c0 + 1)), fg, bg,
                     static_cast<int16_t>(c0 * f.adv) K2_TRUNC_ARG);
    };
    const int t0 = first > pad ? first : pad, t1 = last < pad + len - 1 ? last : pad + len - 1;
    if (t0 > t1) {
        blanks(first, last);
        return;
    }
    blanks(first, t0 - 1);
    size_t b0 = 0;
    for (int g = pad; g < t0; g++) next_codepoint(text, b0);
    size_t b1 = b0;
    for (int g = t0; g <= t1; g++) next_codepoint(text, b1);
    gfx_text(s, f, x, y, text.substr(b0, b1 - b0), fg, bg, static_cast<int16_t>(t0 * f.adv) K2_TRUNC_ARG);
    blanks(t1 + 1, last);
}

[[gnu::noinline]] void FieldBase::init_base(const Surface &s, const Font &f, int16_t x, int16_t y, uint8_t chars, uint8_t max_chars,
                          Align a, uint16_t fg, uint16_t bg) {
    surf_ = s; font_ = &f; x_ = x; y_ = y; align_ = a;
    chars_ = chars > max_chars ? max_chars : chars;
    fg_ = fg; bg_ = bg;
    valid_ = false;
}

K2_HOT void FieldBase::set_colors(uint16_t fg, uint16_t bg) {
    if (fg != fg_ || bg != bg_) valid_ = false;
    fg_ = fg;
    bg_ = bg;
}

K2_HOT void FieldBase::draw_buf(char *cur, std::string_view text K2_TRUNC_DEF) {
    field_draw_buf(surf_, *font_, x_, y_, chars_, align_, fg_, bg_, valid_, cur, text K2_TRUNC_ARG);
}

K2_HOT void FieldBase::draw(std::string_view text K2_TRUNC_DEF) {
    field_draw_buf(surf_, *font_, x_, y_, chars_, align_, fg_, bg_, valid_, cur_data(), text K2_TRUNC_ARG);
}

}  // namespace k2
