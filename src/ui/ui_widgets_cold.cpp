// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Cold/one-shot UI widgets compiled with COLD_SIZE_FLAGS (-Oz).
#include "k2/ui/ui_widgets.hpp"

#include "k2/core/strpack.hpp"
#include "ui_widgets_internal.hpp"
#include "k2/ui/ui_types.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/ui/ui_viewport.hpp"
#include <algorithm>

namespace k2 {

void draw_frame(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int t, uint16_t color) {
    if (w <= 2 * t || h <= 2 * t) { gfx_fill(s, x, y, w, h, color); return; }
    gfx_fill(s, x, y, w, static_cast<int16_t>(t), color);
    gfx_fill(s, x, static_cast<int16_t>(y + h - t), w, static_cast<int16_t>(t), color);
    gfx_fill(s, x, static_cast<int16_t>(y + t), static_cast<int16_t>(t), static_cast<int16_t>(h - 2 * t), color);
    gfx_fill(s, static_cast<int16_t>(x + w - t), static_cast<int16_t>(y + t), static_cast<int16_t>(t), static_cast<int16_t>(h - 2 * t), color);
}

void draw_mark(const Surface &s, int16_t x, int16_t y, int size, bool check, uint16_t fg, uint16_t bg) {
    static constexpr uint16_t kMarkBits[2][16] = {
        {0x0000, 0x0000, 0x0006, 0x000E, 0x001E, 0x003C, 0x0078, 0x60F0,
         0x70E0, 0x79C0, 0x3F80, 0x1F80, 0x0F00, 0x0600, 0x0000, 0x0000},   // check
        {0x0000, 0x0000, 0x300C, 0x381C, 0x1C38, 0x0E70, 0x07E0, 0x03C0,
         0x03C0, 0x07E0, 0x0E70, 0x1C38, 0x381C, 0x300C, 0x0000, 0x0000},   // cross
    };
    if (size == kIconSize) {   // the icon table's own 12 px glyphs: a 16 px source sampled to 12 drew a lopsided cross
        draw_icon(s, x, y, check ? Icon::Check : Icon::Cross, fg, bg);
        return;
    }
    detail::stream_mono(s, x, y, size, 16, kMarkBits[check ? 0 : 1], 15, fg, bg);
}

int marked_width(const Font &f, std::string_view text) {
    char dec[64];
    text = strpack::unpack(text, dec);
    int w = 0;
    for (char ch : text) w += (ch == kMarkCheck || ch == kMarkCross) ? mark_cell(f) : f.adv;
    return w;
}

int draw_marked_text(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text, uint16_t fg, uint16_t bg) {
    char dec[64];
    text = strpack::unpack(text, dec);
    size_t i = 0;
    while (i < text.size()) {
        const char ch = text[i];
        if (ch == kMarkCheck || ch == kMarkCross) {
            const int sz = mark_size(f);
            gfx_fill(s, x, static_cast<int16_t>(y + f.ascent - sz), 2, static_cast<int16_t>(sz), bg);
            draw_mark(s, static_cast<int16_t>(x + 2), static_cast<int16_t>(y + f.ascent - sz), sz, ch == kMarkCheck, ch == kMarkCheck ? kOk : kError, bg);
            gfx_fill(s, static_cast<int16_t>(x + 2 + sz), static_cast<int16_t>(y + f.ascent - sz), 2, static_cast<int16_t>(sz), bg);
            x = static_cast<int16_t>(x + mark_cell(f));
            i++;
            continue;
        }
        size_t j = i;
        while (j < text.size() && text[j] != kMarkCheck && text[j] != kMarkCross) j++;
        gfx_text(s, f, x, y, text.substr(i, j - i), fg, bg);
        x = static_cast<int16_t>(x + static_cast<int>(j - i) * f.adv);
        i = j;
    }
    return x;
}

void draw_label_box(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, std::string_view text,
                    uint16_t fg, uint16_t bg) {
    const TextOrigin p = center_in_box({x, y, w, h}, badge_text_bounds(kLabelFont, text));
    const int x1 = x + w, y1 = y + h;
    const int ax0 = std::max<int>(x, p.x), ax1 = std::min<int>(x1, p.x + text_width(kLabelFont, text));
    const int ay0 = std::max<int>(y, p.y), ay1 = std::min<int>(y1, p.y + kLabelFont.height);
    if (ax0 >= ax1 || ay0 >= ay1) {
        gfx_fill(s, x, y, w, h, bg);
    } else {
        if (ay0 > y) gfx_fill(s, x, y, w, static_cast<int16_t>(ay0 - y), bg);
        if (ay1 < y1) gfx_fill(s, x, static_cast<int16_t>(ay1), w, static_cast<int16_t>(y1 - ay1), bg);
        if (ax0 > x) gfx_fill(s, x, static_cast<int16_t>(ay0), static_cast<int16_t>(ax0 - x), static_cast<int16_t>(ay1 - ay0), bg);
        if (ax1 < x1) gfx_fill(s, static_cast<int16_t>(ax1), static_cast<int16_t>(ay0), static_cast<int16_t>(x1 - ax1), static_cast<int16_t>(ay1 - ay0), bg);
    }
    gfx_text(s, kLabelFont, p.x, p.y, text, fg, bg);
}

void draw_pill_box(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, std::string_view text, uint16_t ink,
                   uint16_t fill, uint16_t frame, uint16_t ground, bool centred) {
    if (ContentViewport::scaled()) {
        const Rect src{static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y), w, h};
        const Rect m = ContentViewport::mapped_rect(src), n = ContentViewport::native_box(src, centred);
        ContentViewport::Pause unscaled;
        const Surface panel{0, 0, kScreenW, kScreenH};
        // the mapped box's columns either side of the native pill (a stretch widens it), over the rows of both boxes
        const int16_t y0 = std::min(m.y, n.y), y1 = static_cast<int16_t>(std::max(m.y + m.h, n.y + h) - y0);
        if (n.x > m.x) gfx_fill(panel, m.x, y0, static_cast<int16_t>(n.x - m.x), y1, ground);
        if (m.x + m.w > n.x + w) gfx_fill(panel, static_cast<int16_t>(n.x + w), y0, static_cast<int16_t>(m.x + m.w - n.x - w), y1, ground);
        draw_pill_box(panel, n.x, n.y, w, h, text, ink, fill, frame, ground);
        return;
    }
    if (frame != fill) {
        draw_frame(s, x, y, w, h, 1, frame);
        draw_label_box(s, static_cast<int16_t>(x + 1), static_cast<int16_t>(y + 1), static_cast<int16_t>(w - 2),
                       static_cast<int16_t>(h - 2), text, ink, fill);
    } else {
        draw_label_box(s, x, y, w, h, text, ink, fill);
    }
}

