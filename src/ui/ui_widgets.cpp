// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_widgets.hpp"
#include "k2/ui/ui_types.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/ui_viewport.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace k2 {

namespace {

constexpr int kMaxW = kScreenW;
constexpr int kReadoutX = 140;   // the top bar's right-hand readout area starts here (draw_top_readout)

bool fits(const Surface &s, int x, int y, int w, int h) {
    return x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= s.w && y + h <= s.h;
}

struct BandRow {
    uint16_t pixels[kScreenW];
    int origin, width, y;

    void fill(int x, int w, uint16_t color) {
        const int first = std::max(x - origin, 0);
        const int last = std::min(x + w - origin, width);
        for (int i = first; i < last; i++) pixels[i] = color;
    }

    void text(const Font &font, int x, int top, std::string_view value, uint16_t fg, uint16_t bg) {
        if (y < top || y >= top + font.height || x < origin || x >= origin + width) return;
        uint16_t lut[16];
        make_blend_lut(fg, bg, lut);
        render_row(font, value, y - top, lut, pixels + x - origin, origin + width - x);
    }

    void icon(int x, int top, Icon icon, uint16_t fg, uint16_t bg) {
        if (y < top || y >= top + kIconSize) return;
        const uint16_t bits = kIconBits[static_cast<int>(icon)][y - top];
        for (int c = 0; c < kIconSize; c++)
            fill(x + c, 1, (bits >> (kIconSize - 1 - c)) & 1 ? fg : bg);
    }

    // One row of the physical chevron whose top-left is (x, top); a no-op outside its rows.
    void chevron(int x, int top, bool toward_top, unsigned rotation, uint16_t color) {
        const int r = y - top;
        if (r < 0 || r >= physical_chevron_h(rotation)) return;
        const unsigned bits = physical_chevron_row(toward_top, rotation, r);
        for (int c = 0; c < physical_chevron_w(rotation); c++)
            if (bits >> c & 1) fill(x + c, 1, color);
    }

    void last_pill(std::string_view value) {
        const TextBounds tb = badge_text_bounds(kLabelFont, value);
        const int w = tb.w + 8;
        const int x = 222 - w;
        if (y >= 2 && y < 16) {
            const int inset = round_row_inset(3, 14, y - 2);
            fill(x + inset, w - 2 * inset, kStale);
        }
        const TextOrigin p = center_in_box({static_cast<int16_t>(x), 2, static_cast<int16_t>(w), 14}, tb);
        text(kLabelFont, p.x, p.y, value, kBg, kStale);
    }
};

// The part of a band that goes to the panel, in band pixels from its top-left (columns c0..c1-1, rows r0..r1-1). The rows
// are still composed whole (cheap), only the window is narrowed: a repaint of the cells that changed sends just those.
struct BandClip {
    int c0 = 0, c1 = kScreenW, r0 = 0, r1 = kScreenW;
};

template <class Paint>
void draw_band(const Surface &s, int x, int y, int h, Paint paint, int max_w = kScreenW, BandClip clip = {}) {
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
    {   // a band wholly outside the capture strip has nothing to compose
        uint16_t by = static_cast<uint16_t>(band.y + r0), bh = static_cast<uint16_t>(r1 - r0);
        if (!lcd::clip_to_band(by, bh)) return;
    }
    BandRow row{{}, x, band.w, y};
    lcd::stream_begin(static_cast<uint16_t>(band.x + c0), static_cast<uint16_t>(band.y + r0),
                      static_cast<uint16_t>(band.x + c1 - 1), static_cast<uint16_t>(band.y + r1 - 1));
    for (int r = r0; r < r1; r++) {
        row.y = y + r;
        row.fill(x, band.w, kBar);
        paint(row);
        lcd::stream_pixels({row.pixels + c0, static_cast<std::size_t>(c1 - c0)});
    }
    lcd::stream_end();
}

// Fill row `i` of a rounded rectangle, merging runs of rows that share an inset into one fill.
void span(const Surface &s, int x, int y, int w, int h, int r, uint16_t color, int i0, int i1) {
    int i = i0;
    while (i < i1) {
        const int in = round_row_inset(r, h, i);
        int j = i + 1;
        while (j < i1 && round_row_inset(r, h, j) == in) j++;
        gfx_fill(s, static_cast<int16_t>(x + in), static_cast<int16_t>(y + i), static_cast<int16_t>(w - 2 * in),
                 static_cast<int16_t>(j - i), color);
        i = j;
    }
}

}  // namespace

