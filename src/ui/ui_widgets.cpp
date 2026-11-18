// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_widgets.hpp"
#include "k2/core/hot.h"
#include "k2/ui/ui_types.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/ui_viewport.hpp"
#include <algorithm>
#include <array>

namespace k2 {

namespace {

constexpr int kMaxW = kScreenW;
constexpr int kReadoutX = 140;   // the top bar's right-hand readout area starts here (draw_top_readout)

K2_HOT bool fits(const Surface &s, int x, int y, int w, int h) {
    return x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= s.w && y + h <= s.h;
}

K2_HOT bool clip_box_to_band(const Surface &s, int x, int y, int w, int h, int &r0, int &r1) {
    r0 = 0;
    r1 = h;
    if (!ContentViewport::stretched()) {
        if (s.y + y < 0) return true;
        uint16_t by = static_cast<uint16_t>(s.y + y), bh = static_cast<uint16_t>(h);
        if (!lcd::clip_to_band(by, bh)) return false;
        r0 = static_cast<int>(by) - (s.y + y);
        r1 = r0 + static_cast<int>(bh);
        return true;
    }
    uint16_t b0 = 0, b1 = 0;
    if (ContentViewport::panel_band(b0, b1)) {
        const Rect p = ContentViewport::mapped_rect(
            {static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y),
             static_cast<int16_t>(w), static_cast<int16_t>(h)});
        if (p.h <= 0 || p.y > b1 || p.y + p.h <= b0) return false;
    }
    return true;
}

struct BandRow {
    uint16_t pixels[kScreenW];
    int origin, width, y;

    K2_HOT void fill(int x, int w, uint16_t color) {
        const int first = std::max(x - origin, 0);
        const int last = std::min(x + w - origin, width);
        for (int i = first; i < last; i++) pixels[i] = color;
    }

    [[gnu::noinline]] K2_HOT void text(const Font &font, int x, int top, std::string_view value, uint16_t fg, uint16_t bg) {
        if (y < top || y >= top + font.height || x < origin || x >= origin + width) return;
        uint16_t lut[16];
        make_blend_lut(fg, bg, lut);
        render_row(font, value, y - top, lut, pixels + x - origin, origin + width - x);
    }

    K2_HOT void icon(int x, int top, Icon icon, uint16_t fg, uint16_t bg) {
        if (y < top || y >= top + kIconSize) return;
        const uint16_t bits = kIconBits[static_cast<int>(icon)][y - top];
        for (int c = 0; c < kIconSize; c++)
            fill(x + c, 1, (bits >> (kIconSize - 1 - c)) & 1 ? fg : bg);
    }

    // One row of the physical chevron whose top-left is (x, top); a no-op outside its rows.
    K2_HOT void chevron(int x, int top, bool toward_top, unsigned rotation, uint16_t color) {
        const int r = y - top;
        if (r < 0 || r >= physical_chevron_h(rotation)) return;
        const unsigned bits = physical_chevron_row(toward_top, rotation, r);
        for (int c = 0; c < physical_chevron_w(rotation); c++)
            if (bits >> c & 1) fill(x + c, 1, color);
    }