void draw_sparkline(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *samples,
                    uint16_t color, uint16_t tint, uint16_t bg) {
    if (!detail::fits(s, x, y, w, h) || w > detail::kMaxW) return;
    uint16_t line[detail::kMaxW];
    lcd::stream_begin(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y),
                      static_cast<uint16_t>(s.x + x + w - 1), static_cast<uint16_t>(s.y + y + h - 1));
    for (int r = 0; r < h; r++) {
        const int level = h - 1 - r;
        const bool grid = r == h / 4 || r == h / 2 || r == (h * 3) / 4;
        const uint16_t base = grid && r > 0 ? kRule : bg;
        for (int c = 0; c < w; c++) {
            const int v = samples[c];
            if (v == kSparkNone) { line[c] = base; continue; }   // no sample yet in this column
            const int prev = c && samples[c - 1] != kSparkNone ? samples[c - 1] : v;
            switch (spark_cell(level, v, prev)) {
                case Cell::Line: line[c] = color; break;
                case Cell::Fill: line[c] = tint; break;
                default: line[c] = base; break;
            }
        }
        lcd::stream_pixels({line, static_cast<std::size_t>(w)});
    }
    lcd::stream_end();
}

void draw_range_trace(const Surface &s, int16_t x, int16_t y, int cols, int xs, int16_t h, const uint8_t *lo,
                      const uint8_t *hi, uint8_t mark_a, uint16_t ca, uint8_t mark_b, uint16_t cb, uint16_t color,
                      uint16_t bg) {
    const int w = cols * xs;
    if (cols <= 0 || xs <= 0 || h < 4 || !detail::fits(s, x, y, w, h) || w > detail::kMaxW) return;
    int r0 = 0, r1 = h;
    if (!detail::clip_box_to_band(s, x, y, w, h, r0, r1)) return;
    uint16_t line[detail::kMaxW];
    lcd::stream_begin(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y + r0),
                      static_cast<uint16_t>(s.x + x + w - 1), static_cast<uint16_t>(s.y + y + r1 - 1));
    for (int r = r0; r < r1; r++) {
        const bool grid = r == h / 4 || r == h / 2 || r == (h * 3) / 4;
        for (int c = 0; c < cols; c++) {
            const bool in_band = r >= range_row(hi[c], h) && r <= range_row(lo[c], h);
            uint16_t px = in_band ? color : c == mark_a ? ca : c == mark_b ? cb : grid && r > 0 ? kRule : bg;
            for (int k = 0; k < xs; k++) line[c * xs + k] = px;
        }
        lcd::stream_pixels({line, static_cast<std::size_t>(w)});
    }
    lcd::stream_end();
}