void fill_round(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int r, uint16_t color) {
    span(s, x, y, w, h, r, color, 0, h);
}

void fill_round_hole(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int r, uint16_t color, int16_t hx, int16_t hy,
                     int16_t hw, int16_t hh) {
    const auto holed = [&](int row) { return y + row >= hy && y + row < hy + hh; };
    for (int i = 0; i < h;) {
        const int in = round_row_inset(r, h, i);
        int j = i + 1;
        while (j < h && round_row_inset(r, h, j) == in && holed(j) == holed(i)) j++;
        const int x0 = x + in, x1 = x + w - in;
        if (!holed(i)) {
            gfx_fill(s, static_cast<int16_t>(x0), static_cast<int16_t>(y + i), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(j - i), color);
        } else {
            const int left = std::min<int>(hx, x1), right = std::max<int>(hx + hw, x0);
            if (left > x0) gfx_fill(s, static_cast<int16_t>(x0), static_cast<int16_t>(y + i), static_cast<int16_t>(left - x0), static_cast<int16_t>(j - i), color);
            if (right < x1) gfx_fill(s, static_cast<int16_t>(right), static_cast<int16_t>(y + i), static_cast<int16_t>(x1 - right), static_cast<int16_t>(j - i), color);
        }
        i = j;
    }
}

void frame_round(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int r, int t, uint16_t color) {
    if (w <= 2 * t || h <= 2 * t) { fill_round(s, x, y, w, h, r, color); return; }
    const int ro = (r * 2 > h) ? h / 2 : r;
    const int ri = ro > t ? ro - t : 0;
    const int ih = h - 2 * t;
    for (int i = 0; i < h; i++) {
        const int a = round_row_inset(ro, h, i);
        if (i < t || i >= h - t) {
            gfx_fill(s, static_cast<int16_t>(x + a), static_cast<int16_t>(y + i), static_cast<int16_t>(w - 2 * a), 1, color);
            continue;
        }
        const int b = t + round_row_inset(ri, ih, i - t);   // hole starts here
        gfx_fill(s, static_cast<int16_t>(x + a), static_cast<int16_t>(y + i), static_cast<int16_t>(b - a), 1, color);
        gfx_fill(s, static_cast<int16_t>(x + w - b), static_cast<int16_t>(y + i), static_cast<int16_t>(b - a), 1, color);
    }
}

void draw_bar_label(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int fill_px, uint16_t track,
                    uint16_t fill, std::string_view text, uint16_t fg) {
    fill_round(s, x, y, w, h, 3, track);
    if (fill_px > 0) fill_round(s, x, y, static_cast<int16_t>(fill_px), h, fill_px < 6 ? fill_px / 2 : 3, fill);
    const int16_t ty = static_cast<int16_t>(y + (h - kLabelFont.height) / 2);
    // Whole label on the track, then the part over the fill again through a surface clipped to it.
    gfx_text(s, kLabelFont, static_cast<int16_t>(x + 4), ty, text, fg, track);
    if (fill_px > 4) {
        K2_TRUNC_PAUSE;   // clipped to the fill on purpose: the whole label was drawn (and checked) on the track above
        const Surface clip{static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y), static_cast<int16_t>(fill_px), h};
        gfx_text(clip, kLabelFont, 4, static_cast<int16_t>(ty - y), text, fg, fill);
    }
}

