// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/kit.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui_viewport.hpp"

namespace k2::kit {

namespace {
constexpr int kTailMaxPx = 112;    // a value never takes more than 14 cells
constexpr int16_t kChevW = 6, kChevH = 9;

const Font &font_or_small(const Font *f) { return f ? *f : kSmall; }

void outline(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int16_t t, uint16_t color) {
    gfx_fill(s, x, y, w, t, color);
    gfx_fill(s, x, static_cast<int16_t>(y + h - t), w, t, color);
    gfx_fill(s, x, static_cast<int16_t>(y + t), t, static_cast<int16_t>(h - 2 * t), color);
    gfx_fill(s, static_cast<int16_t>(x + w - t), static_cast<int16_t>(y + t), t, static_cast<int16_t>(h - 2 * t), color);
}

// Text centred in a box (x, w) on the line whose top is `y`.
void centred_text(const Surface &s, const Font &f, int16_t x, int16_t w, int16_t y, std::string_view text, uint16_t fg, uint16_t bg) {
    char buf[kFitBuf];
    const std::string_view t = fit_text(f, text, w, buf);
    gfx_text(s, f, static_cast<int16_t>(x + (w - text_width(f, t)) / 2), y, t, fg, bg);
}

// A box of the panel: what a fill leaves to a glyph, a rule or a bracket that is drawn over it.
struct Box {
    int16_t x, y, w, h;
};

// Paints `r` in `color` except where `holes` go. A glyph or a rule that is drawn afterwards owns its box, so no pixel is
// streamed twice (the panel bus is the cost of a redraw). Holes are clipped to `r` and may touch; at most kMaxHoles.
constexpr int kMaxHoles = 6;
void fill_except(const Surface &s, Box r, const Box *holes, int n, uint16_t color) {
    if (ContentViewport::stretched()) n = 0;   // a stretched page: a glyph no longer covers its hole, so the glyph paints over a full fill
    Box h[kMaxHoles];
    int16_t ys[2 * kMaxHoles + 2];
    int m = 0, k = 0;
    ys[k++] = r.y;
    ys[k++] = static_cast<int16_t>(r.y + r.h);
    for (int i = 0; i < n && m < kMaxHoles; i++) {
        const int x0 = holes[i].x > r.x ? holes[i].x : r.x, y0 = holes[i].y > r.y ? holes[i].y : r.y;
        const int x1 = holes[i].x + holes[i].w < r.x + r.w ? holes[i].x + holes[i].w : r.x + r.w;
        const int y1 = holes[i].y + holes[i].h < r.y + r.h ? holes[i].y + holes[i].h : r.y + r.h;
        if (x1 <= x0 || y1 <= y0) continue;
        h[m++] = {static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
        ys[k++] = static_cast<int16_t>(y0);
        ys[k++] = static_cast<int16_t>(y1);
    }
    for (int i = 1; i < k; i++)   // sorted band edges
        for (int j = i; j > 0 && ys[j] < ys[j - 1]; j--) { const int16_t t = ys[j]; ys[j] = ys[j - 1]; ys[j - 1] = t; }
    for (int b = 0; b + 1 < k; b++) {
        const int16_t y0 = ys[b], y1 = ys[b + 1];
        if (y1 <= y0) continue;
        int16_t xs[kMaxHoles][2];   // the holes crossing this band, left to right
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

// A labelled verdict cell's text layout: the font by cell height, the fitted text and its centred line box.
struct LabelLayout {
    const Font *font;
    std::string_view text;
    Box hole;
};
LabelLayout label_layout(int16_t x, int16_t y, int16_t w, int16_t h, std::string_view label, char (&buf)[kFitBuf]) {
    const Font &f = h >= 22 ? kSmall : kLabelFont;
    const std::string_view t = fit_text(f, label, w, buf);
    const int16_t tw = static_cast<int16_t>(text_width(f, t));
    return {&f, t, {static_cast<int16_t>(x + (w - tw) / 2), static_cast<int16_t>(y + (h - f.height) / 2), tw, f.height}};
}

// The strike of a struck text drawn with font `f` at (tx, ty) and tw wide: one pixel row through the middle of the x-height, the
// text width plus 1 px each side, clipped to the columns [cx, cx + cw). It is painted over the text, after it.
Box strike_line(const Font &f, int16_t tx, int16_t ty, int16_t tw, int16_t cx, int16_t cw) {
    const Glyph *gx = find_glyph(f, 'x');
    const int xh = gx ? gx->top : f.ascent * 2 / 3;
    int16_t x0 = static_cast<int16_t>(tx - 1), x1 = static_cast<int16_t>(tx + tw + 1);
    if (x0 < cx) x0 = cx;
    if (x1 > cx + cw) x1 = static_cast<int16_t>(cx + cw);
    return {x0, static_cast<int16_t>(ty + f.ascent - 1 - xh / 2), static_cast<int16_t>(x1 > x0 ? x1 - x0 : 0), 1};
}
}  // namespace

int strike_px(Verdict v, std::string_view label, int16_t w, int16_t h) {
    if (v != Verdict::Skip || label.empty()) return 0;
    char buf[kFitBuf];
    const LabelLayout l = label_layout(0, 0, w, h, label, buf);
    return l.text.empty() ? 0 : strike_line(*l.font, l.hole.x, l.hole.y, l.hole.w, 0, w).w;
}

void draw_struck_text(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text, uint16_t ink, uint16_t bg) {
    gfx_text(s, f, x, y, text, ink, bg);
    if (text.empty()) return;
    const Box k = strike_line(f, x, y, static_cast<int16_t>(text_width(f, text)), 0, s.w);
    if (k.w > 0) gfx_fill(s, k.x, k.y, k.w, k.h, ink);
}

// ---- VerdictCell --------------------------------------------------------------------------------------------------------
CellRect run_dot_box(const CellRect &cell) {
    return {static_cast<int16_t>(cell.x + cell.w - kRunDotInsetX - kRunDotSize), static_cast<int16_t>(cell.y + kRunDotInsetY), kRunDotSize, kRunDotSize};
}

void draw_run_dot(const Surface &s, const CellRect &cell, bool on, uint16_t under) {
    const CellRect d = run_dot_box(cell);
    gfx_fill(s, d.x, d.y, d.w, d.h, on ? verdict_style(Verdict::Running).dot : under);
}

// Every pixel of the cell is streamed once: the fill leaves a hole for the label or the glyph. The exceptions are a Skip
// label's strike, a 1 px line painted over the text (strike_px), and a Running label's dot over the cell's corner (dot_px).
void draw_verdict(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, Verdict v, std::string_view label,
                  uint16_t under) {
    const VerdictStyle st = verdict_style(v);
    const uint16_t base = st.filled ? st.fill : under;
    Box inner{x, y, w, h};
    if (!label.empty()) {
        char buf[kFitBuf];
        const LabelLayout lay = label_layout(x, y, w, h, label, buf);
        const Box hole = lay.hole;
        if (v == Verdict::Partial) {   // the shape cue: a bar along the bottom edge, so a partial never reads as a pass in grey
            inner.h = static_cast<int16_t>(inner.h - kPartialBarH);
            gfx_fill(s, x, static_cast<int16_t>(y + h - kPartialBarH), w, kPartialBarH, st.ink);
        }
        fill_except(s, inner, &hole, 1, base);
        gfx_text(s, *lay.font, hole.x, hole.y, lay.text, st.ink, base);
        if (v == Verdict::Skip && !lay.text.empty()) {
            const Box k = strike_line(*lay.font, hole.x, hole.y, hole.w, x, w);
            gfx_fill(s, k.x, k.y, k.w, k.h, st.ink);
        }
        if (st.dotted) draw_run_dot(s, {x, y, w, h}, true);   // over the cell's corner, after the text
        return;
    }
    // No label: the state's glyph, centred.
    int sz = (w < h ? w : h) - 4;
    if (sz > 12) sz = 12;
    const int16_t gx = static_cast<int16_t>(x + (w - sz) / 2), gy = static_cast<int16_t>(y + (h - sz) / 2);
    Box hole{gx, gy, static_cast<int16_t>(sz), static_cast<int16_t>(sz)};   // the check and the cross paint their whole square
    switch (v) {
    case Verdict::None:
    case Verdict::Skip:
    case Verdict::Stopped: hole = {gx, static_cast<int16_t>(gy + sz / 2 - 1), static_cast<int16_t>(sz), 2}; break;
    case Verdict::Pending: hole = {static_cast<int16_t>(gx + sz / 2 - 1), static_cast<int16_t>(gy + sz / 2 - 1), 2, 2}; break;
    case Verdict::Running: {   // the dot itself, centred (a cell too small for it shows the pending dot)
        const int16_t d = sz >= kRunDotSize ? kRunDotSize : 2;
        hole = {static_cast<int16_t>(x + (w - d) / 2), static_cast<int16_t>(y + (h - d) / 2), d, d};
        break;
    }
    case Verdict::Pass:
    case Verdict::Fail: break;
    case Verdict::Partial: hole = {gx, static_cast<int16_t>(gy + sz / 2 - 3), static_cast<int16_t>(sz), 6}; break;   // the tilde's strip
    }
    fill_except(s, inner, &hole, sz < 4 ? 0 : 1, base);
    if (sz < 4) return;
    switch (v) {
    case Verdict::Partial: {
        // A tilde: four columns of 2 px bars stepping mid, up, down, mid inside the 6 px strip, each column painted once.
        const int16_t c = static_cast<int16_t>(gy + sz / 2 - 1), q = static_cast<int16_t>(sz / 4);
        static constexpr int8_t kStep[4] = {0, -2, 2, 0};
        for (int i = 0; i < 4; i++) {
            const int16_t cx = static_cast<int16_t>(gx + i * q), cw = i == 3 ? static_cast<int16_t>(sz - 3 * q) : q;
            const int16_t above = static_cast<int16_t>(2 + kStep[i]), below = static_cast<int16_t>(2 - kStep[i]);
            if (above) gfx_fill(s, cx, static_cast<int16_t>(c - 2), cw, above, base);
            gfx_fill(s, cx, static_cast<int16_t>(c + kStep[i]), cw, 2, st.ink);
            if (below) gfx_fill(s, cx, static_cast<int16_t>(c + kStep[i] + 2), cw, below, base);
        }
        break;
    }
    case Verdict::Pass:  draw_mark(s, gx, gy, sz, true, st.ink, base); break;
    case Verdict::Fail:  draw_mark(s, gx, gy, sz, false, st.ink, base); break;
    case Verdict::None:
    case Verdict::Skip:
    case Verdict::Stopped: gfx_fill(s, gx, static_cast<int16_t>(gy + sz / 2 - 1), static_cast<int16_t>(sz), 2, st.ink); break;
    case Verdict::Pending: gfx_fill(s, static_cast<int16_t>(gx + sz / 2 - 1), static_cast<int16_t>(gy + sz / 2 - 1), 2, 2, st.ink); break;
    case Verdict::Running: gfx_fill(s, hole.x, hole.y, hole.w, hole.h, hole.w == kRunDotSize ? st.dot : st.ink); break;
    }
}

// ---- TopBar -------------------------------------------------------------------------------------------------------------
void draw_progress_rule(const Surface &s, int step) {
    for (int i = 0; i < kProgSegs; i++)
        gfx_fill(s, static_cast<int16_t>(i * kProgPitch), kProgY, kProgSegW, kProgH, i < step ? kFocus : kRule);
}

void draw_top_bar(const Surface &s, std::string_view title, int step, std::string_view badge) {
    char buf[40];
    size_t n = 0;
    if (step >= 1 && step <= kProgSegs) {
        buf[n++] = static_cast<char>('0' + step);
        buf[n++] = '/';
        buf[n++] = static_cast<char>('0' + kProgSegs);
        buf[n++] = ' ';
    }
    for (size_t i = 0; i < title.size() && n < sizeof(buf); i++) buf[n++] = title[i];
    draw_status_left(s, false, Icon::Bolt, std::string_view(buf, n), 132, badge);
    if (step > 0) draw_progress_rule(s, step);
    else gfx_fill(s, 0, kProgY + 1, s.w, kProgH - 1, kBg);   // a plain bar over a wizard bar (Review -> Running): the rule's 2nd row must go (the hairline above was repainted)
}

// ---- RowList -------------------------------------------------------------------------------------------------------------
void draw_drill(const Surface &s, int16_t right, int16_t cy, uint16_t color) {
    // A 2 px stroke chevron, 6 wide and 9 tall: the arm rows step one px per row towards the point.
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
    switch (r.marker) {
    case Marker::None: break;
    case Marker::Radio:
        // A square, no circle to rasterise: the pick is a solid block, the others a hollow 1 px frame. The whole 12 px
        // square is repainted on every state, so a half-drawn one cannot exist; solid versus hollow is the shape cue.
        fill(0, 0, m.w, m.h, col);
        if (!on) fill(1, 1, static_cast<int16_t>(m.w - 2), static_cast<int16_t>(m.h - 2), bg);
        break;
    case Marker::Box3:
    case Marker::Check:
        fill(1, 1, static_cast<int16_t>(m.w - 2), static_cast<int16_t>(m.h - 2), bg);
        fill(0, 0, m.w, 1, col);
        fill(0, static_cast<int16_t>(m.h - 1), m.w, 1, col);
        fill(0, 1, 1, static_cast<int16_t>(m.h - 2), col);
        fill(static_cast<int16_t>(m.w - 1), 1, 1, static_cast<int16_t>(m.h - 2), col);
        if (r.marker == Marker::Box3) {
            if (r.state >= 2) fill(3, 3, 6, 6, col);
            else if (r.state == 1) fill(3, 5, 6, 2, col);
        } else if (on) {
            draw_mark(s, static_cast<int16_t>(x + 2), static_cast<int16_t>(y + 2), 8, true, col, bg);
        }
        break;
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
    list_.set_row_bg(kBg, kFocusT, kRule);
    list_.set_full_lane(true);
    list_.set_highlight(sel_);
    if (rebind) list_.set_offset(0);
    extra_ = -1;
}

void RowList::invalidate() {
    list_.invalidate();
    extra_ = -1;
}

uint32_t RowList::row_sig_cb(void *ctx, int index) {
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

void RowList::draw_row_cb(void *ctx, int index, const Rect &r, bool hi, Field &) {
    auto *self = static_cast<RowList *>(ctx);
    const Surface &s = self->surf_;
    RowSpec sp;
    self->fn_(self->ctx_, index, hi, sp);
    const int16_t y = r.y;
    const uint16_t bg = hi ? kFocusT : kBg;
    if (hi) gfx_fill(s, 0, static_cast<int16_t>(y + 1), 3, static_cast<int16_t>(r.h - 1), kFocus);
    if (sp.marker == Marker::Icon && sp.marker_color == 0 && hi) sp.marker_color = kFocus;
    draw_marker(s, kMarkX, static_cast<int16_t>(y + 7), sp, bg);

    const int16_t ty = static_cast<int16_t>(y + row_text_y(kRowH, kSmall.height));
    // A yielding tail (a hint, not a value) is left out when the text would not fit beside it; with nothing at the right
    // end the text runs up to the scroll lane, so a 24-cell name (the longest the library stores) still fits whole.
    std::string_view tail = sp.tail;
    if (sp.tail_yields && !tail.empty() && !sp.chevron) {
        const int tw = sp.tail_tint ? pill_width(text_cells(tail)) : text_width(kSmall, tail);
        const int col2w = sp.col2.empty() ? 0 : text_width(kSmall, sp.col2) + kTailGap;
        if (kTextX + text_width(kSmall, sp.text) + col2w + kTailGap + tw > kTailX) tail = {};
    }
    int right = sp.chevron || !tail.empty() ? kTailX : s.w - ListView::kBarW;
    char buf[kFitBuf], tbuf[kFitBuf];
    if (sp.chevron) {
        draw_drill(s, kTailX, static_cast<int16_t>(y + 1 + (kRowH - 1) / 2), sp.tail_color);
        right -= kChevW + kTailGap;
    } else if (!tail.empty() && sp.tail_tint) {
        const std::string_view v = fit_text(kLabelFont, tail, kTailMaxPx - 8, tbuf);
        const int pw = pill_width(static_cast<int>(v.size()));
        const int16_t px0 = static_cast<int16_t>(kTailX - pw);
        fill_round(s, px0, static_cast<int16_t>(y + 1 + (kRowH - 1 - kPillH) / 2), static_cast<int16_t>(pw), kPillH, 3, sp.tail_tint);
        gfx_text(s, kLabelFont, static_cast<int16_t>(px0 + 4), static_cast<int16_t>(y + 1 + (kRowH - 1 - kPillH) / 2 + 1), v,
                 sp.tail_color, sp.tail_tint);
        right -= pw + kTailGap;
    } else if (!tail.empty()) {
        const std::string_view v = fit_text(kSmall, tail, kTailMaxPx, tbuf);
        const int vw = text_width(kSmall, v);
        gfx_text(s, kSmall, static_cast<int16_t>(kTailX - vw), ty, v, sp.tail_color, bg);
        right -= vw + kTailGap;
    }
    if (!sp.col2.empty()) {
        const std::string_view c = fit_text(kSmall, sp.col2, right - kCol2X, tbuf);
        gfx_text(s, kSmall, kCol2X, ty, c, kDim, bg);
        right = kCol2X - 4;
    }
    gfx_text(s, kSmall, kTextX, ty,
             sp.data ? fit_data(kSmall, sp.text, right - kTextX, buf) : fit_text(kSmall, sp.text, right - kTextX, buf),
             sp.text_color, bg);
    if (hi && r.h > kRowH && !sp.note.empty())
        gfx_text(s, kLabelFont, kTextX, static_cast<int16_t>(y + kNoteDy), fit_text(kLabelFont, sp.note, kNoteMaxPx, buf), kDim, bg);
}

void RowList::draw(uint32_t now_ms) {
    list_.set_count(count_, false);
    list_.set_highlight(sel_);
    const ListState &st = list_.state();
    if (sel_ >= 0 && sel_ < count_) {
        int off = st.offset;
        if (sel_ < off) off = sel_;
        else if (sel_ >= off + st.visible) off = sel_ - st.visible + 1;
        list_.set_offset(off);
    }
    // The focused row opens only when it has a note. A change of that re-lays every row below it, so the list repaints.
    int16_t want = 0;
    if (sel_ >= 0 && sel_ < count_) {
        RowSpec probe;
        fn_(ctx_, sel_, true, probe);
        if (!probe.note.empty()) want = kFocusExtra;
    }
    if (want != extra_) {
        const bool known = extra_ >= 0;
        list_.set_focus_extra(want);
        if (known) list_.invalidate();
        if (want < kFocusExtra)   // the strip the open row used to cover belongs to nobody now
            gfx_fill(surf_, 0, static_cast<int16_t>(y_ + st.visible * kRowH + want), surf_.w,
                     static_cast<int16_t>(kFocusExtra - want), kBg);
        extra_ = want;
    }
    list_.draw(now_ms);
}

// ---- KvTable -------------------------------------------------------------------------------------------------------------
void kv_static(const Surface &s, const KvLayout &l, const char *const *labels, int n) {
    const Font &f = font_or_small(l.label_font);
    for (int i = 0; i < n; i++) {
        const int16_t y = kv_row_y(l, i);
        gfx_fill(s, 0, y, s.w, 1, kRule);
        gfx_text(s, f, l.label_x, static_cast<int16_t>(y + row_text_y(l.pitch, f.height)), labels[i], kDim, kBg);
    }
    gfx_fill(s, 0, kv_row_y(l, n), s.w, 1, kRule);
}

void kv_cells(const Surface &s, const KvLayout &l, Field *cells, int n, const uint8_t *chars) {
    const Font &f = font_or_small(l.value_font);
    for (int i = 0; i < n; i++) {
        const uint8_t w = chars && chars[i] < l.value_chars ? chars[i] : l.value_chars;
        const int16_t x = l.align == Align::Right ? static_cast<int16_t>(l.value_x + (l.value_chars - w) * f.adv) : l.value_x;
        cells[i].init(s, f, x, static_cast<int16_t>(kv_row_y(l, i) + row_text_y(l.pitch, f.height)), w, l.align, kText, kBg);
    }
}

void kv_row(const Surface &s, const KvLayout &l, int16_t y, std::string_view key, std::string_view value, uint16_t value_color,
            bool ours K2_TRUNC_DEF) {
    const Font &kf = font_or_small(l.label_font), &vf = font_or_small(l.value_font);
    char buf[kFitBuf];
    gfx_fill(s, 0, y, s.w, 1, kRule);
    gfx_text(s, kf, l.label_x, static_cast<int16_t>(y + row_text_y(l.pitch, kf.height)), key, kDim, kBg);
    const int box = l.value_chars * vf.adv;
    gfx_text(s, vf, l.value_x, static_cast<int16_t>(y + row_text_y(l.pitch, vf.height)),
             ours ? fit_text(vf, value, box, buf K2_TRUNC_ARG) : fit_data(vf, value, box, buf K2_TRUNC_ARG), value_color, kBg);
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
    const uint16_t fill = error ? kError : kCaution;
    gfx_fill(s, 0, y, s.w, kAlertH, fill);
    draw_warn_triangle(s, 8, static_cast<int16_t>(y + 8), 20, kBg, fill);
    char buf[kFitBuf];
    gfx_text(s, kMid, 36, static_cast<int16_t>(y + (kAlertH - kMid.height) / 2), fit_text(kMid, word, s.w - 36 - 4, buf), kBg, fill);
}

// ---- ActionCell ----------------------------------------------------------------------------------------------------------
void ActionCell::draw_idle(std::string_view verb, std::string_view hint) {
    uint32_t sig = mix_u32(mix_init(), 0);
    sig = mix(mix(sig, verb), hint);
    e_.paint(sig, [&] {
        bar_px_ = -1;
        gfx_fill(surf_, 0, y_, surf_.w, h_, kFocusT);
        outline(surf_, 0, y_, surf_.w, h_, 2, kFocus);
        const int content = kMid.height + (hint.empty() ? 0 : 6 + kLabelFont.height);
        const int16_t top = static_cast<int16_t>(y_ + (h_ - content) / 2);
        centred_text(surf_, kMid, 4, static_cast<int16_t>(surf_.w - 8), top, verb, kText, kFocusT);
        if (hint.empty()) return;
        char buf[kFitBuf];
        constexpr int16_t kTri = 11;
        const std::string_view t = fit_text(kLabelFont, hint, surf_.w - 40, buf);
        const int tw = kTri + 4 + text_width(kLabelFont, t);
        const int16_t x0 = static_cast<int16_t>((surf_.w - tw) / 2);
        const int16_t ly = static_cast<int16_t>(top + kMid.height + 6);
        draw_warn_triangle(surf_, x0, static_cast<int16_t>(ly + (kLabelFont.height - kTri) / 2), kTri, kCaution, kFocusT);
        gfx_text(surf_, kLabelFont, static_cast<int16_t>(x0 + kTri + 4), ly, t, kCaution, kFocusT);
    });
}

void ActionCell::draw_armed(std::string_view word, std::string_view sub, int left_permille) {
    uint32_t sig = mix_u32(mix_init(), 1);
    sig = mix(mix(sig, word), sub);
    e_.paint(sig, [&] {
        bar_px_ = -1;
        gfx_fill(surf_, 0, y_, surf_.w, h_, kCaution);
        const int content = kMid.height + 4 + kLabelFont.height;
        const int16_t top = static_cast<int16_t>(y_ + (h_ - kBarH - content) / 2);
        centred_text(surf_, kMid, 4, static_cast<int16_t>(surf_.w - 8), top, word, kBg, kCaution);
        centred_text(surf_, kLabelFont, 4, static_cast<int16_t>(surf_.w - 8), static_cast<int16_t>(top + kMid.height + 4), sub, kBg, kCaution);
    });
    if (left_permille < 0) left_permille = 0;
    if (left_permille > 1000) left_permille = 1000;
    const int16_t span = static_cast<int16_t>(surf_.w - 1);
    const int16_t px = static_cast<int16_t>((span * left_permille + 500) / 1000);
    if (px == bar_px_) return;
    const int16_t by = static_cast<int16_t>(y_ + h_ - kBarH);
    if (bar_px_ < 0) gfx_fill(surf_, 0, by, px, kBarH, kBg);
    else if (px < bar_px_) gfx_fill(surf_, px, by, static_cast<int16_t>(bar_px_ - px), kBarH, kCaution);
    else gfx_fill(surf_, bar_px_, by, static_cast<int16_t>(px - bar_px_), kBarH, kBg);
    bar_px_ = px;
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

// ---- InfoBlock -----------------------------------------------------------------------------------------------------------
void draw_info_row(const Surface &s, int16_t y, std::string_view tag, std::string_view value, uint16_t value_color) {
    char buf[kFitBuf];
    const int16_t ty = static_cast<int16_t>(y + (kInfoPitch - kSmall.height) / 2);
    gfx_fill(s, 0, y, s.w, kInfoPitch, kBg);
    gfx_text(s, kSmall, kInfoTagX, ty, tag, kDim, kBg);
    gfx_text(s, kSmall, kInfoValueX, ty, fit_data(kSmall, value, s.w - kInfoValueX - 4, buf), value_color, kBg);
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
struct Bracket {
    Box text{};
    Box segs[4]{};
    int nseg = 0;
};
Bracket bracket_of(const Surface &s, int16_t y, int n, int i, int j, std::string_view cur) {
    Bracket b;
    const int16_t gx0 = static_cast<int16_t>(cell_x0(s, n, i) + 2), gx1 = static_cast<int16_t>(cell_x0(s, n, j + 1) - 4);
    const int16_t gx = static_cast<int16_t>((gx0 + gx1) / 2), by = static_cast<int16_t>(y + 31);
    const int tw = text_width(kLabelFont, cur);
    b.text = {static_cast<int16_t>(gx - tw / 2), static_cast<int16_t>(y + 25), static_cast<int16_t>(tw), kLabelFont.height};
    const int half = tw / 2 + 4;
    if (gx1 - gx0 > half * 2 + 8) {
        b.segs[0] = {gx0, by, static_cast<int16_t>(gx - half - gx0), 1};
        b.segs[1] = {static_cast<int16_t>(gx + half), by, static_cast<int16_t>(gx1 - gx - half + 1), 1};
        b.segs[2] = {gx0, static_cast<int16_t>(by - 3), 1, 3};
        b.segs[3] = {gx1, static_cast<int16_t>(by - 3), 1, 3};
        b.nseg = 4;
    }
    return b;
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
    const std::string_view lab = fit_text(kLabelFont, row_label, kResultLabelW - 8, buf);
    const int16_t lw = static_cast<int16_t>(text_width(kLabelFont, lab));
    const Box lab_box{static_cast<int16_t>(kResultLabelW - 5 - lw), static_cast<int16_t>(y + (h - kLabelFont.height) / 2), lw, kLabelFont.height};
    if (focus) {
        gfx_fill(s, 0, y, 3, body, kFocus);
        fill_except(s, {3, y, static_cast<int16_t>(kResultLabelW - 3), body}, &lab_box, 1, kFocusT);
    } else {
        fill_except(s, {0, y, kResultLabelW, body}, &lab_box, 1, kBg);
    }
    gfx_text(s, kLabelFont, lab_box.x, lab_box.y, lab, focus ? kFocus : kDim, focus ? kFocusT : kBg);
    gfx_fill(s, kResultLabelW, y, 1, body, kRule);
    if (n <= 0) {
        gfx_fill(s, kResultLabelW + 1, y, static_cast<int16_t>(s.w - kResultLabelW - 1), body, kBg);
        return;
    }
    bool two = false;
    for (int i = 0; i < n; i++) two = two || !cells[i].current.empty();
    const int16_t vh = two ? 24 : body;
    for (int i = 0; i < n; i++) {
        const CellRect cr = verdict_cell_rect(s, y, vh, n, i);
        draw_verdict(s, cr.x, cr.y, cr.w, cr.h, cells[i].verdict, cells[i].label, kBg);
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
            const Bracket b = bracket_of(s, y, n, i, j, cells[i].current);
            Box holes[5];
            holes[0] = b.text;
            for (int k = 0; k < b.nseg; k++) holes[1 + k] = b.segs[k];
            fill_except(s, {zx, zy, zw, zh}, holes, 1 + b.nseg, kBg);
        }
        i = j + 1;
    }
    for (int i = 0; i < n;) {
        int j = i;
        while (j + 1 < n && cells[j + 1].current == cells[i].current) j++;
        if (!cells[i].current.empty()) {
            const Bracket b = bracket_of(s, y, n, i, j, cells[i].current);
            gfx_text(s, kLabelFont, b.text.x, b.text.y, cells[i].current, focus ? kFocus : kDim, kBg);
            for (int k = 0; k < b.nseg; k++) gfx_fill(s, b.segs[k].x, b.segs[k].y, b.segs[k].w, b.segs[k].h, kDim);
        }
        i = j + 1;
    }
}

void ResultGrid::init(const Surface &s, int16_t y, int16_t bottom, RowFn fn, void *ctx) {
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
// A row's shape: everything but the cells' verdicts. A different shape repaints the whole row.
uint32_t row_shape(const ResultRowSpec &r, int16_t y, int16_t h) {
    uint32_t k = mix_u32(mix_init(), static_cast<uint32_t>(r.n) | (static_cast<uint32_t>(r.section) << 4) |
                                         (static_cast<uint32_t>(r.focus) << 5) | (static_cast<uint32_t>(y) << 8) |
                                         (static_cast<uint32_t>(h) << 20));
    k = mix(k, r.label);
    for (int i = 0; i < r.n; i++) {
        k = mix(k, r.cells[i].label);
        k = mix(k, r.cells[i].current);
    }
    return k;
}
// A row's core: where it is and which cells it has (the shape without the height and the cells' currents). A Running dot sits on
// the top edge of its cell, so a row whose currents alone changed still has the dot's cell where the rows put it.
uint32_t row_core(const ResultRowSpec &r, int16_t y) {
    uint32_t k = mix_u32(mix_init(), static_cast<uint32_t>(r.n) | (static_cast<uint32_t>(r.section) << 4) |
                                         (static_cast<uint32_t>(r.focus) << 5) | (static_cast<uint32_t>(y) << 8));
    k = mix(k, r.label);
    for (int i = 0; i < r.n; i++) k = mix(k, r.cells[i].label);
    return k;
}
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
        px += strike_px(r.cells[k].verdict, r.cells[k].label, verdict_cell_rect(s, 0, vh, r.n, k).w, vh);
        px += dot_px(r.cells[k].verdict, !r.cells[k].label.empty());
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
}  // namespace

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
            fn_(ctx_, i, r);
            if (i == first_) r.section = false;
            sum += result_row_h(r);
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
        fn_(ctx_, i, r);
        if (i == first_) r.section = false;
        const int16_t h = result_row_h(r);
        if (yy + h > bottom_) break;
        if (i == index) return yy;
        yy = static_cast<int16_t>(yy + h);
    }
    return -1;
}

namespace {
// Whether the dot box of a labelled cell overlaps the ink box of one of its glyphs (then wiping the dot would wipe label
// pixels, so the caller repaints the cell instead). The ink boxes are exact: in a grid cell the line box starts 4 px down and
// the ink 7, so the dot at the top edge clears it and the cheap wipe applies.
bool dot_hits_label(const CellRect &cr, std::string_view label) {
    char buf[kFitBuf];
    const LabelLayout lay = label_layout(cr.x, cr.y, cr.w, cr.h, label, buf);
    const CellRect d = run_dot_box(cr);
    const Font &f = *lay.font;
    int pen = lay.hole.x;
    for (const char c : lay.text) {
        const Glyph *g = find_glyph(f, static_cast<uint8_t>(c));
        if (g) {
            const int gx = pen + g->left, gy = lay.hole.y + f.ascent - g->top;
            if (d.x < gx + g->w && d.x + d.w > gx && d.y < gy + g->h && d.y + d.h > gy) return true;
        }
        pen += f.adv;
    }
    return false;
}
}  // namespace

// The grid's dot bookkeeping after a paint: the cell was painted (so a dot that was on it is gone), and a Running one has its
// dot again (draw_verdict paints it).
void ResultGrid::note_dot(const CellRect &cell, bool running, std::string_view label, int slot, int k) {
    if (dot_on_ && dot_cell_.x == cell.x && dot_cell_.y == cell.y) dot_on_ = false;
    if (!running || label.empty()) return;
    dot_on_ = true; dot_cell_ = cell; dot_slot_ = slot; dot_k_ = k;
    dot_hit_ = dot_hits_label(cell, label);
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
        fn_(ctx_, i, r);
        if (i == first_) r.section = false;
        const int16_t h = result_row_h(r);
        if (yy + h > bottom_) break;
        if (valid_[slot] && core_[slot] == row_core(r, yy)) {
            const int16_t cy = static_cast<int16_t>(yy + (r.section ? kResultSectionH : 0)), vh = verdict_part_h(r, h);
            for (int k = 0; k < r.n; k++) {
                if (r.cells[k].verdict != Verdict::Running || r.cells[k].label.empty()) continue;
                want = verdict_cell_rect(surf_, cy, vh, r.n, k);
                want_hit = dot_hits_label(want, r.cells[k].label);
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
            draw_run_dot(surf_, dot_cell_, false);
            spent += kRunDotPx;
            if (valid_[dot_slot_] && ((vd_[dot_slot_] >> (3 * dot_k_)) & 7u) == run) set_vd(dot_slot_, dot_k_, pend);
        }
        dot_on_ = false;
    }
    // 3. The new dot arrives.
    if (have && !dot_on_) {
        draw_run_dot(surf_, want, true);
        spent += kRunDotPx;
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
    int16_t yy = y_;
    int slot = 0;
    for (int pass = prefer >= first_ ? 0 : 1; pass < 2; pass++) {
        yy = y_;
        slot = 0;
        for (int i = first_; i < count_ && slot < kMaxSlots; i++, slot++) {
            ResultRowSpec r;
            fn_(ctx_, i, r);
            if (i == first_) r.section = false;
            const int16_t h = result_row_h(r);
            if (yy + h > bottom_) break;
            if ((i == prefer) == (pass == 0)) {
                const uint32_t sh = row_shape(r, yy, h);
                const uint16_t vd = row_verdicts(r);
                if (!valid_[slot] || sig_[slot] != sh) {
                    const int cost = surf_.w * h + row_overlay_px(surf_, r, h, ~0u);
                    if ((strict || spent > 0) && spent + cost > budget_px) {
                        pending_ = true;
                    } else {
                        draw_result_row(surf_, yy, h, r.label, r.cells, r.n, r.focus, r.section);
                        spent += cost;
                        sig_[slot] = sh; vd_[slot] = vd; core_[slot] = row_core(r, yy); valid_[slot] = true;
                        if (dot_on_ && dot_cell_.y >= yy && dot_cell_.y < yy + h) dot_on_ = false;   // the row covers the whole width
                        const int16_t cy = static_cast<int16_t>(yy + (r.section ? kResultSectionH : 0)), vh = verdict_part_h(r, h);
                        for (int k = 0; k < r.n; k++)
                            if (r.cells[k].verdict == Verdict::Running)
                                note_dot(verdict_cell_rect(surf_, cy, vh, r.n, k), true, r.cells[k].label, slot, k);
                    }
                } else if (vd_[slot] != vd) {
                    const int16_t cy = static_cast<int16_t>(yy + (r.section ? kResultSectionH : 0)), vh = verdict_part_h(r, h);
                    const unsigned chg = changed_cells(vd_[slot], vd, r.n);
                    int cost = row_overlay_px(surf_, r, h, chg);
                    for (int k = 0; k < r.n; k++)
                        if ((chg >> k) & 1) cost += verdict_cell_rect(surf_, cy, vh, r.n, k).w * vh;
                    if ((strict || spent > 0) && spent + cost > budget_px) {
                        pending_ = true;
                    } else {
                        for (int k = 0; k < r.n; k++) {
                            if (!((chg >> k) & 1)) continue;
                            const CellRect cr = verdict_cell_rect(surf_, cy, vh, r.n, k);
                            draw_verdict(surf_, cr.x, cr.y, cr.w, cr.h, r.cells[k].verdict, r.cells[k].label, kBg);
                            note_dot(cr, r.cells[k].verdict == Verdict::Running, r.cells[k].label, slot, k);
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
        if ((strict || spent > 0) && spent + cost > budget_px) {
            pending_ = true;   // the blank under the last row waits too
        } else {
            if (cost) gfx_fill(surf_, 0, yy, surf_.w, static_cast<int16_t>(bottom_ - yy), kBg);
            if (dot_on_ && dot_cell_.y >= yy) dot_on_ = false;   // under the last row: wiped
            spent += cost;
            end_y_ = yy;
        }
    }
    return spent;
}

void ResultGrid::draw() { draw_some(1 << 30); }

// ---- PassBandGauge -------------------------------------------------------------------------------------------------------
namespace {
// Paints columns [c0, c1) of the gauge strip.
void gauge_cols(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t lo, int16_t hi, int16_t c0, int16_t c1) {
    if (c0 < x) c0 = x;
    if (c1 > x + w) c1 = static_cast<int16_t>(x + w);
    if (c1 <= c0) return;
    gfx_fill(s, c0, y, static_cast<int16_t>(c1 - c0), kPassBandH, kBg);
    gfx_fill(s, c0, static_cast<int16_t>(y + 4), static_cast<int16_t>(c1 - c0), 4, kRule);
    const int16_t a = c0 > lo ? c0 : lo, b = c1 < hi + 1 ? c1 : static_cast<int16_t>(hi + 1);
    if (b > a) {
        gfx_fill(s, a, static_cast<int16_t>(y + 2), static_cast<int16_t>(b - a), 8, kOkT);
        gfx_fill(s, a, static_cast<int16_t>(y + 4), static_cast<int16_t>(b - a), 4, kOk);
    }
}
}  // namespace

int draw_pass_band(const Surface &s, int16_t x, int16_t y, int16_t w, int32_t value, int32_t target, int32_t full, int prev_px) {
    if (full <= 0) full = 1;
    auto at = [&](int64_t v) { return static_cast<int16_t>(x + (static_cast<int64_t>(w) * v) / full); };
    int16_t lo = at(static_cast<int64_t>(target) * 95 / 100), hi = at(static_cast<int64_t>(target) * 105 / 100);
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

// ---- RangeGauge ----------------------------------------------------------------------------------------------------------
RangeGeom range_geom(int16_t, int16_t, int32_t, int32_t, int32_t, int32_t, int32_t) { return {0, 0, 0, 0}; }   // red
RangePos draw_range_gauge(const Surface &, int16_t, int16_t, int16_t, int32_t, int32_t, int32_t, int32_t, int32_t, RangePos) { return {}; }

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
