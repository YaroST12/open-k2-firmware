// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/strpack.hpp"
#include "k2/ui/kit.hpp"
#include "kit_internal.hpp"
#include "k2/core/math_opt.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui_viewport.hpp"
#include <algorithm>

namespace k2::kit {

namespace {
bool g_browse;   // .bss zero: boot paints entered until Ui sets the ring state
// Text centred in a box (x, w) on the line whose top is `y`.
void centred_text(const Surface &s, const Font &f, int16_t x, int16_t w, int16_t y, std::string_view text, uint16_t fg, uint16_t bg) {
    char buf[kFitBuf];
    const std::string_view t = fit_text(f, text, w, buf);
    gfx_text(s, f, static_cast<int16_t>(x + (w - text_width(f, t)) / 2), y, t, fg, bg);
}
}  // namespace

void set_browse(bool on) { g_browse = on; }
bool browsing() { return g_browse; }

namespace detail {
LabelLayout label_layout(int16_t x, int16_t y, int16_t w, int16_t h, std::string_view label, char (&buf)[kFitBuf]) {
    const Font &f = h >= 22 ? kSmall : kLabelFont;
    const std::string_view t = fit_text(f, label, w, buf);
    const int16_t tw = static_cast<int16_t>(text_width(f, t));
    return {&f, t, {static_cast<int16_t>(x + (w - tw) / 2), static_cast<int16_t>(y + (h - f.height) / 2), tw, f.height}};
}

Box strike_line(const Font &f, int16_t tx, int16_t ty, int16_t tw, int16_t cx, int16_t cw) {
    const Glyph *gx = find_glyph(f, 'x');
    const int xh = gx ? gx->top : f.ascent * 2 / 3;
    int16_t x0 = static_cast<int16_t>(tx - 1), x1 = static_cast<int16_t>(tx + tw + 1);
    if (x0 < cx) x0 = cx;
    if (x1 > cx + cw) x1 = static_cast<int16_t>(cx + cw);
    return {x0, static_cast<int16_t>(ty + f.ascent - 1 - xh / 2), static_cast<int16_t>(x1 > x0 ? x1 - x0 : 0), 1};
}
}  // namespace detail

int strike_px(Verdict v, std::string_view label, int16_t w, int16_t h) {
    if (v != Verdict::Skip || label.empty()) return 0;
    char buf[kFitBuf];
    const detail::LabelLayout l = detail::label_layout(0, 0, w, h, label, buf);
    return l.text.empty() ? 0 : detail::strike_line(*l.font, l.hole.x, l.hole.y, l.hole.w, 0, w).w;
}

void draw_struck_text(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text, uint16_t ink, uint16_t bg) {
    gfx_text(s, f, x, y, text, ink, bg);
    if (text.empty()) return;
    const Box k = detail::strike_line(f, x, y, static_cast<int16_t>(text_width(f, text)), 0, s.w);
    if (k.w > 0) gfx_fill(s, k.x, k.y, k.w, k.h, ink);
}

// ---- VerdictCell --------------------------------------------------------------------------------------------------------
void draw_run_mark(const Surface &s, const CellRect &cell, bool on, uint16_t under) {
    draw_frame(s, cell.x, cell.y, cell.w, cell.h, kRunFrame, on ? verdict_style(Verdict::Running).mark : under);
}

// Every pixel of the cell is streamed once: the fill leaves a hole for the label or the glyph. The exceptions are a Skip
// label's strike, a 1 px line painted over the text (strike_px), and a Running label's outline over the cell's rim (mark_px).
void draw_verdict(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, Verdict v, std::string_view label,
                  uint16_t under, bool sweep) {
    const VerdictStyle st = verdict_style(v);
    const uint16_t base = st.filled ? st.fill : under;
    Box inner{x, y, w, h};
    if (!label.empty()) {
        char buf[kFitBuf];
        const detail::LabelLayout lay = detail::label_layout(x, y, sweep ? static_cast<int16_t>(w - kSweepMarkW) : w, h, label, buf);
        const Box hole = lay.hole;
        // The sweep mark sits on the label's baseline, right of it; it is a second hole, so every pixel is still streamed once.
        const Box sm{static_cast<int16_t>(hole.x + hole.w + kSweepGap), static_cast<int16_t>(hole.y + lay.font->ascent - kLabelFont.ascent),
                     static_cast<int16_t>(kLabelFont.adv), static_cast<int16_t>(kLabelFont.height)};
        if (v == Verdict::Partial) {   // the shape cue: a bar along the bottom edge, so a partial never reads as a pass in grey
            inner.h = static_cast<int16_t>(inner.h - kPartialBarH);
            gfx_fill(s, x, static_cast<int16_t>(y + h - kPartialBarH), w, kPartialBarH, st.ink);
        } else if (v == Verdict::Noted) {   // the shape cue: a dim bar down the left edge (listed, not measured)
            inner.x = static_cast<int16_t>(inner.x + kNotedBarW);
            inner.w = static_cast<int16_t>(inner.w - kNotedBarW);
            gfx_fill(s, x, y, kNotedBarW, h, st.mark);
        }
        const Box holes[2] = {hole, sm};
        fill_except(s, inner, holes, sweep ? 2 : 1, base);
        if (sweep) gfx_text(s, kLabelFont, sm.x, sm.y, "S", v == Verdict::Fail ? st.ink : kDim, base);   // dim; black on the solid red
        gfx_text(s, *lay.font, hole.x, hole.y, lay.text, st.ink, base);
        if (v == Verdict::Skip && !lay.text.empty()) {
            const Box k = detail::strike_line(*lay.font, hole.x, hole.y, hole.w, x, w);
            gfx_fill(s, k.x, k.y, k.w, k.h, st.ink);
        }
        if (st.marked) draw_run_mark(s, {x, y, w, h}, true);   // over the cell's rim, after the text
        return;
    }
    // No label: the state's glyph, centred, in a square cell kept at its native size like a row marker (a squeeze dropped rows of
    // the cell and of its 1 px rims); `under` fills the rest of the mapped box.
    if (ContentViewport::scaled()) {
        const Rect m = ContentViewport::mapped_rect({static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y), w, h});
        const Rect n = gfx_native_rect(s, x, y, w, h);
        if (n.w != w || n.h != h) return;
        ContentViewport::Pause unscaled;
        const Surface panel{0, 0, kScreenW, kScreenH};
        if (m.x + m.w > n.x + w) gfx_fill(panel, static_cast<int16_t>(n.x + w), m.y, static_cast<int16_t>(m.x + m.w - n.x - w), m.h, under);
        if (m.y + m.h > n.y + h) gfx_fill(panel, m.x, static_cast<int16_t>(n.y + h), m.w, static_cast<int16_t>(m.y + m.h - n.y - h), under);
        draw_verdict(panel, n.x, n.y, w, h, v, label, under, sweep);
        return;
    }
    int sz = (w < h ? w : h) - 4;
    if (sz > 12) sz = 12;
    const int16_t gx = static_cast<int16_t>(x + (w - sz) / 2), gy = static_cast<int16_t>(y + (h - sz) / 2);
    const int16_t mx = static_cast<int16_t>(gx + sz / 2 - 1), my = static_cast<int16_t>(gy + sz / 2 - 1);
    Box hole{gx, gy, static_cast<int16_t>(sz), static_cast<int16_t>(sz)};
    bool solid_hole = false;
    switch (v) {
    case Verdict::None:
    case Verdict::Skip:
    case Verdict::Stopped: hole = {gx, my, static_cast<int16_t>(sz), 2}; solid_hole = true; break;
    case Verdict::Pending: hole = {mx, my, 2, 2}; solid_hole = true; break;
    case Verdict::Noted:   hole = {mx, gy, 2, static_cast<int16_t>(sz)}; solid_hole = true; break;
    case Verdict::Partial: hole = {gx, static_cast<int16_t>(my - 2), static_cast<int16_t>(sz), 6}; break;
    case Verdict::Running:
    case Verdict::Pass:
    case Verdict::Fail: break;
    }
    fill_except(s, inner, &hole, sz < 4 ? 0 : 1, base);
    if (sz < 4) return;
    if (solid_hole) {
        gfx_fill(s, hole.x, hole.y, hole.w, hole.h, st.ink);
    } else if (v == Verdict::Partial) {
        // A tilde: four columns of 2 px bars stepping mid, up, down, mid inside the 6 px strip, each column painted once.
        const int16_t q = static_cast<int16_t>(sz / 4);
        static constexpr int8_t kStep[4] = {0, -2, 2, 0};
        for (int i = 0; i < 4; i++) {
            const int16_t cx = static_cast<int16_t>(gx + i * q), cw = i == 3 ? static_cast<int16_t>(sz - 3 * q) : q;
            const int16_t above = static_cast<int16_t>(2 + kStep[i]), below = static_cast<int16_t>(2 - kStep[i]);
            if (above) gfx_fill(s, cx, static_cast<int16_t>(my - 2), cw, above, base);
            gfx_fill(s, cx, static_cast<int16_t>(my + kStep[i]), cw, 2, st.ink);
            if (below) gfx_fill(s, cx, static_cast<int16_t>(my + kStep[i] + 2), cw, below, base);
        }
    } else if (v == Verdict::Running) {
        const int16_t t = sz >= 8 ? kRunFrame : 1;
        draw_frame(s, gx, gy, static_cast<int16_t>(sz), static_cast<int16_t>(sz), t, st.mark);
        gfx_fill(s, static_cast<int16_t>(gx + t), static_cast<int16_t>(gy + t), static_cast<int16_t>(sz - 2 * t),
                 static_cast<int16_t>(sz - 2 * t), base);
    } else {
        draw_mark(s, gx, gy, sz, v == Verdict::Pass, st.ink, base);
    }
}