void draw_icon(const Surface &s, int16_t x, int16_t y, Icon icon, uint16_t fg, uint16_t bg) {
    if (!fits(s, x, y, kIconSize, kIconSize)) return;
    const uint16_t *rows = kIconBits[static_cast<int>(icon)];
    uint16_t line[kIconSize];
    lcd::stream_begin(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y),
                      static_cast<uint16_t>(s.x + x + kIconSize - 1), static_cast<uint16_t>(s.y + y + kIconSize - 1));
    for (int r = 0; r < kIconSize; r++) {
        for (int c = 0; c < kIconSize; c++) line[c] = (rows[r] >> (kIconSize - 1 - c)) & 1 ? fg : bg;
        lcd::stream_pixels({line, static_cast<std::size_t>(kIconSize)});
    }
    lcd::stream_end();
}

namespace {
// Art rows, '#' = ink; converted at compile time so the flash holds only the 14 x 16-bit rows.
constexpr uint16_t art_row(const char *r) {
    uint16_t v = 0;
    for (int i = 0; i < 14; i++) v = static_cast<uint16_t>((v << 1) | (r[i] == '#'));
    return v;
}
template <size_t N>
constexpr std::array<uint16_t, 14> art(const char *const (&a)[N]) {
    static_assert(N == 14);
    std::array<uint16_t, 14> o{};
    for (size_t i = 0; i < N; i++) o[i] = art_row(a[i]);
    return o;
}
constexpr const char *kChargerArt[14] = {
    "...##....##...", "...##....##...", "...##....##...", "..##########..",
    ".############.", ".######..####.", ".#####..#####.", ".####....####.",
    ".####..######.", ".###..#######.", ".############.", "..##########..",
    "......##......", "......##......"};
constexpr const char *kPlugArt[14] = {
    "....######....", "...########...", "...##....##...", "...##....##...",
    "...########...", "...########...", "..##########..", "..##########..",
    "..##########..", "..##########..", "....######....", ".....####.....",
    "......##......", "......##......"};
}  // namespace

void draw_icon14(const Surface &s, int16_t x, int16_t y, Icon14 icon, uint16_t fg, uint16_t bg) {
    if (!fits(s, x, y, 14, 14)) return;
    static constexpr std::array<uint16_t, 14> kBits[2] = {art(kChargerArt), art(kPlugArt)};
    const auto &rows = kBits[static_cast<int>(icon)];
    uint16_t line[14];
    lcd::stream_begin(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y),
                      static_cast<uint16_t>(s.x + x + 13), static_cast<uint16_t>(s.y + y + 13));
    for (int r = 0; r < 14; r++) {
        for (int c = 0; c < 14; c++) line[c] = (rows[r] >> (13 - c)) & 1 ? fg : bg;
        lcd::stream_pixels({line, 14});
    }
    lcd::stream_end();
}

void draw_mark(const Surface &s, int16_t x, int16_t y, int size, bool check, uint16_t fg, uint16_t bg) {
    if (size > 16 || !fits(s, x, y, size, size)) return;
    struct Seg { float ax, ay, bx, by; };
    const float n = static_cast<float>(size);
    const Seg check_s[2] = {{0.08f * n, 0.55f * n, 0.38f * n, 0.88f * n}, {0.38f * n, 0.88f * n, 0.95f * n, 0.12f * n}};
    const Seg cross_s[2] = {{0.12f * n, 0.12f * n, 0.88f * n, 0.88f * n}, {0.88f * n, 0.12f * n, 0.12f * n, 0.88f * n}};
    const Seg *seg = check ? check_s : cross_s;
    const float half = 0.5f + (n >= 10 ? 0.45f : 0.35f);   // stroke half width + half a pixel of edge
    uint16_t lut[16], line[16];
    make_blend_lut(fg, bg, lut);
    lcd::stream_begin(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y),
                      static_cast<uint16_t>(s.x + x + size - 1), static_cast<uint16_t>(s.y + y + size - 1));
    for (int r = 0; r < size; r++) {
        for (int c = 0; c < size; c++) {
            const float px = c + 0.5f, py = r + 0.5f;
            float best = 1e9f;
            for (int k = 0; k < 2; k++) {
                const float dx = seg[k].bx - seg[k].ax, dy = seg[k].by - seg[k].ay;
                float t = ((px - seg[k].ax) * dx + (py - seg[k].ay) * dy) / (dx * dx + dy * dy);
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                const float ex = px - (seg[k].ax + t * dx), ey = py - (seg[k].ay + t * dy);
                const float d2 = ex * ex + ey * ey;
                if (d2 < best) best = d2;
            }
            const float g = best > 0.0f ? std::sqrt(best) : 0.0f;
            const float a = half - g;
            const int q = a <= 0 ? 0 : a >= 1 ? 15 : static_cast<int>(a * 15.0f + 0.5f);
            line[c] = lut[q];
        }
        lcd::stream_pixels({line, static_cast<std::size_t>(size)});
    }
    lcd::stream_end();
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