    K2_HOT void last_pill(std::string_view value) {
        const TextBounds tb = badge_text_bounds(kLabelFont, value);
        const int w = tb.w + 8;
        const int x = 222 - w;
        if (y >= 2 && y < 16) fill(x, w, kStale);
        const TextOrigin p = center_in_box({static_cast<int16_t>(x), 2, static_cast<int16_t>(w), 14}, tb);
        text(kLabelFont, p.x, p.y, value, kBg, kStale);
    }
};

// The part of a band that goes to the panel, in band pixels from its top-left (columns c0..c1-1, rows r0..r1-1). The rows
// are still composed whole (cheap), only the window is narrowed: a repaint of the cells that changed sends just those.
struct BandClip {
    int c0 = 0, c1 = kScreenW, r0 = 0, r1 = kScreenW;
};

constexpr int kSlotPx = ReadoutShown::kCells * kLabelAdv;
K2_HOT std::string_view slot_text(std::string_view value) { return text_width(kLabelFont, value) > kSlotPx ? "OVR" : value; }

K2_HOT void readout_row(BandRow &row, StatusReadout mode, std::string_view v, std::string_view a, Arrow flow, uint8_t rotation,
                 std::string_view last_text) {
    if (mode == StatusReadout::Live) {
        if (flow != Arrow::Idle) {   // slot x 140..149, centred on the 12 px text rows
            const unsigned rot = rotation & 3u;
            row.chevron(ReadoutShown::kChevX + (kChevW - physical_chevron_w(rot)) / 2,
                        kBarTextY + (static_cast<int>(kLabelFont.height) - physical_chevron_h(rot)) / 2,
                        flow == Arrow::Up, rot, kInk);
        }
        // Fixed slots right-aligned so the digits never jitter horizontally, each led by a 3 px colour key (the
        // quantity's spot colour: the text itself is ink). Values too wide for their slot show "OVR".
        if (row.y >= kMastKeyY && row.y < kMastKeyY + kMastKeyH) {
            row.fill(kMastVoltKeyX, kMastKeyW, kSpotVolt);
            row.fill(kMastAmpKeyX, kMastKeyW, kSpotAmp);
        }
        row.text(kLabelFont, ReadoutShown::kVoltX + kSlotPx - text_width(kLabelFont, v), kBarTextY, v, kInk, kBar);
        row.text(kLabelFont, ReadoutShown::kAmpX + kSlotPx - text_width(kLabelFont, a), kBarTextY, a, kInk, kBar);
    } else if (mode == StatusReadout::Stale) {
        row.last_pill(last_text);   // BandRow-native: no nested stream inside the band write
    }
}

enum class BandKind : uint8_t { Footer, StatusLeft, Readout, Pager };
struct BandOp {
    BandKind kind = BandKind::Footer;
    FooterStyle fst{};
    const Font *font = nullptr;
    std::string_view s0{}, s1{}, s2{};
    int16_t fy = 0, tx = 0, ty = 0, sep = 0;
    bool flag = false;
    Icon icon = Icon::Bolt;
    StatusReadout mode = StatusReadout::None;
    Arrow flow = Arrow::Idle;
    uint8_t rotation = 0;
    int count = 0, current = 0;
};

[[gnu::noinline]] K2_HOT void draw_band(const Surface &s, int x, int y, int h, const BandOp &op, int max_w = kScreenW, BandClip clip = {}) {
    const int width = std::min<int>(s.w - x, std::min<int>(kScreenW, max_w));
    h = std::min<int>(h, s.h - y);
    if (x < 0 || y < 0 || width <= 0 || h <= 0) return;
    const Rect band = ContentViewport::mapped_rect(
        {static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y),
         static_cast<int16_t>(width), static_cast<int16_t>(h)});
    ContentViewport::Pause unscaled_band;
    const int c0 = std::max(clip.c0, 0), c1 = std::min<int>(clip.c1, band.w);
    const int r0 = std::max(clip.r0, 0), r1 = std::min<int>(clip.r1, band.h);
    if (c0 >= c1 || r0 >= r1) return;
    int cr0 = r0, cr1 = r1;
    {   // a band wholly outside the capture strip has nothing to compose
        uint16_t by = static_cast<uint16_t>(band.y + r0), bh = static_cast<uint16_t>(r1 - r0);
        if (!lcd::clip_to_band(by, bh)) return;
        cr0 = static_cast<int>(by) - band.y;
        cr1 = cr0 + static_cast<int>(bh);
    }
    BandRow row{{}, x, band.w, y};
    lcd::stream_begin(static_cast<uint16_t>(band.x + c0), static_cast<uint16_t>(band.y + cr0),
                      static_cast<uint16_t>(band.x + c1 - 1), static_cast<uint16_t>(band.y + cr1 - 1));
    for (int r = cr0; r < cr1; r++) {
        row.y = y + r;
        if (op.kind != BandKind::Footer) row.fill(x, band.w, kBar);
        switch (op.kind) {
        case BandKind::Footer:
            row.fill(0, kScreenW, row.y == op.fy && op.fst.rule ? op.fst.rule : op.fst.fill);
            if (op.fst.bar) row.fill(0, kFooterBarW, op.fst.bar);
            row.text(kSmall, kFooterTextX, op.fy + 1, op.s0, op.fst.text, op.fst.fill);
            break;
        case BandKind::StatusLeft:
            if (op.flag) row.icon(kMastIconX, kBarTextY, op.icon, kInk, kBar);
            row.text(*op.font, op.tx, op.ty, op.s0, kInk, kBar);
            if (!op.s1.empty()) {
                row.fill(op.sep, 1, kInk3);
                row.text(kLabelFont, op.sep + 5, kBarTextY, op.s1, kInk2, kBar);
            }
            break;
        case BandKind::Readout:
            readout_row(row, op.mode, op.s0, op.s1, op.flow, op.rotation, op.s2);
            break;
        case BandKind::Pager: {
            const int pr = row.y - kPagerY;
            if (pr >= 0 && pr < kPagerBox) {
                const bool edge = pr == 0 || pr == kPagerBox - 1;
                for (int i = 0; i < op.count; i++) {
                    const int px0 = kPagerRight - (op.count - i) * kPagerPitch + (kPagerPitch - kPagerBox);
                    if (i == op.current || edge) row.fill(px0, kPagerBox, kInk);
                    else { row.fill(px0, 1, kInk); row.fill(px0 + kPagerBox - 1, 1, kInk); }
                }
            }
            break;
        }
        }
        lcd::stream_pixels({row.pixels + c0, static_cast<std::size_t>(c1 - c0)});
    }
    lcd::stream_end();
}