// ---- TopBar -------------------------------------------------------------------------------------------------------------
void draw_progress_rule(const Surface &s, int step, int segs) {
    constexpr int kSpan = kProgSegs * kProgPitch;   // 225: the segments plus one gap after the last, over 224 px
    for (int i = 0; i < segs; i++) {
        const int x0 = i * kSpan / segs, x1 = (i + 1) * kSpan / segs;
        gfx_fill(s, static_cast<int16_t>(x0), kProgY, static_cast<int16_t>(x1 - x0 - 1), kProgH, i < step ? kFocus : kRule);
    }
}

void draw_top_bar(const Surface &s, std::string_view title, int step, std::string_view badge, bool readout_follows, int segs) {
    char buf[40];
    size_t n = 0;
    if (step >= 1 && step <= segs) {
        buf[n++] = static_cast<char>('0' + step);
        buf[n++] = '/';
        buf[n++] = static_cast<char>('0' + segs);
        buf[n++] = ' ';
    }
    strpack::for_chars(title, [&](char c) { if (n < sizeof(buf)) buf[n++] = c; });
    draw_status_left(s, false, Icon::Bolt, std::string_view(buf, n), 132, badge, readout_follows);
    if (step > 0) draw_progress_rule(s, step, segs);
    else gfx_fill(s, 0, kProgY + 1, s.w, kProgH - 1, kBg);
}

// ---- RowList -------------------------------------------------------------------------------------------------------------
void draw_drill(const Surface &s, int16_t right, int16_t cy, uint16_t color) {
    // A 2 px stroke chevron, 6 wide and 9 tall: the arm rows step one px per row towards the point.
    constexpr int16_t kChevW = 6;
    for (int i = 0; i < 5; i++) {
        const int16_t x = static_cast<int16_t>(right - kChevW + i);
        gfx_fill(s, x, static_cast<int16_t>(cy - 4 + i), 2, 1, color);
        if (i < 4) gfx_fill(s, x, static_cast<int16_t>(cy + 4 - i), 2, 1, color);
    }
}

void draw_marker(const Surface &s, int16_t x, int16_t y, const RowSpec &r, uint16_t bg) {
    const bool on = r.state != 0;
    const uint16_t col = r.marker_color ? r.marker_color : on ? kFocus : kDim;
    // The square is drawn at its native size, clipped to the viewport (not squeezed with the row body like a plain
    // fill): in a landscape rotation the row mapping can squeeze a 1 px border line away to zero physical rows,
    // and which lines vanish moves as the rows scroll — the selection squares then lose their top or bottom edge.
    // The whole square is mapped once and every sub-rect is offset in native pixels, so the 1 px frame survives.
    const Rect m = gfx_native_rect(s, x, y, kMarkSize, kMarkSize);
    if (m.w <= 0 || m.h <= 0) return;
    const auto fill = [&](int16_t ox, int16_t oy, int16_t fw, int16_t fh, uint16_t c) {
        gfx_fill_panel({static_cast<int16_t>(m.x + ox), static_cast<int16_t>(m.y + oy), fw, fh}, c);
    };
    const auto frame = [&] {
        fill(0, 0, m.w, 1, col);
        fill(0, static_cast<int16_t>(m.h - 1), m.w, 1, col);
        fill(0, 1, 1, static_cast<int16_t>(m.h - 2), col);
        fill(static_cast<int16_t>(m.w - 1), 1, 1, static_cast<int16_t>(m.h - 2), col);
    };
    switch (r.marker) {
    case Marker::None: break;
    case Marker::Radio:
        if (on) {
            fill(0, 0, m.w, m.h, col);
            break;
        }
        [[fallthrough]];
    case Marker::Box3:
    case Marker::Check:
        frame();
        fill(1, 1, static_cast<int16_t>(m.w - 2), static_cast<int16_t>(m.h - 2), bg);
        if (r.marker == Marker::Box3) {
            if (r.state >= 2) fill(3, 3, 6, 6, col);
            else if (r.state == 1) fill(3, 5, 6, 2, col);
        } else if (r.marker == Marker::Check && on) {
            draw_mark(s, static_cast<int16_t>(x + 2), static_cast<int16_t>(y + 2), 8, true, col, bg);
        }
        break;
#if K2_TWIN
    case Marker::Level: {
        const int16_t in = static_cast<int16_t>(m.h - 2);
        const int16_t ink = r.state >= 2 ? in : r.state == 1 ? static_cast<int16_t>(in / 2) : 0;
        frame();
        if (in - ink > 0) fill(1, 1, static_cast<int16_t>(m.w - 2), static_cast<int16_t>(in - ink), bg);
        if (ink > 0) fill(1, static_cast<int16_t>(1 + in - ink), static_cast<int16_t>(m.w - 2), ink, col);
        break;
    }
#else
    case Marker::Level: break;
#endif
    case Marker::Verdict:
        draw_verdict(s, x, y, kMarkSize, kMarkSize, static_cast<Verdict>(r.state), {}, bg);
        break;
    case Marker::Icon:
        draw_icon(s, x, y, r.icon, r.marker_color ? r.marker_color : kDim, bg);
        break;
    }
}