int draw_pill(const Surface &s, int16_t x, int16_t y, std::string_view text, uint16_t color, uint16_t tint,
              bool solid) {
    const TextBounds tb = badge_text_bounds(kLabelFont, text);
    const int w = pill_width(static_cast<int>(text.size()));
    const uint16_t fill = solid ? color : tint;
    const uint16_t fg = solid ? kBg : color;
    // The pill's ground goes round the text's box, which the glyphs paint themselves: every pixel is streamed once (a scaled
    // page keeps the whole fill, there a glyph box does not match the logical hole, and centers the native glyphs in it).
    if (ContentViewport::scaled()) {
        fill_round(s, x, y, static_cast<int16_t>(w), kPillH, 3, fill);
        const Rect mb = ContentViewport::mapped_rect(
            {static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y), static_cast<int16_t>(w), kPillH});
        const TextOrigin p = center_in_box(mb, tb);
        ContentViewport::Pause unscaled;
        gfx_text({0, 0, kScreenW, kScreenW}, kLabelFont, p.x, p.y, text, fg, fill);
    } else {
        const TextOrigin p = center_in_box({x, y, static_cast<int16_t>(w), kPillH}, tb);
        fill_round_hole(s, x, y, static_cast<int16_t>(w), kPillH, 3, fill, p.x, p.y,
                        static_cast<int16_t>(text_width(kLabelFont, text)), static_cast<int16_t>(kLabelFont.height));
        gfx_text(s, kLabelFont, p.x, p.y, text, fg, fill);
    }
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