K2_HOT void stream_mono(const Surface &s, int16_t x, int16_t y, int sz, int src,
                        const uint16_t *rows, int msb, uint16_t fg, uint16_t bg) {
    if (sz <= 0 || sz > 16 || !fits(s, x, y, sz, sz)) return;
    int r0 = 0, r1 = sz;
    if (!clip_box_to_band(s, x, y, sz, sz, r0, r1)) return;
    uint16_t line[16];
    lcd::stream_begin(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y + r0),
                      static_cast<uint16_t>(s.x + x + sz - 1), static_cast<uint16_t>(s.y + y + r1 - 1));
    for (int r = r0; r < r1; r++) {
        const uint16_t bits = rows[sz == src ? r : (r * src) / sz];
        for (int c = 0; c < sz; c++) {
            const int sc = sz == src ? c : (c * src) / sz;
            line[c] = (bits >> (msb - sc)) & 1 ? fg : bg;
        }
        lcd::stream_pixels({line, static_cast<std::size_t>(sz)});
    }
    lcd::stream_end();
}

}  // namespace

K2_HOT void fill_round(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int, uint16_t color) {
    gfx_fill(s, x, y, w, h, color);
}

void fill_round_hole(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int, uint16_t color, int16_t hx, int16_t hy,
                     int16_t hw, int16_t hh) {
    const int x1 = x + w, y1 = y + h;
    const int ax0 = std::max<int>(x, hx), ax1 = std::min<int>(x1, hx + hw);
    const int ay0 = std::max<int>(y, hy), ay1 = std::min<int>(y1, hy + hh);
    if (ax0 >= ax1 || ay0 >= ay1) { gfx_fill(s, x, y, w, h, color); return; }
    if (ay0 > y) gfx_fill(s, x, y, w, static_cast<int16_t>(ay0 - y), color);
    if (ay1 < y1) gfx_fill(s, x, static_cast<int16_t>(ay1), w, static_cast<int16_t>(y1 - ay1), color);
    if (ax0 > x) gfx_fill(s, x, static_cast<int16_t>(ay0), static_cast<int16_t>(ax0 - x), static_cast<int16_t>(ay1 - ay0), color);
    if (ax1 < x1) gfx_fill(s, static_cast<int16_t>(ax1), static_cast<int16_t>(ay0), static_cast<int16_t>(x1 - ax1), static_cast<int16_t>(ay1 - ay0), color);
}

K2_HOT void frame_round(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int, int t, uint16_t color) {
    if (w <= 2 * t || h <= 2 * t) { gfx_fill(s, x, y, w, h, color); return; }
    gfx_fill(s, x, y, w, static_cast<int16_t>(t), color);
    gfx_fill(s, x, static_cast<int16_t>(y + h - t), w, static_cast<int16_t>(t), color);
    gfx_fill(s, x, static_cast<int16_t>(y + t), static_cast<int16_t>(t), static_cast<int16_t>(h - 2 * t), color);
    gfx_fill(s, static_cast<int16_t>(x + w - t), static_cast<int16_t>(y + t), static_cast<int16_t>(t), static_cast<int16_t>(h - 2 * t), color);
}

K2_HOT void draw_icon(const Surface &s, int16_t x, int16_t y, Icon icon, uint16_t fg, uint16_t bg) {
    stream_mono(s, x, y, kIconSize, kIconSize, kIconBits[static_cast<int>(icon)], kIconSize - 1, fg, bg);
}

