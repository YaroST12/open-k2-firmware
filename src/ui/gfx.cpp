// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/gfx.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui_viewport.hpp"

#include "k2/drivers/lcd.hpp"

namespace k2 {

namespace { constexpr int kMaxLineH = 32; }

void gfx_fill(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (x < 0) { w = static_cast<int16_t>(w + x); x = 0; }
    if (y < 0) { h = static_cast<int16_t>(h + y); y = 0; }
    if (x + w > s.w) w = static_cast<int16_t>(s.w - x);
    if (y + h > s.h) h = static_cast<int16_t>(s.h - y);
    if (w <= 0 || h <= 0) return;
    lcd::fill_rect(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y),
                     static_cast<uint16_t>(w), static_cast<uint16_t>(h), color);
}

void gfx_text(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text,
              uint16_t fg, uint16_t bg, int16_t cell_offset K2_TRUNC_DEF) {
    if (x < 0 || y < 0 || x >= s.w || y >= s.h || text.empty()) return;
    int px = text_width(f, text);
    if (px > s.w - x - cell_offset) {
        K2_TRUNC_NOTE(trunc::Kind::Clip, text, s.w - x - cell_offset);
        px = s.w - x - cell_offset;
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

    uint16_t lut[16];
    make_blend_lut(fg, bg, lut);
    uint16_t line[kScreenW];

    lcd::stream_begin(static_cast<uint16_t>(native.x), static_cast<uint16_t>(native.y),
                     static_cast<uint16_t>(native.x + px - 1), static_cast<uint16_t>(native.y + rows - 1));
    for (int r = 0; r < rows; r++) {
        int n = render_row(f, text, r, lut, line, px);
        lcd::stream_pixels({line, static_cast<std::size_t>(n)});
    }
    lcd::stream_end();
}

void gfx_text_rot(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text,
                  uint16_t fg, uint16_t bg, TextDir dir K2_TRUNC_DEF) {
    const int tw = text_width(f, text);
    if (text.empty()) return;
    if (f.height > kMaxLineH || x < 0 || y < 0 || x + f.height > s.w || y + tw > s.h) {
        K2_TRUNC_NOTE(trunc::Kind::Clip, text, s.h - y);   // nothing is drawn at all
        return;
    }
    uint16_t lut[16];
    make_blend_lut(fg, bg, lut);
    uint16_t col[kMaxLineH];

    lcd::stream_begin(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y),
                     static_cast<uint16_t>(s.x + x + f.height - 1), static_cast<uint16_t>(s.y + y + tw - 1));
    for (int sy = 0; sy < tw; sy++) {
        render_rotated_row(f, text, dir, sy, lut, col);
        lcd::stream_pixels({col, static_cast<std::size_t>(f.height)});
    }
    lcd::stream_end();
}

void Field::init(const Surface &s, const Font &f, int16_t x, int16_t y, uint8_t chars, Align a,
                 uint16_t fg, uint16_t bg) {
    surf_ = s; font_ = &f; x_ = x; y_ = y; align_ = a;
    chars_ = chars > kMaxChars ? kMaxChars : chars;
    fg_ = fg; bg_ = bg;
    valid_ = false;
}

void Field::set_colors(uint16_t fg, uint16_t bg) {
    if (fg != fg_ || bg != bg_) valid_ = false;
    fg_ = fg;
    bg_ = bg;
}

void Field::draw(std::string_view text K2_TRUNC_DEF) {
    char next[kMaxChars + 1];
    if (text.size() > chars_) K2_TRUNC_NOTE(trunc::Kind::Cut, text, width_px());   // a field never wraps or grows
    size_t len = text.size() < chars_ ? text.size() : chars_;
    size_t pad = align_ == Align::Right ? chars_ - len : 0;
    for (size_t i = 0; i < chars_; i++) next[i] = ' ';
    for (size_t i = 0; i < len; i++) next[pad + i] = text[i];

    int first = 0, last = chars_ - 1;
    if (valid_) {
        while (first <= last && next[first] == cur_[first]) first++;
        while (last >= first && next[last] == cur_[last]) last--;
        if (first > last) return;                       // nothing changed
    }
    for (int i = 0; i < chars_; i++) cur_[i] = next[i];
    valid_ = true;
    gfx_text(surf_, *font_, x_, y_,
             std::string_view(next + first, static_cast<size_t>(last - first + 1)), fg_, bg_,
             static_cast<int16_t>(first * font_->adv) K2_TRUNC_ARG);
}

}  // namespace k2