void RowList::init(const Surface &s, int16_t y, RowFn fn, void *ctx) {
    const bool rebind = ctx != ctx_;
    surf_ = s; y_ = y; fn_ = fn; ctx_ = ctx;
    int rows = (kFooterY - y - kFocusExtra) / kRowH;
    if (rows > ListView::kMaxRows) rows = ListView::kMaxRows;
    list_.init(s, 0, y, 1, kRowH, rows, kSmall, draw_row_cb, this);
    list_.set_row_sig(row_sig_cb, this);
    list_.set_row_bg(kBg, kFocusFill, kBg);   // Fieldbook: white space between rows, no hairlines
    list_.set_full_lane(true);
    list_.set_self_ground(true);   // draw_row_cb paints the row's ground itself, around its text and marker
    list_.set_highlight(lit());
    if (rebind) list_.set_offset(0);
    extra_ = -1;
}

void RowList::invalidate() {
    list_.invalidate();
    extra_ = -1;
}

void RowList::clear_body_around(const Surface &s) {
    gfx_fill(s, 0, kBelowBar, s.w, static_cast<int16_t>(kBodyY - kBelowBar), kBg);
    gfx_fill(s, 0, kListBottom, s.w, static_cast<int16_t>(kFooterY - kListBottom), kBg);
}

// ---- KvTable -------------------------------------------------------------------------------------------------------------
void kv_static_row(const Surface &s, const KvLayout &l, const char *label, int i, bool trailing_line) {
    const Font &f = detail::font_or_small(l.label_font);
    const int16_t y = kv_row_y(l, i);
    gfx_fill(s, 0, y, s.w, 1, kBg);
    const int16_t ty = static_cast<int16_t>(y + row_text_y(l.pitch, f.height));
    gfx_text(s, f, l.label_x, ty, label, kInk2, kBg);
    draw_leader(s, static_cast<int16_t>(l.label_x + text_width(f, label) + 5), static_cast<int16_t>(l.value_x - kLeaderGap),
                static_cast<int16_t>(ty + f.ascent - 2));
    if (trailing_line) gfx_fill(s, 0, kv_row_y(l, i + 1), s.w, 1, kBg);
}

void draw_dashed(const Surface &s, int16_t x0, int16_t x1, int16_t y, uint16_t ink, uint16_t bg) {
    for (int16_t x = x0; x < x1; x = static_cast<int16_t>(x + kDashOn + kDashOff)) {
        const int16_t on = static_cast<int16_t>(std::min<int>(kDashOn, x1 - x));
        gfx_fill(s, x, y, on, 1, ink);
        const int16_t off = static_cast<int16_t>(std::min<int>(kDashOff, x1 - x - on));
        if (off > 0) gfx_fill(s, static_cast<int16_t>(x + on), y, off, 1, bg);
    }
}

void kv_cells(const Surface &s, const KvLayout &l, Field *cells, int n, const uint8_t *chars) {
    const Font &f = detail::font_or_small(l.value_font);
    for (int i = 0; i < n; i++) {
        const uint8_t w = chars && chars[i] < l.value_chars ? chars[i] : l.value_chars;
        const int16_t x = l.align == Align::Right ? static_cast<int16_t>(l.value_x + (l.value_chars - w) * f.adv) : l.value_x;
        cells[i].init(s, f, x, static_cast<int16_t>(kv_row_y(l, i) + row_text_y(l.pitch, f.height)), w, l.align, kText, kBg);
    }
}