void draw_mark(const Surface &s, int16_t x, int16_t y, int size, bool check, uint16_t fg, uint16_t bg) {
    static constexpr uint16_t kMarkBits[2][16] = {
        {0x0000, 0x0000, 0x0006, 0x000E, 0x001E, 0x003C, 0x0078, 0x60F0,
         0x70E0, 0x79C0, 0x3F80, 0x1F80, 0x0F00, 0x0600, 0x0000, 0x0000},   // check
        {0x0000, 0x0000, 0x300C, 0x381C, 0x1C38, 0x0E70, 0x07E0, 0x03C0,
         0x03C0, 0x07E0, 0x0E70, 0x1C38, 0x381C, 0x300C, 0x0000, 0x0000},   // cross
    };
    stream_mono(s, x, y, size, 16, kMarkBits[check ? 0 : 1], 15, fg, bg);
}

int marked_width(const Font &f, std::string_view text) {
    int w = 0;
    for (char ch : text) w += (ch == kMarkCheck || ch == kMarkCross) ? mark_cell(f) : f.adv;
    return w;
}

int draw_marked_text(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text, uint16_t fg, uint16_t bg) {
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
    const TextBounds tb = badge_text_bounds(kLabelFont, text);
    if (ContentViewport::scaled()) {
        gfx_fill(s, x, y, w, h, bg);
        const Rect mb = ContentViewport::mapped_rect(
            {static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y), w, h});
        const TextOrigin p = center_in_box(mb, tb);
        ContentViewport::Pause unscaled;
        gfx_text({0, 0, kScreenW, kScreenW}, kLabelFont, p.x, p.y, text, fg, bg);
    } else {
        const TextOrigin p = center_in_box({x, y, w, h}, tb);
        fill_round_hole(s, x, y, w, h, 0, bg, p.x, p.y,
                        static_cast<int16_t>(text_width(kLabelFont, text)), static_cast<int16_t>(kLabelFont.height));
        gfx_text(s, kLabelFont, p.x, p.y, text, fg, bg);
    }
}

int draw_pill(const Surface &s, int16_t x, int16_t y, std::string_view text, uint16_t color, uint16_t tint,
              bool solid) {
    const int w = pill_width(static_cast<int>(text.size()));
    draw_label_box(s, x, y, static_cast<int16_t>(w), kPillH, text, solid ? kBg : color, solid ? color : tint);
    return w;
}

void draw_gauge(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int fill_px, uint16_t color,
                int prev_px) {
    if (fill_px > w) fill_px = w;
    if (fill_px < 0) fill_px = 0;
    if (prev_px < 0) {   // full repaint
        if (fill_px > 0) gfx_fill(s, x, y, static_cast<int16_t>(fill_px), h, color);
        if (fill_px < w) gfx_fill(s, static_cast<int16_t>(x + fill_px), y, static_cast<int16_t>(w - fill_px), h, kRule);
        return;
    }
    if (prev_px > w) prev_px = w;
    if (fill_px > prev_px) gfx_fill(s, static_cast<int16_t>(x + prev_px), y, static_cast<int16_t>(fill_px - prev_px), h, color);
    else if (fill_px < prev_px) gfx_fill(s, static_cast<int16_t>(x + fill_px), y, static_cast<int16_t>(prev_px - fill_px), h, kRule);
}

void draw_triangle(const Surface &s, int16_t x, int16_t y, int h, bool right, uint16_t color) {
    for (int r = 0; r < h; r++) {
        const int w = tri_row_w(h, r);
        gfx_fill(s, static_cast<int16_t>(right ? x : x + tri_width(h) - w), static_cast<int16_t>(y + r),
                 static_cast<int16_t>(w), 1, color);
    }
}

void draw_chevron(const Surface &s, int16_t x, int16_t y, bool up, uint16_t color) {
    for (int k = 0; k < kChevH; k++) {
        const int l = chevron_left(up, k);
        const int yy = y + k;
        if (l == 8 - l) {   // apex row: both arms overlap
            gfx_fill(s, static_cast<int16_t>(x + l), static_cast<int16_t>(yy), 2, 1, color);
        } else {
            gfx_fill(s, static_cast<int16_t>(x + l), static_cast<int16_t>(yy), 2, 1, color);
            gfx_fill(s, static_cast<int16_t>(x + 8 - l), static_cast<int16_t>(yy), 2, 1, color);
        }
    }
}