bool draw_footer_budgeted(const Surface &s, FooterTone tone, std::string_view text, Elem &e, PaintBudget *bg K2_TRUNC_DEF) {
    if (bg && bg->more) return false;
    uint32_t sg = mix_u32(mix_init(), static_cast<uint32_t>(tone) * 31u + 1u);
    sg = mix(sg, text);
    return e.paint_budgeted(sg, bg, static_cast<int>(s.w) * 18, [&] { draw_footer(s, tone, text K2_TRUNC_ARG); });
}

bool status_toast(Toast &t, uint32_t now_ms, uint32_t &shown, FooterTone tone, std::string_view text) {
    const uint32_t sg = mix(mix_u32(mix_init(), static_cast<uint32_t>(tone) * 31u + 1u), text) | 1u;
    if (sg == shown) return false;
    shown = sg;
    t.show(tone, text, now_ms);
    return true;
}

void IconCell::draw(Icon icon, uint16_t fg, uint16_t bg) {
    const uint32_t sg = mix_u32(mix_init(), static_cast<uint32_t>(icon) * 31u + fg * 7u + bg * 131u);
    e.paint(sg, [&] { draw_icon(surf, x, y, icon, fg, bg); });
}

void draw_top_pager(const Surface &s, int count, int current) {
    detail::BandOp op{};
    op.kind = detail::BandKind::Pager;
    op.count = count;
    op.current = current;
    detail::draw_band(s, detail::kReadoutX, 0, kBarH - 1, op);
}

namespace {
// The text itself, or its start plus "..." (the cut only; callers say whether the gate counts it).
std::string_view cut_end(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf], bool &cut) {
    text = strpack::unpack(text, buf);
    if (text_width(f, text) <= max_px) { cut = false; return text; }
    cut = true;
    const int cells = max_px / f.adv;
    int keep = cells - 3;                               // characters kept before the dots
    size_t i = 0, len = 0;
    for (int n = 0; n < keep && i < text.size(); n++) {
        i++;
        while (i < text.size() && (static_cast<uint8_t>(text[i]) & 0xC0) == 0x80) i++;
        if (i > kFitBuf - 4) break;
        len = i;
    }
    for (size_t k = 0; k < len; k++) buf[k] = text[k];
    size_t dots = cells >= 3 ? 3 : cells > 0 ? static_cast<size_t>(cells) : 0;
    for (size_t k = 0; k < dots; k++) buf[len + k] = '.';
    return {buf, len + dots};
}

std::string_view cut_mid(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf], bool &cut) {
    constexpr int kTail = 4, kDots = 2;
    text = strpack::unpack(text, buf);
    if (text_width(f, text) <= max_px) { cut = false; return text; }
    const int cells = max_px / f.adv;
    if (cells < kTail + kDots + 1) return cut_end(f, text, max_px, buf, cut);
    // Tail: the start of the last kTail whole characters.
    size_t t = text.size();
    for (int n = 0; n < kTail && t > 0; n++) {
        t--;
        while (t > 0 && (static_cast<uint8_t>(text[t]) & 0xC0) == 0x80) t--;
    }
    // Head: cells - dots - tail whole characters from the start.
    size_t head = 0;
    for (int n = 0; n < cells - kDots - kTail && head < t; n++) {
        head++;
        while (head < t && (static_cast<uint8_t>(text[head]) & 0xC0) == 0x80) head++;
    }
    const size_t tail = text.size() - t;
    if (head + kDots + tail > kFitBuf) return cut_end(f, text, max_px, buf, cut);   // not for 48 byte buffers
    cut = true;
    size_t n = 0;
    for (size_t k = 0; k < head; k++) buf[n++] = text[k];
    for (int k = 0; k < kDots; k++) buf[n++] = '.';
    for (size_t k = 0; k < tail; k++) buf[n++] = text[t + k];
    return {buf, n};
}
}  // namespace

std::string_view fit_text(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf] K2_TRUNC_DEF) {
    bool cut = false;
    const std::string_view out = cut_end(f, text, max_px, buf, cut);
    if (cut) K2_TRUNC_NOTE(trunc::Kind::Cut, text, max_px);
    return out;
}

std::string_view fit_data(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf] K2_TRUNC_DEF) {
    bool cut = false;
    const std::string_view out = cut_end(f, text, max_px, buf, cut);
    if (cut) K2_TRUNC_NOTE(trunc::Kind::Data, text, max_px);
    return out;
}

std::string_view fit_text_mid(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf] K2_TRUNC_DEF) {
    bool cut = false;
    const std::string_view out = cut_mid(f, text, max_px, buf, cut);
    if (cut) K2_TRUNC_NOTE(trunc::Kind::Data, text, max_px);
    return out;
}

}  // namespace k2