void draw_sparkline(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *samples,
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

void draw_range_trace(const Surface &s, int16_t x, int16_t y, int cols, int xs, int16_t h, const uint8_t *lo,
                      const uint8_t *hi, uint8_t mark_a, uint16_t ca, uint8_t mark_b, uint16_t cb, uint16_t color,
                      uint16_t bg) {
    const int w = cols * xs;
    if (cols <= 0 || xs <= 0 || h < 4 || !fits(s, x, y, w, h) || w > kMaxW) return;
    uint16_t line[kMaxW];
    lcd::stream_begin(static_cast<uint16_t>(s.x + x), static_cast<uint16_t>(s.y + y),
                      static_cast<uint16_t>(s.x + x + w - 1), static_cast<uint16_t>(s.y + y + h - 1));
    for (int r = 0; r < h; r++) {
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

void draw_footer(const Surface &s, FooterTone tone, std::string_view text K2_TRUNC_DEF) {
    const int16_t fy = static_cast<int16_t>(s.h - 18);   // the footer hugs the content bottom (kFooterY on a full-height surface)
    const FooterStyle st = footer_style(tone);
    char buf[kFitBuf];                                  // a footer that would run off the surface ends in "..."
    const auto fitted = fit_text(kSmall, text, std::min<int>(kFooterTextPx, s.w - kFooterTextX), buf K2_TRUNC_ARG);
    draw_band(s, 0, fy, s.h - fy, [&](BandRow &row) {
        // the whole band row: in landscape it is 240 panel px, not the 224 logical ones
        row.fill(0, kScreenW, row.y == fy && st.rule ? st.rule : st.fill);
        if (st.bar) row.fill(0, kFooterBarW, st.bar);
        row.text(kSmall, kFooterTextX, fy + 1, fitted, st.text, st.fill);
    });
}

void draw_footer_cached(const Surface &s, FooterTone tone, std::string_view text, Elem &e K2_TRUNC_DEF) {
    uint32_t sg = mix_u32(mix_init(), static_cast<uint32_t>(tone) * 31u + 1u);
    sg = mix(sg, text);
    e.paint(sg, [&] { draw_footer(s, tone, text K2_TRUNC_ARG); });
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
void draw_status_left(const Surface &s, bool has_icon, Icon icon, std::string_view title, int16_t right,
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
    draw_band(s, 0, 0, kBarH - 1, [&](BandRow &row) {   // the band stops above the rule row, which is painted once below
        if (has_icon && fits(s, kMastIconX, kBarTextY, kIconSize, kIconSize)) row.icon(kMastIconX, kBarTextY, icon, kInk, kBar);
        row.text(tf, x, ty, fitted, kInk, kBar);
        if (!badge.empty()) {
            row.fill(sep, 1, kInk3);
            row.text(kLabelFont, sep + 5, kBarTextY, badge, kInk2, kBar);
        }
    }, readout_follows ? kReadoutX : kScreenW);
    gfx_fill(s, 0, static_cast<int16_t>(kBarH - 1), s.w, 1, kAccent);   // the masthead rule
}

// The call repaints the band rows 0..kBarH-2 over x 140..223; the hairline row (kBarH-1) is never
// touched here: it belongs to title_bar()'s full-band repaint, and touching it here would make the
// capture passes (which redraw once per strip) leave different pixels behind than a plain tick.
namespace {
constexpr int kSlotPx = ReadoutShown::kCells * kLabelAdv;
std::string_view slot_text(std::string_view value) { return text_width(kLabelFont, value) > kSlotPx ? "OVR" : value; }

void readout_row(BandRow &row, StatusReadout mode, std::string_view v, std::string_view a, Arrow flow, uint8_t rotation,
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
}  // namespace

void draw_top_readout(const Surface &s, StatusReadout mode, std::string_view volts, std::string_view amps,
                      Arrow flow, uint8_t rotation, std::string_view last_text) {
    const std::string_view v = slot_text(volts), a = slot_text(amps);
    if (mode == StatusReadout::Live) {
        if (v.data() != volts.data()) K2_TRUNC_NOTE_HERE(trunc::Kind::Data, volts, kSlotPx);   // a measurement, not copy
        if (a.data() != amps.data()) K2_TRUNC_NOTE_HERE(trunc::Kind::Data, amps, kSlotPx);
    }
    draw_band(s, kReadoutX, 0, kBarH - 1, [&](BandRow &row) { readout_row(row, mode, v, a, flow, rotation, last_text); });
}

void draw_top_readout(const Surface &s, StatusReadout mode, std::string_view volts, std::string_view amps, Arrow flow,
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
        for (int i = 0; i < d.n; i++)
            draw_band(s, kReadoutX, 0, kBarH - 1,
                      [&](BandRow &row) { readout_row(row, mode, v, a, flow, rotation, {}); }, kScreenW,
                      BandClip{d.span[i].x0 - kReadoutX, d.span[i].x1 - kReadoutX, kBarTextY,
                               kBarTextY + static_cast<int>(kLabelFont.height)});
    }
    now.valid = cells;
    shown = now;
}

void draw_top_pager(const Surface &s, int count, int current) {
    draw_band(s, kReadoutX, 0, kBarH - 1, [&](BandRow &row) {
        const int r = row.y - kPagerY;
        if (r < 0 || r >= kPagerBox) return;
        const bool edge = r == 0 || r == kPagerBox - 1;
        for (int i = 0; i < count; i++) {
            const int x = kPagerRight - (count - i) * kPagerPitch + (kPagerPitch - kPagerBox);
            if (i == current || edge) row.fill(x, kPagerBox, kInk);
            else { row.fill(x, 1, kInk); row.fill(x + kPagerBox - 1, 1, kInk); }
        }
    });
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

void draw_last_pill(const Surface &s, std::string_view text) {
    const TextBounds tb = badge_text_bounds(kLabelFont, text);
    const int16_t w = static_cast<int16_t>(tb.w + 8);
    const int16_t x = static_cast<int16_t>(222 - w);
    fill_round(s, x, 2, w, 14, 3, kStale);
    const TextOrigin p = center_in_box({x, 2, w, 14}, tb);
    gfx_text(s, kLabelFont, p.x, p.y, text, kBg, kStale);
}

void draw_party_glyph(const Surface &s, int16_t x, int16_t y, int party, uint16_t color) {
    static constexpr uint8_t kBits[kPartyGlyphs][kPartySize] = {
        {0x06, 0x0C, 0x18, 0x3E, 0x0C, 0x18, 0x30, 0x20},   // bolt
        {0x3C, 0x42, 0x85, 0x89, 0x91, 0xA1, 0x42, 0x3C},   // ring with a slash
        {0x3C, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x3C},   // upright rectangle
    };
    if (party < 0 || party >= kPartyGlyphs) return;
    for (int r = 0; r < kPartySize; r++) {
        const uint8_t bits = kBits[party][r];
        for (int c = 0; c < kPartySize;) {
            if (!(bits & (0x80 >> c))) { c++; continue; }
            int run = 1;
            while (c + run < kPartySize && (bits & (0x80 >> (c + run)))) run++;
            gfx_fill(s, static_cast<int16_t>(x + c), static_cast<int16_t>(y + r), static_cast<int16_t>(run), 1, color);
            c += run;
        }
    }
}

void draw_flow_diagram(const Surface &s, int16_t x, int16_t y, int16_t h, const FlowSpec &f) {
    const FlowGeom g = flow_geom(h);
    fill_round(s, x, y, kFlowW, h, 6, kSurface);
    draw_port_male(s, static_cast<int16_t>(x + 6), static_cast<int16_t>(y + g.top_port_y), f.top_color, kSurface);
    draw_port_female(s, static_cast<int16_t>(x + 6), static_cast<int16_t>(y + g.bottom_port_y), f.bottom_color);
    if (!f.top_role.empty()) {
        const int16_t w = static_cast<int16_t>(f.top_role.size() * kLabelAdv + 8);
        fill_round(s, static_cast<int16_t>(x + 28), static_cast<int16_t>(y + g.role_top_y), w, kPillH, 3, f.top_tint);
        gfx_text(s, kLabelFont, static_cast<int16_t>(x + 32), static_cast<int16_t>(y + g.role_top_y + 1), f.top_role, f.top_color, f.top_tint);
    }
    if (!f.bottom_role.empty()) {
        const int16_t w = static_cast<int16_t>(f.bottom_role.size() * kLabelAdv + 8);
        fill_round(s, static_cast<int16_t>(x + 28), static_cast<int16_t>(y + g.role_bottom_y), w, kPillH, 3, f.bottom_tint);
        gfx_text(s, kLabelFont, static_cast<int16_t>(x + 32), static_cast<int16_t>(y + g.role_bottom_y + 1), f.bottom_role, f.bottom_color, f.bottom_tint);
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
    fill_round(s, x, y, w, h, 5, kSurface);
    fill_round(s, x, static_cast<int16_t>(y + 4), 3, static_cast<int16_t>(h - 8), 1, accent);
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
    fill_round(s, x, y, 22, 22, 5, c);
    const char d[2] = {static_cast<char>('0' + number % 10), 0};
    gfx_text(s, kSmall, static_cast<int16_t>(x + 6), static_cast<int16_t>(y + 2), d, kBg, c);
}

}  // namespace k2