void draw_port_male(const Surface &s, int16_t x, int16_t y, uint16_t color, uint16_t bg) {
    fill_round(s, x, y, 18, 8, 3, color);
    gfx_fill(s, static_cast<int16_t>(x + 4), static_cast<int16_t>(y + 3), 10, 2, bg);
}

void draw_port_female(const Surface &s, int16_t x, int16_t y, uint16_t color) {
    frame_round(s, x, y, 18, 8, 3, 1, color);
    gfx_fill(s, static_cast<int16_t>(x + 4), static_cast<int16_t>(y + 3), 10, 2, color);
}

K2_HOT void draw_sparkline(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *samples,
                    uint16_t color, uint16_t tint, uint16_t bg) {
    if (!fits(s, x, y, w, h) || w > kMaxW) return;
    uint16_t line[kMaxW];
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

K2_HOT void draw_range_trace(const Surface &s, int16_t x, int16_t y, int cols, int xs, int16_t h, const uint8_t *lo,
                      const uint8_t *hi, uint8_t mark_a, uint16_t ca, uint8_t mark_b, uint16_t cb, uint16_t color,
                      uint16_t bg) {
    const int w = cols * xs;
    if (cols <= 0 || xs <= 0 || h < 4 || !fits(s, x, y, w, h) || w > kMaxW) return;
    int r0 = 0, r1 = h;
    if (!clip_box_to_band(s, x, y, w, h, r0, r1)) return;
    uint16_t line[kMaxW];
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

namespace { bool g_footer_error; }
bool footer_error_shown() { return g_footer_error; }
void footer_error_clear() { g_footer_error = false; }

K2_HOT void draw_footer(const Surface &s, FooterTone tone, std::string_view text K2_TRUNC_DEF) {
    g_footer_error = tone == FooterTone::Error;
    const int16_t fy = static_cast<int16_t>(s.h - 18);   // the footer hugs the content bottom (kFooterY on a full-height surface)
    const FooterStyle st = footer_style(tone);
    char buf[kFitBuf];                                  // a footer that would run off the surface ends in "..."
    const auto fitted = fit_text(kSmall, text, std::min<int>(kFooterTextPx, s.w - kFooterTextX), buf K2_TRUNC_ARG);
    BandOp op{};
    op.kind = BandKind::Footer;
    op.fst = st;
    op.s0 = fitted;
    op.fy = fy;
    draw_band(s, 0, fy, s.h - fy, op);
}

void draw_footer_cached(const Surface &s, FooterTone tone, std::string_view text, Elem &e K2_TRUNC_DEF) {
    draw_footer_budgeted(s, tone, text, e, nullptr K2_TRUNC_ARG);
}

bool draw_footer_budgeted(const Surface &s, FooterTone tone, std::string_view text, Elem &e, PaintBudget *bg K2_TRUNC_DEF) {
    uint32_t sg = mix_u32(mix_init(), static_cast<uint32_t>(tone) * 31u + 1u);
    sg = mix(sg, text);
    return e.paint_budgeted(sg, bg, static_cast<int>(s.w) * 18, [&] { draw_footer(s, tone, text K2_TRUNC_ARG); });
}

void IconCell::draw(Icon icon, uint16_t fg, uint16_t bg) {
    const uint32_t sg = mix_u32(mix_init(), static_cast<uint32_t>(icon) * 31u + fg * 7u + bg * 131u);
    e.paint(sg, [&] { draw_icon(surf, x, y, icon, fg, bg); });
}

// Fieldbook masthead: no fill (the band is the ground), the 12 px icon at x8, the title in font 16 at x8 (x26 behind
// the icon) or, when that would not fit before the badge or the readout, in font 12; an exception badge (a held Rd pin)
// right-aligned to `right` behind a 1 px rule; the accent rule under it (row kBarH-1; draw_top_bar adds the second row).
K2_HOT void draw_status_left(const Surface &s, bool has_icon, Icon icon, std::string_view title, int16_t right,
                      std::string_view badge, bool readout_follows) {
    char buf[kFitBuf];
    const int badge_w = static_cast<int>(badge.size() * kLabelAdv);
    const int sep = badge.empty() ? right : right - badge_w - 5;   // 4 px gap, 1 px rule before the badge
    const bool big = text_width(kSmall, title) <= sep - 4 - (has_icon ? kMastTitleIconX : kMastTitleX);
    // The font 12 fallback sits tighter: 2 px after the icon, 3 px before the badge rule.
    const int x = !has_icon ? kMastTitleX : big ? kMastTitleIconX : kMastIconX + kIconSize + 2;
    const Font &tf = big ? kSmall : kLabelFont;
    const auto fitted = fit_text(tf, title, sep - (big ? 4 : 3) - x, buf);
    const int ty = big ? kMastTitleY : kBarTextY;
    BandOp op{};
    op.kind = BandKind::StatusLeft;
    op.font = &tf;
    op.s0 = fitted;
    op.s1 = badge;
    op.tx = static_cast<int16_t>(x);
    op.ty = static_cast<int16_t>(ty);
    op.sep = static_cast<int16_t>(sep);
    op.flag = has_icon && fits(s, kMastIconX, kBarTextY, kIconSize, kIconSize);
    op.icon = icon;
    draw_band(s, 0, 0, kBarH - 1, op, readout_follows ? kReadoutX : kScreenW);
    gfx_fill(s, 0, static_cast<int16_t>(kBarH - 1), s.w, 1, kAccent);   // the masthead rule
}

K2_HOT void draw_top_readout(const Surface &s, StatusReadout mode, std::string_view volts, std::string_view amps,
                      Arrow flow, uint8_t rotation, std::string_view last_text) {
    const std::string_view v = slot_text(volts), a = slot_text(amps);
    if (mode == StatusReadout::Live) {
        if (v.data() != volts.data()) K2_TRUNC_NOTE_HERE(trunc::Kind::Data, volts, kSlotPx);   // a measurement, not copy
        if (a.data() != amps.data()) K2_TRUNC_NOTE_HERE(trunc::Kind::Data, amps, kSlotPx);
    }
    BandOp op{};
    op.kind = BandKind::Readout;
    op.mode = mode;
    op.s0 = v;
    op.s1 = a;
    op.s2 = last_text;
    op.flow = flow;
    op.rotation = rotation;
    draw_band(s, kReadoutX, 0, kBarH - 1, op);
}

K2_HOT void draw_top_readout(const Surface &s, StatusReadout mode, std::string_view volts, std::string_view amps, Arrow flow,
                      uint8_t rotation, ReadoutShown &shown) {
    ReadoutShown now;
    now.mode = mode;
    now.flow = mode == StatusReadout::Live ? flow : Arrow::Idle;
    now.rotation = rotation;
    const std::string_view v = slot_text(volts), a = slot_text(amps);
    const bool cells = ReadoutShown::set(now.volts, v) & ReadoutShown::set(now.amps, a);
    const bool delta = cells && shown.valid && mode == StatusReadout::Live && shown.mode == StatusReadout::Live &&
                       shown.rotation == rotation;
    if (!delta) {
        draw_top_readout(s, mode, volts, amps, flow, rotation);
    } else {
        const ReadoutDelta d = readout_delta(shown, now);
        BandOp op{};
        op.kind = BandKind::Readout;
        op.mode = mode;
        op.s0 = v;
        op.s1 = a;
        op.flow = flow;
        op.rotation = rotation;
        for (int i = 0; i < d.n; i++)
            draw_band(s, kReadoutX, 0, kBarH - 1, op, kScreenW,
                      BandClip{d.span[i].x0 - kReadoutX, d.span[i].x1 - kReadoutX, kBarTextY,
                               kBarTextY + static_cast<int>(kLabelFont.height)});
    }
    now.valid = cells;
    shown = now;
}

K2_HOT void draw_top_pager(const Surface &s, int count, int current) {
    BandOp op{};
    op.kind = BandKind::Pager;
    op.count = count;
    op.current = current;
    draw_band(s, kReadoutX, 0, kBarH - 1, op);
}

namespace {
// The text itself, or its start plus "..." (the cut only; callers say whether the gate counts it).
std::string_view cut_end(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf]) {
    if (text_width(f, text) <= max_px) return text;
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

std::string_view cut_mid(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf]) {
    constexpr int kTail = 4, kDots = 2;
    if (text_width(f, text) <= max_px) return text;
    const int cells = max_px / f.adv;
    if (cells < kTail + kDots + 1) return cut_end(f, text, max_px, buf);
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
    if (head + kDots + tail > kFitBuf) return cut_end(f, text, max_px, buf);   // not for 48 byte buffers
    size_t n = 0;
    for (size_t k = 0; k < head; k++) buf[n++] = text[k];
    for (int k = 0; k < kDots; k++) buf[n++] = '.';
    for (size_t k = 0; k < tail; k++) buf[n++] = text[t + k];
    return {buf, n};
}
}  // namespace

std::string_view fit_text(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf] K2_TRUNC_DEF) {
    const std::string_view out = cut_end(f, text, max_px, buf);
    if (out.data() != text.data()) K2_TRUNC_NOTE(trunc::Kind::Cut, text, max_px);
    return out;
}

