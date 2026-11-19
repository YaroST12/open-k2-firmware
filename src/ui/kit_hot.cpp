// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Hot-path UI kit painters placed in the zero-wait flash window (K2_HOT).
#include "k2/ui/kit.hpp"
#include "kit_internal.hpp"
#include "k2/core/hot.h"
#include "k2/drivers/lcd.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui_viewport.hpp"

namespace k2::kit {

namespace {
constexpr int kTailMaxPx = 112;
constexpr int16_t kChevW = 6;
constexpr int kMaxHoles = 6;
}  // namespace

K2_HOT void fill_except(const Surface &s, Box r, const Box *holes, int n, uint16_t color) {
    if (r.w <= 0 || r.h <= 0) return;
    if (!ContentViewport::stretched()) {
        if (s.y + r.y >= 0) {
            uint16_t by = static_cast<uint16_t>(s.y + r.y), bh = static_cast<uint16_t>(r.h);
            if (!lcd::clip_to_band(by, bh)) return;
            r.y = static_cast<int16_t>(by - s.y);
            r.h = static_cast<int16_t>(bh);
        }
    } else {
        uint16_t b0 = 0, b1 = 0;
        if (ContentViewport::panel_band(b0, b1)) {
            const Rect p = ContentViewport::mapped_rect(
                {static_cast<int16_t>(s.x + r.x), static_cast<int16_t>(s.y + r.y), r.w, r.h});
            if (p.h <= 0 || p.y > b1 || p.y + p.h <= b0) return;
        }
    }
    const bool scaled = ContentViewport::scaled();
    Box h[kMaxHoles];
    int16_t ys[2 * kMaxHoles + 2];
    int m = 0, k = 0;
    ys[k++] = r.y;
    ys[k++] = static_cast<int16_t>(r.y + r.h);
    for (int i = 0; i < n && m < kMaxHoles; i++) {
        Box hb = holes[i];
        if (scaled) {
            auto mapped = [&](int w, int hh) {
                return ContentViewport::mapped_rect({static_cast<int16_t>(s.x + hb.x), static_cast<int16_t>(s.y + hb.y),
                                                     static_cast<int16_t>(w), static_cast<int16_t>(hh)});
            };
            int w = hb.w, hh = hb.h;
            while (w > 0 && mapped(w, 1).w > hb.w) w--;
            while (hh > 0 && mapped(1, hh).h > hb.h) hh--;
            hb.w = static_cast<int16_t>(w);
            hb.h = static_cast<int16_t>(hh);
        }
        const int x0 = hb.x > r.x ? hb.x : r.x, y0 = hb.y > r.y ? hb.y : r.y;
        const int x1 = hb.x + hb.w < r.x + r.w ? hb.x + hb.w : r.x + r.w;
        const int y1 = hb.y + hb.h < r.y + r.h ? hb.y + hb.h : r.y + r.h;
        if (x1 <= x0 || y1 <= y0) continue;
        h[m++] = {static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
        ys[k++] = static_cast<int16_t>(y0);
        ys[k++] = static_cast<int16_t>(y1);
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && ys[j] < ys[j - 1]; j--) { const int16_t t = ys[j]; ys[j] = ys[j - 1]; ys[j - 1] = t; }
    for (int b = 0; b + 1 < k; b++) {
        const int16_t y0 = ys[b], y1 = ys[b + 1];
        if (y1 <= y0) continue;
        int16_t xs[kMaxHoles][2];
        int c = 0;
        for (int i = 0; i < m; i++) {
            if (h[i].y > y0 || y0 >= h[i].y + h[i].h) continue;
            int j = c++;
            for (; j > 0 && xs[j - 1][0] > h[i].x; j--) { xs[j][0] = xs[j - 1][0]; xs[j][1] = xs[j - 1][1]; }
            xs[j][0] = h[i].x;
            xs[j][1] = static_cast<int16_t>(h[i].x + h[i].w);
        }
        int16_t at = r.x;
        for (int i = 0; i < c; i++) {
            if (xs[i][0] > at) gfx_fill(s, at, y0, static_cast<int16_t>(xs[i][0] - at), static_cast<int16_t>(y1 - y0), color);
            if (xs[i][1] > at) at = xs[i][1];
        }
        if (r.x + r.w > at) gfx_fill(s, at, y0, static_cast<int16_t>(r.x + r.w - at), static_cast<int16_t>(y1 - y0), color);
    }
}

K2_HOT void draw_key_slot(const Surface &s, bool column, TextDir dir, unsigned slot, const SlotLabel &lab, bool is_ok, bool down) {
    if (s.w <= 0 || s.h <= 0) return;
    {
        uint16_t by = static_cast<uint16_t>(s.y), bh = static_cast<uint16_t>(s.h);
        if (!lcd::clip_to_band(by, bh)) return;
    }
    char text[24];
    const int along = column ? s.h : s.w;
    const LabelZone zone = label_zone(along, slot);
    const std::string_view shrt = lab.shrt;
    const bool armed = is_ok && shrt == "Confirm";
    const int tri = !down && (shrt == kUp || shrt == kDn) ? (shrt == kUp ? 1 : -1) : 0;
    const unsigned chars = tri ? 0 : label_text(text, lab, zone.max_chars);
    const std::string_view label(text, chars);
    const bool cap = is_ok && chars > 0 && !down;
    const uint16_t ground = down ? kCap : kBg2;
    const uint16_t cap_fill = armed ? kCaution : kCap;
    const uint16_t bg = cap ? cap_fill : ground;
    const uint16_t fg = down ? kOnCap : cap ? (armed ? kSpotInk : kOnCap)
                      : lab.dim ? kInk3 : shrt == "Stop" ? kError : kInk2;
    Box holes[2];
    int nh = 0;
    int16_t tx = 0, ty = 0;
    const int16_t fh = static_cast<int16_t>(kLabelFont.height);
    const int16_t tw = static_cast<int16_t>(text_width(kLabelFont, label));
    if (chars > 0) {
        const int16_t a0 = static_cast<int16_t>(label_origin(zone, chars));
        tx = column ? static_cast<int16_t>((s.w - fh) / 2) : a0;
        ty = column ? a0 : static_cast<int16_t>((s.h - fh) / 2);
        if (cap) {
            const int16_t c0 = static_cast<int16_t>(zone.start - 2), cl = static_cast<int16_t>(zone.len + 4);
            const int16_t across = column ? s.w : s.h, c1 = static_cast<int16_t>((across - 14) / 2);
            holes[nh++] = column ? Box{c1, c0, 14, cl} : Box{c0, c1, cl, 14};
        } else {
            holes[nh++] = {tx, ty, column ? fh : tw, column ? tw : fh};
        }
    }
    Box tri_box{};
    if (tri) {
        const int16_t cx = static_cast<int16_t>(zone.start + zone.len / 2);
        tri_box = column ? Box{static_cast<int16_t>((s.w - 5) / 2), static_cast<int16_t>(cx - 4), 5, 9}
                         : Box{static_cast<int16_t>(cx - 4), static_cast<int16_t>((s.h - 5) / 2), 9, 5};
        holes[nh++] = tri_box;
    }
    fill_except(s, {0, 0, s.w, s.h}, holes, nh, ground);
    if (cap) {
        const Box t{tx, ty, column ? fh : tw, column ? tw : fh};
        fill_except(s, holes[0], &t, 1, cap_fill);
    }
    if (chars > 0) {
        if (column) gfx_text_rot(s, kLabelFont, tx, ty, label, fg, bg, dir);
        else gfx_text(s, kLabelFont, tx, ty, label, fg, bg);
    }
    if (!tri) return;
    const bool up = tri > 0;
    const int toward = !column ? (up ? 0 : 1) : (dir == TextDir::Ccw) == up ? 2 : 3;
    const uint16_t ink = lab.dim ? kInk3 : kInk2;
    for (int i = 0; i < 5; i++) {
        const int k = toward == 0 || toward == 2 ? i : 4 - i;
        const int16_t lo = static_cast<int16_t>(4 - k), span = static_cast<int16_t>(2 * k + 1);
        if (!column) {
            const int16_t ly = static_cast<int16_t>(tri_box.y + i);
            gfx_fill(s, tri_box.x, ly, lo, 1, ground);
            gfx_fill(s, static_cast<int16_t>(tri_box.x + lo), ly, span, 1, ink);
            gfx_fill(s, static_cast<int16_t>(tri_box.x + lo + span), ly, lo, 1, ground);
        } else {
            const int16_t lx = static_cast<int16_t>(tri_box.x + i);
            gfx_fill(s, lx, tri_box.y, 1, lo, ground);
            gfx_fill(s, lx, static_cast<int16_t>(tri_box.y + lo), 1, span, ink);
            gfx_fill(s, lx, static_cast<int16_t>(tri_box.y + lo + span), 1, lo, ground);
        }
    }
}

K2_HOT void paint_texts(const Surface &s, Box ground, const TextRun *runs, int n, uint16_t bg) {
    Box holes[kMaxHoles];
    int m = 0;
    for (int i = 0; i < n && m < kMaxHoles; i++)
        holes[m++] = {runs[i].x, runs[i].y, static_cast<int16_t>(text_width(*runs[i].font, runs[i].text)), static_cast<int16_t>(runs[i].font->height)};
    fill_except(s, ground, holes, m, bg);
    for (int i = 0; i < n; i++) gfx_text(s, *runs[i].font, runs[i].x, runs[i].y, runs[i].text, runs[i].ink, bg);
}

namespace detail {
K2_HOT LabelLayout label_layout(int16_t x, int16_t y, int16_t w, int16_t h, std::string_view label, char (&buf)[kFitBuf]) {
    const Font &f = h >= 22 ? kSmall : kLabelFont;
    const std::string_view t = fit_text(f, label, w, buf);
    const int16_t tw = static_cast<int16_t>(text_width(f, t));
    return {&f, t, {static_cast<int16_t>(x + (w - tw) / 2), static_cast<int16_t>(y + (h - f.height) / 2), tw, f.height}};
}

K2_HOT Box strike_line(const Font &f, int16_t tx, int16_t ty, int16_t tw, int16_t cx, int16_t cw) {
    const Glyph *gx = find_glyph(f, 'x');
    const int xh = gx ? gx->top : f.ascent * 2 / 3;
    int16_t x0 = static_cast<int16_t>(tx - 1), x1 = static_cast<int16_t>(tx + tw + 1);
    if (x0 < cx) x0 = cx;
    if (x1 > cx + cw) x1 = static_cast<int16_t>(cx + cw);
    return {x0, static_cast<int16_t>(ty + f.ascent - 1 - xh / 2), static_cast<int16_t>(x1 > x0 ? x1 - x0 : 0), 1};
}
}  // namespace detail

K2_HOT void draw_top_bar(const Surface &s, std::string_view title, int step, std::string_view badge, bool readout_follows, int segs) {
    char buf[40];
    size_t n = 0;
    if (step >= 1 && step <= segs) {
        buf[n++] = static_cast<char>('0' + step);
        buf[n++] = '/';
        buf[n++] = static_cast<char>('0' + segs);
        buf[n++] = ' ';
    }
    for (size_t i = 0; i < title.size() && n < sizeof(buf); i++) buf[n++] = title[i];
    draw_status_left(s, false, Icon::Bolt, std::string_view(buf, n), 132, badge, readout_follows);
    if (step > 0) draw_progress_rule(s, step, segs);
    else gfx_fill(s, 0, kProgY + 1, s.w, kProgH - 1, kBg);
}

K2_HOT uint32_t RowList::row_sig_cb(void *ctx, int index) {
    auto *self = static_cast<RowList *>(ctx);
    RowSpec r;
    const bool focused = index == self->sel_;
    self->fn_(self->ctx_, index, focused, r);
    uint32_t h = mix_init();
    h = mix_u32(h, static_cast<uint32_t>(r.marker) | (static_cast<uint32_t>(r.state) << 4) |
                       (static_cast<uint32_t>(r.icon) << 8) | (static_cast<uint32_t>(r.chevron) << 16) |
                       (static_cast<uint32_t>(focused) << 17));
    h = mix_u32(h, r.marker_color | (static_cast<uint32_t>(r.text_color) << 16));
    h = mix_u32(h, r.tail_color | (static_cast<uint32_t>(r.tail_tint) << 16));
    h = mix(h, r.text);
    h = mix(h, r.tail);
    h = mix(h, r.col2);
    h = mix(h, r.note);
    return h;
}

K2_HOT void RowList::draw_row_cb(void *ctx, int index, const Rect &r, bool hi, Field &) {
    auto *self = static_cast<RowList *>(ctx);
    const Surface &s = self->surf_;
    RowSpec sp;
    self->fn_(self->ctx_, index, hi, sp);
    const int16_t y = r.y;
    const uint16_t bg = hi ? kFocusFill : kBg;
    const bool bar = hi && kFocusBar != 0;
    if (hi && !bar) {
        sp.text_color = kOnFocus;
        if (!sp.tail_tint) sp.tail_color = kOnFocusDim;
        if (sp.marker != Marker::Verdict) sp.marker_color = kOnFocus;
    } else if (hi && sp.marker == Marker::Icon && sp.marker_color == 0) {
        sp.marker_color = kAccent;
    }

    const int16_t ty = static_cast<int16_t>(y + row_text_y(kRowH, kSmall.height));
    std::string_view tail = sp.tail;
    if (sp.tail_yields && !tail.empty() && !sp.chevron) {
        const int tw = sp.tail_tint ? pill_width(text_cells(tail)) : text_width(kSmall, tail);
        const int col2w = sp.col2.empty() ? 0 : text_width(kSmall, sp.col2) + kTailGap;
        if (kTextX + text_width(kSmall, sp.text) + col2w + kTailGap + tw > kTailX) tail = {};
    }
    int right = sp.chevron || !tail.empty() ? kTailX : s.w - ListView::kBarW;
    char buf[kFitBuf], tbuf[kFitBuf], cbuf[kFitBuf], nbuf[kFitBuf];
    Box holes[5];
    int nh = 0;
    const auto hole = [&](int16_t hx, int16_t hy, int w, int h) {
        holes[nh++] = {hx, hy, static_cast<int16_t>(w), static_cast<int16_t>(h)};
    };
    if (sp.marker != Marker::None) hole(kMarkX, static_cast<int16_t>(y + 7), kMarkSize, kMarkSize);
    std::string_view tail_v;
    int pill_w = 0;
    if (sp.chevron) {
        right -= kChevW + kTailGap;
    } else if (!tail.empty() && sp.tail_tint) {
        tail_v = fit_text(kLabelFont, tail, kTailMaxPx - 8, tbuf);
        pill_w = pill_width(static_cast<int>(tail_v.size()));
        right -= pill_w + kTailGap;
    } else if (!tail.empty()) {
        tail_v = fit_text(kSmall, tail, kTailMaxPx, tbuf);
        const int vw = text_width(kSmall, tail_v);
        hole(static_cast<int16_t>(kTailX - vw), ty, vw, kSmall.height);
        right -= vw + kTailGap;
    }
    std::string_view col2_v;
    if (!sp.col2.empty()) {
        col2_v = fit_text(kSmall, sp.col2, right - kCol2X, cbuf);
        hole(kCol2X, ty, text_width(kSmall, col2_v), kSmall.height);
        right = kCol2X - 4;
    }
    const std::string_view text_v =
        sp.data ? fit_data(kSmall, sp.text, right - kTextX, buf) : fit_text(kSmall, sp.text, right - kTextX, buf);
    hole(kTextX, ty, text_width(kSmall, text_v), kSmall.height);
    std::string_view note_v;
    if (hi && r.h > kRowH && !sp.note.empty()) {
        note_v = fit_text(kLabelFont, sp.note, kNoteMaxPx, nbuf);
        hole(kTextX, static_cast<int16_t>(y + kNoteDy), text_width(kLabelFont, note_v), kLabelFont.height);
    }

    const int16_t gx = bar ? 3 : 0;
    if (bar) gfx_fill(s, 0, static_cast<int16_t>(y + 1), 3, static_cast<int16_t>(r.h - 1), kFocusBar);
    fill_except(s, {gx, static_cast<int16_t>(y + 1), static_cast<int16_t>(self->list_.row_width() - gx), static_cast<int16_t>(r.h - 1)},
                holes, nh, bg);

    draw_marker(s, kMarkX, static_cast<int16_t>(y + 7), sp, bg);
    if (sp.chevron) {
        draw_drill(s, kTailX, static_cast<int16_t>(y + 1 + (kRowH - 1) / 2), sp.tail_color);
    } else if (pill_w) {
        const int16_t px0 = static_cast<int16_t>(kTailX - pill_w);
        gfx_fill(s, px0, static_cast<int16_t>(y + 1 + (kRowH - 1 - kPillH) / 2), static_cast<int16_t>(pill_w), kPillH, sp.tail_tint);
        gfx_text(s, kLabelFont, static_cast<int16_t>(px0 + 4), static_cast<int16_t>(y + 1 + (kRowH - 1 - kPillH) / 2 + 1), tail_v,
                 sp.tail_color, sp.tail_tint);
    } else if (!tail_v.empty()) {
        gfx_text(s, kSmall, static_cast<int16_t>(kTailX - text_width(kSmall, tail_v)), ty, tail_v, sp.tail_color, bg);
    }
    const uint16_t dim = hi ? kOnFocusDim : kDim;
    if (!col2_v.empty()) gfx_text(s, kSmall, kCol2X, ty, col2_v, dim, bg);
    gfx_text(s, kSmall, kTextX, ty, text_v, sp.text_color, bg);
    if (!note_v.empty()) gfx_text(s, kLabelFont, kTextX, static_cast<int16_t>(y + kNoteDy), note_v, dim, bg);
}

K2_HOT void RowList::draw(uint32_t now_ms, PaintBudget *bg) {
    if (!fn_) { if (bg) bg->defer(); return; }
    list_.set_count(count_, false);
    list_.set_highlight(sel_);
    const ListState &st = list_.state();
    if (sel_ >= 0 && sel_ < count_) {
        int off = st.offset;
        if (sel_ < off) off = sel_;
        else if (sel_ >= off + st.visible) off = sel_ - st.visible + 1;
        list_.set_offset(off);
    }
    int16_t want = 0;
    if (sel_ >= 0 && sel_ < count_) {
        RowSpec probe;
        fn_(ctx_, sel_, true, probe);
        if (!probe.note.empty()) want = kFocusExtra;
    }
    if (want != extra_) {
        const bool known = extra_ >= 0;
        if (want < kFocusExtra && bg && !bg->admit(static_cast<int>(surf_.w) * (kFocusExtra - want), true)) return;
        list_.set_focus_extra(want);
        if (known) list_.invalidate();
        if (want < kFocusExtra)
            gfx_fill(surf_, 0, static_cast<int16_t>(y_ + st.visible * kRowH + want), surf_.w,
                     static_cast<int16_t>(kFocusExtra - want), kBg);
        extra_ = want;
    }
    list_.draw(now_ms, bg);
}

K2_HOT void kv_static_row(const Surface &s, const KvLayout &l, const char *label, int i, bool trailing_line) {
    const Font &f = detail::font_or_small(l.label_font);
    const int16_t y = kv_row_y(l, i);
    gfx_fill(s, 0, y, s.w, 1, kBg);
    const int16_t ty = static_cast<int16_t>(y + row_text_y(l.pitch, f.height));
    gfx_text(s, f, l.label_x, ty, label, kInk2, kBg);
    draw_leader(s, static_cast<int16_t>(l.label_x + text_width(f, label) + 5), static_cast<int16_t>(l.value_x - kLeaderGap),
                static_cast<int16_t>(ty + f.ascent - 2));
    if (trailing_line) gfx_fill(s, 0, kv_row_y(l, i + 1), s.w, 1, kBg);
}

K2_HOT void draw_dashed(const Surface &s, int16_t x0, int16_t x1, int16_t y, uint16_t ink, uint16_t bg) {
    for (int16_t x = x0; x < x1; x = static_cast<int16_t>(x + kDashOn + kDashOff)) {
        const int16_t on = static_cast<int16_t>(std::min<int>(kDashOn, x1 - x));
        gfx_fill(s, x, y, on, 1, ink);
        const int16_t off = static_cast<int16_t>(std::min<int>(kDashOff, x1 - x - on));
        if (off > 0) gfx_fill(s, static_cast<int16_t>(x + on), y, off, 1, bg);
    }
}

K2_HOT void kv_cells(const Surface &s, const KvLayout &l, Field *cells, int n, const uint8_t *chars) {
    const Font &f = detail::font_or_small(l.value_font);
    for (int i = 0; i < n; i++) {
        const uint8_t w = chars && chars[i] < l.value_chars ? chars[i] : l.value_chars;
        const int16_t x = l.align == Align::Right ? static_cast<int16_t>(l.value_x + (l.value_chars - w) * f.adv) : l.value_x;
        cells[i].init(s, f, x, static_cast<int16_t>(kv_row_y(l, i) + row_text_y(l.pitch, f.height)), w, l.align, kText, kBg);
    }
}

K2_HOT void kv_ground_row(const Surface &s, const KvLayout &l, const char *label, int i, const uint8_t *chars, uint32_t marks) {
    const Font &kf = detail::font_or_small(l.label_font), &vf = detail::font_or_small(l.value_font);
    const int16_t y = kv_row_y(l, i);
    const uint8_t w = chars && chars[i] < l.value_chars ? chars[i] : l.value_chars;
    const int16_t vx = l.align == Align::Right ? static_cast<int16_t>(l.value_x + (l.value_chars - w) * vf.adv) : l.value_x;
    Box holes[3];
    int h = 0;
    if (marks >> i & 1u)
        holes[h++] = {kMarkX, static_cast<int16_t>(y + 1 + (l.pitch - 1 - kMarkSize) / 2), kMarkSize, kMarkSize};
    holes[h++] = {l.label_x, static_cast<int16_t>(y + row_text_y(l.pitch, kf.height)), static_cast<int16_t>(text_width(kf, label)), kf.height};
    holes[h++] = {vx, static_cast<int16_t>(y + row_text_y(l.pitch, vf.height)), static_cast<int16_t>(w * vf.adv), vf.height};
    fill_except(s, {0, static_cast<int16_t>(y + 1), s.w, static_cast<int16_t>(l.pitch - 1)}, holes, h, kBg);
}

K2_HOT void kv_row(const Surface &s, const KvLayout &l, int16_t y, std::string_view key, std::string_view value, uint16_t value_color,
            bool ours K2_TRUNC_DEF) {
    const Font &kf = detail::font_or_small(l.label_font), &vf = detail::font_or_small(l.value_font);
    char buf[kFitBuf];
    gfx_fill(s, 0, y, s.w, l.clear ? l.pitch : 1, kBg);
    gfx_text(s, kf, l.label_x, static_cast<int16_t>(y + row_text_y(l.pitch, kf.height)), key, kInk2, kBg);
    const int box = l.value_chars * vf.adv;
    gfx_text(s, vf, l.value_x, static_cast<int16_t>(y + row_text_y(l.pitch, vf.height)),
             ours ? fit_text(vf, value, box, buf K2_TRUNC_ARG) : fit_data(vf, value, box, buf K2_TRUNC_ARG), value_color, kBg);
}

}  // namespace k2::kit