void kv_ground_row(const Surface &s, const KvLayout &l, const char *label, int i, const uint8_t *chars, uint32_t marks) {
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

void kv_row(const Surface &s, const KvLayout &l, int16_t y, std::string_view key, std::string_view value, uint16_t value_color,
            bool ours K2_TRUNC_DEF) {
    const Font &kf = detail::font_or_small(l.label_font), &vf = detail::font_or_small(l.value_font);
    char buf[kFitBuf];
    gfx_fill(s, 0, y, s.w, l.clear ? l.pitch : 1, kBg);
    gfx_text(s, kf, l.label_x, static_cast<int16_t>(y + row_text_y(l.pitch, kf.height)), key, kInk2, kBg);
    const int box = l.value_chars * vf.adv;
    gfx_text(s, vf, l.value_x, static_cast<int16_t>(y + row_text_y(l.pitch, vf.height)),
             ours ? fit_text(vf, value, box, buf K2_TRUNC_ARG) : fit_data(vf, value, box, buf K2_TRUNC_ARG), value_color, kBg);
}

void kv_static(const Surface &s, const KvLayout &l, const char *const *labels, int n) {
    for (int i = 0; i < n; i++) kv_static_row(s, l, labels[i], i, i + 1 == n);
}

[[gnu::noinline]] void kv_draw_text(const Surface &s, const KvLayout &l, int row, std::string_view text,
                                    uint16_t fg, uint16_t bg) {
    gfx_text(s, kSmall, l.value_x, static_cast<int16_t>(kv_row_y(l, row) + row_text_y(l.pitch, kSmall.height)),
             text, fg, bg);
}

[[gnu::noinline]] void draw_big_unit(const Surface &s, int16_t x, int16_t y, uint8_t num_chars,
                                     std::string_view unit, uint16_t col) {
    gfx_text(s, kMid, static_cast<int16_t>(x + num_chars * kBig.adv + 2),
             static_cast<int16_t>(y + kBig.ascent - kMid.ascent), unit, col, kBg);
}

// Native dots at their mapped origins: a stretched 2 px fill came out 3 px wide every few dots in landscape.
void draw_leader(const Surface &s, int16_t x0, int16_t x1, int16_t y) {
    for (int x = (x0 + kLeaderPitch - 1) / kLeaderPitch * kLeaderPitch; x + kLeaderDot <= x1; x += kLeaderPitch)
        gfx_fill_panel(gfx_native_rect(s, static_cast<int16_t>(x), y, kLeaderDot, kLeaderDot), kInk3);
}

int draw_chip(const Surface &s, int16_t x, int16_t y, std::string_view text, uint16_t fill, uint16_t ink, int16_t min_w,
              uint16_t ground) {
    const int16_t w = std::max<int16_t>(static_cast<int16_t>(badge_text_bounds(kLabelFont, text).w + 8), min_w);
    draw_pill_box(s, x, y, w, kChipH, text, ink, fill, fill, ground);
    return w;
}

void erase_chips(const Surface &s, int16_t x, int16_t y, int16_t w, uint16_t ground) {
    gfx_fill(s, x, y, w, kChipH, ground);
    if (!ContentViewport::scaled() || w <= 0) return;
    const Rect m = ContentViewport::mapped_rect({static_cast<int16_t>(s.x + x), static_cast<int16_t>(s.y + y), w, kChipH});
    if (m.h < kChipH) gfx_fill_panel({m.x, static_cast<int16_t>(m.y + m.h), m.w, static_cast<int16_t>(kChipH - m.h)}, ground);
}

int draw_chip_frame(const Surface &s, int16_t x, int16_t y, std::string_view text, uint16_t frame, uint16_t ink, uint16_t bg) {
    const int16_t w = static_cast<int16_t>(badge_text_bounds(kLabelFont, text).w + 8);
    draw_pill_box(s, x, y, w, kChipH, text, ink, bg, frame, bg);
    return w;
}

static_assert(8 + 32 * 6 <= 208 - 4, "the identity line's 32 cells of font 12 fit beside the lane");

bool paint_identity(const Surface &c, const Ident &id, bool full, uint32_t &sig, PaintBudget *bg) {
    uint32_t h = mix_u32(uint32_t(id.dim) | uint32_t(id.n) << 1);
    h = mix(h, id.head.view());
    for (int i = 0; i < id.n; i++) {
        h = mix_u32(h, 0x100u | id.chip[i].k);
        h = mix(h, id.chip[i].t.view());
    }
    h = mix(mix_u32(h, 0x200u), id.line.view()) | 1u;
    if (!full && h == sig) return true;
    if (bg && !bg->admit(static_cast<int>(c.w) * (kPlaceListY - kIdY), true)) return false;
    sig = h;
    char lbuf[kFitBuf];
    const std::string_view line_v = id.line.view().empty() ? std::string_view{} : fit_data(kLabelFont, id.line.view(), c.w - 12, lbuf);
    const int16_t hw = static_cast<int16_t>(text_width(kMid, id.head.view()));
    const int16_t chip_y = static_cast<int16_t>(kIdHeadY + (kMid.height - kChipH) / 2);
    Box holes[6];
    int nh = 0;
    holes[nh++] = {8, kIdHeadY, hw, static_cast<int16_t>(kMid.height)};
    int16_t cx[3] = {};
    int shown = 0;
    for (int16_t x = static_cast<int16_t>(8 + hw + 8); shown < id.n; shown++) {   // a chip that does not fit is left out whole
        const int16_t cw = static_cast<int16_t>(badge_text_bounds(kLabelFont, id.chip[shown].t.view()).w + 8);
        if (x + cw > c.w - 8) break;
        cx[shown] = x;
        holes[nh++] = {x, chip_y, cw, kChipH};
        x = static_cast<int16_t>(x + cw + 4);
    }
    const int16_t lw = static_cast<int16_t>(text_width(kLabelFont, line_v));
    if (lw > 0) holes[nh++] = {8, kIdLineY, lw, static_cast<int16_t>(kLabelFont.height)};
    fill_except(c, {0, kIdY, c.w, static_cast<int16_t>(kIdDashY - kIdY)}, holes, nh, kBg);
    gfx_text(c, kMid, 8, kIdHeadY, id.head.view(), id.dim ? kInk2 : kInk, kBg);
    static constexpr struct { uint16_t fill, ink; } kChipCols[4] = {
        {kChip, kOnChip}, {kInk2, kInk}, {kPassFill, kOnPass}, {kSpotVolt, kSpotInk},
    };
    for (int i = 0; i < shown; i++) {
        const Ident::Chip &ch = id.chip[i];
        const auto &cc = kChipCols[ch.k];
        if (ch.k == Ident::Outline) draw_chip_frame(c, cx[i], chip_y, ch.t.view(), cc.fill, cc.ink, kBg);
        else draw_chip(c, cx[i], chip_y, ch.t.view(), cc.fill, cc.ink);
    }
    if (lw > 0) gfx_text(c, kLabelFont, 8, kIdLineY, line_v, kInk2, kBg);
    gfx_fill(c, 0, kIdDashY, 8, 1, kBg);
    draw_dashed(c, 8, static_cast<int16_t>(c.w - 8), kIdDashY, kInk3, kBg);
    gfx_fill(c, static_cast<int16_t>(c.w - 8), kIdDashY, 8, 1, kBg);
    gfx_fill(c, 0, static_cast<int16_t>(kIdDashY + 1), c.w, static_cast<int16_t>(kPlaceListY - kIdDashY - 1), kBg);
    return true;
}

void fill_list_gap(const Surface &s, int16_t y) {
    const int16_t end = list_end(y);
    if (end < kListBottom) gfx_fill(s, 0, end, s.w, static_cast<int16_t>(kListBottom - end), kBg);
}

void kv_ground(const Surface &s, const KvLayout &l, const char *const *labels, int n, const uint8_t *chars, uint32_t marks) {
    for (int i = 0; i < n; i++) kv_ground_row(s, l, labels[i], i, chars, marks);
}

// ---- AlertBand -----------------------------------------------------------------------------------------------------------
void draw_warn_triangle(const Surface &s, int16_t x, int16_t y, int16_t sz, uint16_t color, uint16_t ink) {
    for (int r = 0; r < sz; r++) {
        const int w = 2 + r * (sz - 2) / (sz - 1);
        gfx_fill(s, static_cast<int16_t>(x + (sz - w) / 2), static_cast<int16_t>(y + r), static_cast<int16_t>(w), 1, color);
    }
    const int16_t cx = static_cast<int16_t>(x + sz / 2 - 1);
    gfx_fill(s, cx, static_cast<int16_t>(y + sz * 7 / 20), 2, static_cast<int16_t>(sz * 7 / 20), ink);
    gfx_fill(s, cx, static_cast<int16_t>(y + sz * 15 / 20), 2, 2, ink);
}

void draw_alert_band(const Surface &s, int16_t y, bool error, std::string_view word) {
    const uint16_t fill = error ? kError : kCaution, ink = error ? kErrInk : kSpotInk;
    gfx_fill(s, 0, y, s.w, kAlertH, fill);
    draw_warn_triangle(s, 4, static_cast<int16_t>(y + 8), 20, ink, fill);
    char buf[kFitBuf];
    gfx_text(s, kMid, 26, static_cast<int16_t>(y + (kAlertH - kMid.height) / 2), fit_text(kMid, word, s.w - 26 - 2, buf), ink, fill);
    gfx_fill(s, 0, static_cast<int16_t>(y + kAlertH), s.w, 2, ink);
    gfx_fill(s, 0, static_cast<int16_t>(y + kAlertH + 2), s.w, static_cast<int16_t>(kAlertStripeH - 2), kBg);
}

namespace {
void ticket_ground(const Surface &s, int16_t y, int16_t h, int16_t w, uint16_t fill) {
    gfx_fill(s, 0, y, kTicketInset, h, kBg);
    gfx_fill(s, static_cast<int16_t>(kTicketInset + w), y, kTicketInset, h, kBg);
    gfx_fill(s, kTicketInset, y, w, h, fill);
}
}  // namespace

// ---- ActionCell ----------------------------------------------------------------------------------------------------------
bool ActionCell::draw_ticket(uint32_t tag, std::string_view head, std::string_view sub,
                             uint16_t fill, uint16_t ink, uint16_t frame, bool armed, PaintBudget *bg) {
    uint32_t sig = mix_u32(tag);
    sig = mix(mix(sig, head), sub);
    const int16_t w = static_cast<int16_t>(surf_.w - 2 * kTicketInset);
    return e_.paint_budgeted(sig, bg, static_cast<int>(surf_.w) * h_, [&] {
        bar_px_ = -1;
        ticket_ground(surf_, y_, h_, w, fill);
        if (frame) draw_frame(surf_, kTicketInset, y_, w, h_, 2, frame);
        const int gap = armed ? 4 : 6;
        const int content = kMid.height + (armed || !sub.empty() ? gap + kLabelFont.height : 0);
        const int16_t top = static_cast<int16_t>(y_ + (h_ - (armed ? kBarH : 0) - content) / 2);
        centred_text(surf_, kMid, 4, static_cast<int16_t>(surf_.w - 8), top, head, ink, fill);
        if (sub.empty()) return;
        const int16_t ly = static_cast<int16_t>(top + kMid.height + gap);
        if (armed) {
            centred_text(surf_, kLabelFont, 4, static_cast<int16_t>(surf_.w - 8), ly, sub, ink, fill);
        } else {
            char buf[kFitBuf];
            constexpr int16_t kTri = 11;
            const std::string_view t = fit_text(kLabelFont, sub, surf_.w - 40, buf);
            const int tw = kTri + 4 + text_width(kLabelFont, t);
            const int16_t x0 = static_cast<int16_t>((surf_.w - tw) / 2);
            draw_warn_triangle(surf_, x0, static_cast<int16_t>(ly + (kLabelFont.height - kTri) / 2), kTri, kCaution, fill);
            gfx_text(surf_, kLabelFont, static_cast<int16_t>(x0 + kTri + 4), ly, t, ink, fill);
        }
    });
}

[[gnu::noinline]] bool ActionCell::draw_idle(std::string_view verb, std::string_view hint, PaintBudget *bg, bool focused) {
    focused = focused && !g_browse;
    return draw_ticket(focused ? 0 : 2, verb, hint,
                       focused ? kTicket : kBg, focused ? kOnTicket : kInk2, focused ? kTicketFrame : kRule, false, bg);
}

bool ActionCell::draw_armed(std::string_view word, std::string_view sub, int left_permille, PaintBudget *bg) {
    if (!draw_ticket(1, word, sub, kCaution, kSpotInk, 0, true, bg)) return false;
    const int16_t w = static_cast<int16_t>(surf_.w - 2 * kTicketInset);
    if (left_permille < 0) left_permille = 0;
    if (left_permille > 1000) left_permille = 1000;
    const int16_t span = static_cast<int16_t>(w - 1);
    const int16_t px = static_cast<int16_t>((span * left_permille + 500) / 1000);
    if (px == bar_px_) return true;
    const int16_t by = static_cast<int16_t>(y_ + h_ - kBarH);
    const int16_t x0 = kTicketInset;
    if (bar_px_ < 0) gfx_fill(surf_, x0, by, px, kBarH, kSpotInk);
    else if (px < bar_px_) gfx_fill(surf_, static_cast<int16_t>(x0 + px), by, static_cast<int16_t>(bar_px_ - px), kBarH, kCaution);
    else gfx_fill(surf_, static_cast<int16_t>(x0 + bar_px_), by, static_cast<int16_t>(px - bar_px_), kBarH, kSpotInk);
    bar_px_ = px;
    return true;
}

// ---- ProgressBar ---------------------------------------------------------------------------------------------------------
int ProgressBar::px_for(int done, int total) const {
    int px = total > 0 ? w_ * done / total : 0;
    if (px < 0) px = 0;
    if (px > w_) px = w_;
    return px;
}

int ProgressBar::cost(int done, int total) const {
    const int px = px_for(done, total);
    if (px == px_) return 0;
    if (px_ < 0) return w_ * kH;
    return (px > px_ ? px - px_ : px_ - px) * kH;
}

void ProgressBar::set(int done, int total) {
    const int px = px_for(done, total);
    if (px == px_) return;
    if (px_ < 0) {
        gfx_fill(surf_, static_cast<int16_t>(x_ + px), y_, static_cast<int16_t>(w_ - px), kH, kRule);
        gfx_fill(surf_, x_, y_, static_cast<int16_t>(px), kH, kFocus);
    } else if (px > px_) {
        gfx_fill(surf_, static_cast<int16_t>(x_ + px_), y_, static_cast<int16_t>(px - px_), kH, kFocus);
    } else {
        gfx_fill(surf_, static_cast<int16_t>(x_ + px), y_, static_cast<int16_t>(px_ - px), kH, kRule);
    }
    px_ = static_cast<int16_t>(px);
}

// ---- ResultGrid ----------------------------------------------------------------------------------------------------------
namespace {
// Cell i of n: first column and one past the last (a 1 px gap separates neighbours; the right edge stays clear).
int16_t cell_x0(const Surface &s, int n, int i) {
    const int span = s.w - (kResultLabelW + 1);
    return static_cast<int16_t>(kResultLabelW + 1 + (span * i * 2 + n) / (2 * n));
}

// Verdict cell i of n in a row whose cell part starts at y and is vh tall (the 1 px gap column after it is not part of the box).
CellRect verdict_cell_rect(const Surface &s, int16_t y, int16_t vh, int n, int i) {
    const int16_t x0 = cell_x0(s, n, i), x1 = static_cast<int16_t>(cell_x0(s, n, i + 1) - 1);
    return {x0, y, static_cast<int16_t>(x1 - x0), vh};
}

// The bracket of one run of cells that share a current, under their volt cells: the current text centred, a line either side
// of it with a tick at each end. The boxes are what the caller leaves unpainted when it wipes the run's column.
int bracket_holes(const Surface &s, int16_t y, int n, int i, int j, std::string_view cur, Box (&holes)[5]) {
    const int16_t gx0 = static_cast<int16_t>(cell_x0(s, n, i) + 2), gx1 = static_cast<int16_t>(cell_x0(s, n, j + 1) - 4);
    const int16_t gx = static_cast<int16_t>((gx0 + gx1) / 2), by = static_cast<int16_t>(y + 31);
    const int tw = text_width(kLabelFont, cur);
    holes[0] = {static_cast<int16_t>(gx - tw / 2), static_cast<int16_t>(y + 25), static_cast<int16_t>(tw), kLabelFont.height};
    const int half = tw / 2 + 4;
    if (gx1 - gx0 <= half * 2 + 8) return 1;
    holes[1] = {gx0, by, static_cast<int16_t>(gx - half - gx0), 1};
    holes[2] = {static_cast<int16_t>(gx + half), by, static_cast<int16_t>(gx1 - gx - half + 1), 1};
    holes[3] = {gx0, static_cast<int16_t>(by - 3), 1, 3};
    holes[4] = {gx1, static_cast<int16_t>(by - 3), 1, 3};
    return 5;
}
}  // namespace

// Every pixel of the row is streamed once (the row tiles into the label strip, the rule, the volt cells with their gap
// columns, the bracket band and the closing line), so a repaint needs no erase and costs the row's area, not twice it.
void draw_result_row(const Surface &s, int16_t y, int16_t h, std::string_view row_label, const ResultCell *cells, int n,
                     bool focus, bool section) {
    if (section) {
        gfx_fill(s, 0, y, s.w, kResultSectionH, kRule);
        y = static_cast<int16_t>(y + kResultSectionH);
        h = static_cast<int16_t>(h - kResultSectionH);
    }
    const int16_t body = static_cast<int16_t>(h - 1);   // the last line stays kBg: rows never touch
    gfx_fill(s, 0, static_cast<int16_t>(y + body), s.w, 1, kBg);
    // Label strip: the focus bar and tint, or the plain ground, with the right-aligned unit label on top.
    char buf[kFitBuf];
    const std::string_view lab = fit_text(kLabelFont, row_label, kResultLabelW - 6, buf);
    const int16_t lw = static_cast<int16_t>(text_width(kLabelFont, lab));
    const Box lab_box{static_cast<int16_t>(kResultLabelW - 3 - lw), static_cast<int16_t>(y + (h - kLabelFont.height) / 2), lw, kLabelFont.height};
    // Focus is inversion: the label cell is a focus block (Classic: its tint behind the 3 px bar).
    const int16_t bar = focus && kFocusBar ? 3 : 0;
    if (bar) gfx_fill(s, 0, y, bar, body, kFocusBar);
    fill_except(s, {bar, y, static_cast<int16_t>(kResultLabelW - bar), body}, &lab_box, 1, focus ? kFocusFill : kBg);
    gfx_text(s, kLabelFont, lab_box.x, lab_box.y, lab, focus ? kOnFocus : kDim, focus ? kFocusFill : kBg);
    gfx_fill(s, kResultLabelW, y, 1, body, kBg);   // Fieldbook: white space between the label strip and the cells
    if (n <= 0) {
        gfx_fill(s, kResultLabelW + 1, y, static_cast<int16_t>(s.w - kResultLabelW - 1), body, kBg);
        return;
    }
    bool two = false;
    for (int i = 0; i < n; i++) two = two || !cells[i].current.empty();
    const int16_t vh = two ? 24 : body;
    for (int i = 0; i < n; i++) {
        const CellRect cr = verdict_cell_rect(s, y, vh, n, i);
        draw_verdict(s, cr.x, cr.y, cr.w, cr.h, cells[i].verdict, cells[i].label, kBg, cells[i].sweep);
        gfx_fill(s, static_cast<int16_t>(cr.x + cr.w), y, 1, vh, kBg);   // the gap column between neighbours
    }
    if (!two) return;
    // One bracket per run of equal current, under the volt cells: the run's column is wiped around the bracket, then drawn.
    const int16_t zy = static_cast<int16_t>(y + vh), zh = static_cast<int16_t>(body - vh);
    for (int i = 0; i < n;) {
        int j = i;
        while (j + 1 < n && cells[j + 1].current == cells[i].current) j++;
        const int16_t zx = cell_x0(s, n, i), zw = static_cast<int16_t>(cell_x0(s, n, j + 1) - zx);
        if (cells[i].current.empty()) {
            gfx_fill(s, zx, zy, zw, zh, kBg);
        } else {
            Box holes[5];
            const int nh = bracket_holes(s, y, n, i, j, cells[i].current, holes);
            fill_except(s, {zx, zy, zw, zh}, holes, nh, kBg);
            gfx_text(s, kLabelFont, holes[0].x, holes[0].y, cells[i].current, focus ? kInk : kDim, kBg);
            for (int k = 1; k < nh; k++) gfx_fill(s, holes[k].x, holes[k].y, holes[k].w, holes[k].h, kInk3);
        }
        i = j + 1;
    }
}

[[gnu::noinline]] void ResultGrid::init(const Surface &s, int16_t y, int16_t bottom, RowFn fn, void *ctx) {
    if (ctx != ctx_) first_ = 0;
    surf_ = s; y_ = y; bottom_ = bottom; fn_ = fn; ctx_ = ctx;
    dot_on_ = false;   // a fresh bind: whatever was on screen is the caller's to clear
    invalidate();
}

void ResultGrid::invalidate() {
    for (bool &v : valid_) v = false;
    end_y_ = -1;
}

namespace {
uint32_t row_hash(const ResultRowSpec &r, int16_t y, int16_t h, bool with_cur) {
    uint32_t k = mix_u32(static_cast<uint32_t>(r.n) | (static_cast<uint32_t>(r.section) << 4) |
                                         (static_cast<uint32_t>(r.focus) << 5) | (static_cast<uint32_t>(y) << 8) |
                                         (with_cur ? static_cast<uint32_t>(h) << 20 : 0u));
    k = mix(k, r.label);
    for (int i = 0; i < r.n; i++) {
        k = mix(k, r.cells[i].label);
        k = mix_u32(k, r.cells[i].sweep);   // the mark moves the label: a new shape
        if (with_cur) k = mix(k, r.cells[i].current);
    }
    return k;
}
uint32_t row_shape(const ResultRowSpec &r, int16_t y, int16_t h) { return row_hash(r, y, h, true); }
uint32_t row_core(const ResultRowSpec &r, int16_t y) { return row_hash(r, y, 0, false); }
// The cells' verdicts, 3 bits each (7 values, at most 5 cells). Same shape and other verdicts: only those cells repaint.
uint16_t row_verdicts(const ResultRowSpec &r) {
    uint16_t v = 0;
    for (int i = 0; i < r.n; i++) v = static_cast<uint16_t>(v | (static_cast<uint16_t>(r.cells[i].verdict) << (3 * i)));
    return v;
}
// Height of the verdict part of a row of total height h (the volts of a two-line row, else the whole body).
int16_t verdict_part_h(const ResultRowSpec &r, int16_t h) {
    bool two = false;
    for (int i = 0; i < r.n; i++) two = two || !r.cells[i].current.empty();
    return two ? 24 : static_cast<int16_t>(h - (r.section ? kResultSectionH : 0) - 1);
}
// The pixels painted over the cell area by the Skip cells' strikes and the Running cell's dot; `only` is a bit per cell
// (the changed ones in a verdict-only repaint), ~0 for the whole row.
int row_overlay_px(const Surface &s, const ResultRowSpec &r, int16_t h, unsigned only) {
    const int16_t vh = verdict_part_h(r, h);
    int px = 0;
    for (int k = 0; k < r.n; k++) {
        if (!((only >> k) & 1)) continue;
        const int16_t cw = verdict_cell_rect(s, 0, vh, r.n, k).w;
        px += strike_px(r.cells[k].verdict, r.cells[k].label, cw, vh);
        px += mark_px(r.cells[k].verdict, !r.cells[k].label.empty(), cw, vh);
    }
    return px;
}
// The cells whose verdict differs between two row_verdicts() values, one bit per cell.
unsigned changed_cells(uint16_t a, uint16_t b, int n) {
    unsigned m = 0;
    for (int k = 0; k < n; k++)
        if (((a ^ b) >> (3 * k)) & 7) m |= 1u << k;
    return m;
}
int16_t grid_row(ResultGrid::RowFn fn, void *ctx, int first, int i, ResultRowSpec &r) {
    fn(ctx, i, r);
    if (i == first) r.section = false;
    return result_row_h(r);
}
}  // namespace

void ResultGrid::invalidate_from(int16_t y) {
    int16_t yy = y_;
    int slot = 0;
    for (int i = first_; i < count_ && slot < kMaxSlots; i++, slot++) {
        ResultRowSpec r;
        const int16_t h = grid_row(fn_, ctx_, first_, i, r);
        if (yy + h > bottom_) break;
        yy = static_cast<int16_t>(yy + h);
        if (yy > y) valid_[slot] = false;
    }
}

void ResultGrid::show(int index, int lead) {
    if (index < 0 || index >= count_) return;
    if (first_ >= count_) first_ = count_ > 0 ? count_ - 1 : 0;
    if (index < first_) {
        first_ = index > lead ? index - lead : 0;
        return;
    }
    const int avail = bottom_ - y_;
    while (first_ < index) {
        int sum = 0;
        for (int i = first_; i <= index; i++) {
            ResultRowSpec r;
            sum += grid_row(fn_, ctx_, first_, i, r);
        }
        if (sum <= avail) break;
        first_++;
    }
}

void ResultGrid::anchor_bottom(int index) {
    if (index < 0 || index >= count_) return;
    const int avail = bottom_ - y_;
    // Window f..index costs sum(h) minus the section rule of its first row; it only grows as f falls, so walk f down from index.
    int sum = 0, f = index;
    for (int i = index; i >= 0; i--) {
        ResultRowSpec r;
        fn_(ctx_, i, r);
        const int h = result_row_h(r);
        const int sec = r.section ? kResultSectionH : 0;
        if (i < index && sum + h - sec > avail) break;   // i would be first: its own rule drops
        sum += h;
        f = i;
        if (i == index && h - sec > avail) break;
    }
    first_ = f;
}

int ResultGrid::row_y(int index) const {
    int16_t yy = y_;
    for (int i = first_; i < count_; i++) {
        ResultRowSpec r;
        const int16_t h = grid_row(fn_, ctx_, first_, i, r);
        if (yy + h > bottom_) break;
        if (i == index) return yy;
        yy = static_cast<int16_t>(yy + h);
    }
    return -1;
}

// Whether the outline's rim of a labelled cell overlaps the ink box of one of its glyphs (then wiping the outline would wipe
// label pixels, so the caller repaints the cell instead). The ink boxes are exact: in a grid cell the line box starts 4 px
// down and the ink 7, and a label is narrower than its cell by more than the rim, so the cheap wipe applies.
bool mark_hits_label(const CellRect &cr, std::string_view label, bool sweep) {
    char buf[kFitBuf];
    const detail::LabelLayout lay = detail::label_layout(cr.x, cr.y, sweep ? static_cast<int16_t>(cr.w - kSweepMarkW) : cr.w, cr.h, label, buf);
    const int x0 = cr.x + kRunFrame, x1 = cr.x + cr.w - kRunFrame, y0 = cr.y + kRunFrame, y1 = cr.y + cr.h - kRunFrame;
    const Font &f = *lay.font;
    int pen = lay.hole.x;
    for (const char c : lay.text) {
        const Glyph *g = find_glyph(f, static_cast<uint8_t>(c));
        if (g && g->w > 0 && g->h > 0) {
            const int gx = pen + g->left, gy = lay.hole.y + f.ascent - g->top;
            if (gx < x0 || gx + g->w > x1 || gy < y0 || gy + g->h > y1) return true;
        }
        pen += f.adv;
    }
    return false;
}

// The grid's dot bookkeeping after a paint: the cell was painted (so a dot that was on it is gone), and a Running one has its
// dot again (draw_verdict paints it).
void ResultGrid::note_dot(const CellRect &cell, bool running, const ResultCell &c, int slot, int k) {
    if (dot_on_ && dot_cell_.x == cell.x && dot_cell_.y == cell.y) dot_on_ = false;
    if (!running || c.label.empty()) return;
    dot_on_ = true; dot_cell_ = cell; dot_slot_ = slot; dot_k_ = k;
    dot_hit_ = mark_hits_label(cell, c.label, c.sweep);
}

int ResultGrid::sync_dots() {
    int spent = 0;
    // 1. Where the rows put the dot: the Running labelled cell of a row that is on screen where the rows put it.
    bool have = false, want_hit = false;
    CellRect want{};
    int wslot = 0, wk = 0;
    int16_t yy = y_;
    int slot = 0;
    for (int i = first_; i < count_ && slot < kMaxSlots; i++, slot++) {
        ResultRowSpec r;
        const int16_t h = grid_row(fn_, ctx_, first_, i, r);
        if (yy + h > bottom_) break;
        if (valid_[slot] && core_[slot] == row_core(r, yy)) {
            const int16_t cy = static_cast<int16_t>(yy + (r.section ? kResultSectionH : 0)), vh = verdict_part_h(r, h);
            for (int k = 0; k < r.n; k++) {
                if (r.cells[k].verdict != Verdict::Running || r.cells[k].label.empty()) continue;
                want = verdict_cell_rect(surf_, cy, vh, r.n, k);
                want_hit = mark_hits_label(want, r.cells[k].label, r.cells[k].sweep);
                wslot = slot; wk = k; have = true;
            }
        }
        yy = static_cast<int16_t>(yy + h);
    }
    const unsigned run = static_cast<unsigned>(Verdict::Running), pend = static_cast<unsigned>(Verdict::Pending);
    auto set_vd = [&](int s, int k, unsigned v) {
        vd_[s] = static_cast<uint16_t>((vd_[s] & ~(7u << (3 * k))) | (v << (3 * k)));
    };
    // 2. The old dot leaves, wherever it is on screen. Its cell reads Pending until draw_some paints the verdict it has now.
    if (dot_on_ && !(have && want.x == dot_cell_.x && want.y == dot_cell_.y)) {
        if (dot_hit_) {
            valid_[dot_slot_] = false;   // a cell too short for the cheap wipe: its row repaints with the budgeted rows
        } else {
            draw_run_mark(surf_, dot_cell_, false);
            spent += run_mark_px(dot_cell_.w, dot_cell_.h);
            if (valid_[dot_slot_] && ((vd_[dot_slot_] >> (3 * dot_k_)) & 7u) == run) set_vd(dot_slot_, dot_k_, pend);
        }
        dot_on_ = false;
    }
    // 3. The new dot arrives.
    if (have && !dot_on_) {
        draw_run_mark(surf_, want, true);
        spent += run_mark_px(want.w, want.h);
        dot_on_ = true; dot_cell_ = want; dot_slot_ = wslot; dot_k_ = wk; dot_hit_ = want_hit;
        set_vd(wslot, wk, run);
    }
    return spent;
}

// One pass over the visible rows. A row whose shape changed repaints whole (w * h px); a row that only changed verdicts repaints
// just those cells (draw_verdict with the same box draw_result_row uses, so the pixels are the same). `prefer` goes first.
// A piece that does not fit in what is left of `budget_px` waits for the next call (pending()); the first piece of a call
// always paints (so a call makes progress whatever the budget) unless `strict`, where nothing may exceed the budget.
int ResultGrid::draw_some(int budget_px, int prefer, bool strict) {
    int spent = 0;
    pending_ = false;
    auto fits_budget = [&](int cost) {
        if ((strict || spent > 0) && spent + cost > budget_px) { pending_ = true; return false; }
        return true;
    };
    int16_t yy = y_;
    int slot = 0;
    for (int pass = prefer >= first_ ? 0 : 1; pass < 2; pass++) {
        yy = y_;
        slot = 0;
        for (int i = first_; i < count_ && slot < kMaxSlots; i++, slot++) {
            ResultRowSpec r;
            const int16_t h = grid_row(fn_, ctx_, first_, i, r);
            if (yy + h > bottom_) break;
            if ((i == prefer) == (pass == 0)) {
                const uint32_t sh = row_shape(r, yy, h);
                const uint16_t vd = row_verdicts(r);
                const int16_t cy = static_cast<int16_t>(yy + (r.section ? kResultSectionH : 0));
                const int16_t vh = verdict_part_h(r, h);
                if (!valid_[slot] || sig_[slot] != sh) {
                    const int cost = surf_.w * h + row_overlay_px(surf_, r, h, ~0u);
                    if (fits_budget(cost)) {
                        draw_result_row(surf_, yy, h, r.label, r.cells, r.n, r.focus, r.section);
                        spent += cost;
                        sig_[slot] = sh; vd_[slot] = vd; core_[slot] = row_core(r, yy); valid_[slot] = true;
                        if (dot_on_ && dot_cell_.y >= yy && dot_cell_.y < yy + h) dot_on_ = false;   // the row covers the whole width
                        for (int k = 0; k < r.n; k++)
                            if (r.cells[k].verdict == Verdict::Running)
                                note_dot(verdict_cell_rect(surf_, cy, vh, r.n, k), true, r.cells[k], slot, k);
                    }
                } else if (vd_[slot] != vd) {
                    const unsigned chg = changed_cells(vd_[slot], vd, r.n);
                    int cost = row_overlay_px(surf_, r, h, chg);
                    for (int k = 0; k < r.n; k++)
                        if ((chg >> k) & 1) cost += verdict_cell_rect(surf_, cy, vh, r.n, k).w * vh;
                    if (fits_budget(cost)) {
                        for (int k = 0; k < r.n; k++) {
                            if (!((chg >> k) & 1)) continue;
                            const CellRect cr = verdict_cell_rect(surf_, cy, vh, r.n, k);
                            draw_verdict(surf_, cr.x, cr.y, cr.w, cr.h, r.cells[k].verdict, r.cells[k].label, kBg, r.cells[k].sweep);
                            note_dot(cr, r.cells[k].verdict == Verdict::Running, r.cells[k], slot, k);
                        }
                        spent += cost;
                        vd_[slot] = vd;
                    }
                }
            }
            yy = static_cast<int16_t>(yy + h);
        }
    }
    vis_ = slot;
    for (int k = slot; k < kMaxSlots; k++) valid_[k] = false;
    if (yy != end_y_) {
        const int cost = bottom_ > yy ? (bottom_ - yy) * surf_.w : 0;
        if (fits_budget(cost)) {
            if (cost) gfx_fill(surf_, 0, yy, surf_.w, static_cast<int16_t>(bottom_ - yy), kBg);
            if (dot_on_ && dot_cell_.y >= yy) dot_on_ = false;   // under the last row: wiped
            spent += cost;
            end_y_ = yy;
        }
    }
    return spent;
}

void ResultGrid::draw() { draw_some(1 << 30); }

// ---- PassBandGauge & RangeGauge ------------------------------------------------------------------------------------------
namespace {
struct StripCols {
    int16_t c0, c1;
    void fill(const Surface &s, int16_t y, int a, int b, int row, int rows, uint16_t col) const {
        a = std::max<int>(a, c0);
        b = std::min<int>(b, c1);
        if (b > a) gfx_fill(s, static_cast<int16_t>(a), static_cast<int16_t>(y + row),
                            static_cast<int16_t>(b - a), static_cast<int16_t>(rows), col);
    }
};

// Clamps [c0, c1) to [x, x + w) and paints the strip's ground and 4 px rule track.
bool strip_base(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t c0, int16_t c1, StripCols &out) {
    if (c0 < x) c0 = x;
    if (c1 > x + w) c1 = static_cast<int16_t>(x + w);
    if (c1 <= c0) return false;
    out = {c0, c1};
    gfx_fill(s, c0, y, static_cast<int16_t>(c1 - c0), kPassBandH, kBg);
    gfx_fill(s, c0, static_cast<int16_t>(y + 4), static_cast<int16_t>(c1 - c0), 4, kRule);
    return true;
}

// Paints columns [c0, c1) of the pass-band strip.
void gauge_cols(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t lo, int16_t hi, int16_t c0, int16_t c1) {
    StripCols sc;
    if (!strip_base(s, x, y, w, c0, c1, sc)) return;
    sc.fill(s, y, lo, hi + 1, 2, 8, kOkT);
    sc.fill(s, y, lo, hi + 1, 4, 4, kOk);
}

// Paints columns [c0, c1) of the range strip: ground and track, the dotted end lines, the tint between them, the setpoint
// marker's caps, then the needle over everything. Every part clips to the columns, so any column interval repaints whole.
void range_cols(const Surface &s, int16_t x, int16_t y, int16_t w, const RangeGeom &g, int16_t c0, int16_t c1) {
    StripCols sc;
    if (!strip_base(s, x, y, w, c0, c1, sc)) return;
    sc.fill(s, y, g.lo + 2, g.hi - 1, 2, 8, kOkT);          // the tint between the lines (rows 2..9, over the track too)
    for (const int a : {int(g.lo), int(g.hi) - 1})         // the lines: 2 px wide, 2 rows on / 2 off (rows 0,1 4,5 8,9)
        for (const int r : {0, 4, 8}) sc.fill(s, y, a, a + 2, r, 2, kOk);
    sc.fill(s, y, g.mark - 2, g.mark + 4, 0, 3, kFocus);   // the marker: a cap on the top and bottom edges
    sc.fill(s, y, g.mark - 2, g.mark + 4, 9, 3, kFocus);
    sc.fill(s, y, g.needle, g.needle + 2, 0, kPassBandH, kText);
}
}  // namespace

int draw_pass_band(const Surface &s, int16_t x, int16_t y, int16_t w, int32_t value, int32_t target, int32_t full, int prev_px) {
    if (full <= 0) full = 1;
    auto at = [&](int32_t v) { return static_cast<int16_t>(x + math::mul_div_i32(w, v, full)); };
    int16_t lo = at(math::mul_div_i32(target, 95, 100)), hi = at(math::mul_div_i32(target, 105, 100));
    if (hi - lo < kPassBandMinW - 1) {   // a low target on a wide scale: keep the window visible, centred on the target
        const int16_t mid = at(target);
        lo = static_cast<int16_t>(mid - (kPassBandMinW - 1) / 2);
        hi = static_cast<int16_t>(lo + kPassBandMinW - 1);
    }
    int16_t np = at(value < 0 ? 0 : value);
    if (np < x) np = x;
    if (np > x + w - 2) np = static_cast<int16_t>(x + w - 2);
    if (prev_px < 0) {
        gauge_cols(s, x, y, w, lo, hi, x, static_cast<int16_t>(x + w));
    } else if (prev_px == np) {
        return np;
    } else {
        gauge_cols(s, x, y, w, lo, hi, static_cast<int16_t>(prev_px), static_cast<int16_t>(prev_px + 2));
    }
    gfx_fill(s, np, y, 2, kPassBandH, kText);
    return np;
}

RangeGeom range_geom(int16_t x, int16_t w, int32_t value, int32_t set, int32_t lo, int32_t hi, int32_t full) {
    if (full <= 0) full = 1;
    auto at = [&](int32_t v) { return static_cast<int16_t>(x + math::mul_div_i32(w, std::max<int32_t>(v, 0), full)); };
    RangeGeom g;
    g.lo = std::clamp<int16_t>(at(lo), x, static_cast<int16_t>(x + w - 2));
    g.hi = std::clamp<int16_t>(at(hi), static_cast<int16_t>(g.lo + 1), static_cast<int16_t>(x + w - 1));
    g.mark = std::clamp<int16_t>(at(set), static_cast<int16_t>(x + 2), static_cast<int16_t>(x + w - 4));   // the 6 px cap stays on the strip
    g.needle = std::clamp<int16_t>(at(value), x, static_cast<int16_t>(x + w - 2));
    return g;
}

RangePos draw_range_gauge(const Surface &s, int16_t x, int16_t y, int16_t w, int32_t value, int32_t set, int32_t lo, int32_t hi,
                          int32_t full, RangePos prev) {
    const RangeGeom g = range_geom(x, w, value, set, lo, hi, full);
    if (prev.needle < 0 || prev.mark < 0) {
        range_cols(s, x, y, w, g, x, static_cast<int16_t>(x + w));
    } else {
        if (prev.mark != g.mark) {   // the old marker's columns and the new one's (the tint or track shows again where it was)
            range_cols(s, x, y, w, g, static_cast<int16_t>(prev.mark - 2), static_cast<int16_t>(prev.mark + 4));
            range_cols(s, x, y, w, g, static_cast<int16_t>(g.mark - 2), static_cast<int16_t>(g.mark + 4));
        }
        if (prev.needle != g.needle) {
            range_cols(s, x, y, w, g, prev.needle, static_cast<int16_t>(prev.needle + 2));
            range_cols(s, x, y, w, g, g.needle, static_cast<int16_t>(g.needle + 2));
        }
    }
    return {g.mark, g.needle};
}

// ---- HeroReadout ---------------------------------------------------------------------------------------------------------
void draw_hero(const Surface &s, Field &digits, const HeroSpec &h, std::string_view value, bool full) {
    const Font &f = *h.digits;
    if (full) {
        digits.init(s, f, static_cast<int16_t>(h.x - h.chars * f.adv), h.y, h.chars, Align::Right, h.color, kBg);
        if (!h.unit.empty())
            gfx_text(s, kMid, static_cast<int16_t>(h.x + 3), top_for_baseline(kMid, static_cast<int16_t>(h.y + f.ascent)), h.unit, h.color, kBg);
    }
    digits.draw(value);
}

}  // namespace k2::kit