std::string_view fit_data(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf] K2_TRUNC_DEF) {
    const std::string_view out = cut_end(f, text, max_px, buf);
    if (out.data() != text.data()) K2_TRUNC_NOTE(trunc::Kind::Data, text, max_px);
    return out;
}

std::string_view fit_text_mid(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf] K2_TRUNC_DEF) {
    const std::string_view out = cut_mid(f, text, max_px, buf);
    if (out.data() != text.data()) K2_TRUNC_NOTE(trunc::Kind::Data, text, max_px);
    return out;
}

void draw_flow_diagram(const Surface &s, int16_t x, int16_t y, int16_t h, const FlowSpec &f) {
    const FlowGeom g = flow_geom(h);
    gfx_fill(s, x, y, kFlowW, h, kSurface);
    draw_port_male(s, static_cast<int16_t>(x + 6), static_cast<int16_t>(y + g.top_port_y), f.top_color, kSurface);
    draw_port_female(s, static_cast<int16_t>(x + 6), static_cast<int16_t>(y + g.bottom_port_y), f.bottom_color);
    const struct { int16_t ry; std::string_view role; uint16_t color, tint; } roles[2] = {
        {g.role_top_y, f.top_role, f.top_color, f.top_tint},
        {g.role_bottom_y, f.bottom_role, f.bottom_color, f.bottom_tint},
    };
    for (const auto &rl : roles) {
        if (rl.role.empty()) continue;
        const int16_t w = static_cast<int16_t>(rl.role.size() * kLabelAdv + 8);
        gfx_fill(s, static_cast<int16_t>(x + 28), static_cast<int16_t>(y + rl.ry), w, kPillH, rl.tint);
        gfx_text(s, kLabelFont, static_cast<int16_t>(x + 32), static_cast<int16_t>(y + rl.ry + 1), rl.role, rl.color, rl.tint);
    }
    if (f.flow == Arrow::Idle) {
        gfx_fill(s, static_cast<int16_t>(x + 14), static_cast<int16_t>(y + g.line_y), 2, g.line_h, kFaint);
    } else {
        const int n = flow_chevrons(h);
        const int span = n * kChevPitch - (kChevPitch - kChevH);
        const int y0 = y + g.line_y + (g.line_h - span) / 2;
        for (int k = 0; k < n; k++)
            draw_chevron(s, static_cast<int16_t>(x + 10), static_cast<int16_t>(y0 + k * kChevPitch), f.flow == Arrow::Up, kFlow);
    }
    if (!f.caption.empty())
        gfx_text(s, kLabelFont, static_cast<int16_t>(x + 30), static_cast<int16_t>(y + g.caption_y), f.caption, f.caption_color, kSurface);
}

void draw_checklist(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t accent,
                    const CheckItem *items, int n) {
    gfx_fill(s, x, y, w, h, kSurface);
    gfx_fill(s, x, static_cast<int16_t>(y + 4), 3, static_cast<int16_t>(h - 8), accent);
    for (int i = 0; i < n; i++) {
        const CheckItem &it = items[i];
        draw_icon(s, static_cast<int16_t>(x + 8), static_cast<int16_t>(y + checklist_icon_y(i)),
                  it.met ? Icon::Check : Icon::Cross, it.met ? kOk : accent, kSurface);
        gfx_text(s, kSmall, static_cast<int16_t>(x + 24), static_cast<int16_t>(y + checklist_text_y(i)), it.text,
                 it.met ? kDim : kText, kSurface);
    }
}

void draw_step_tile(const Surface &s, int16_t x, int16_t y, int number, bool done) {
    const uint16_t c = done ? kOk : kCaution;
    gfx_fill(s, x, y, 22, 22, c);
    const char d[2] = {static_cast<char>('0' + number % 10), 0};
    gfx_text(s, kSmall, static_cast<int16_t>(x + 6), static_cast<int16_t>(y + 2), d, kBg, c);
}

}  // namespace k2
