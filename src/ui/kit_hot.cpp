// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Hot-path UI kit painters placed in the zero-wait flash window (K2_HOT).
#include "k2/ui/kit.hpp"
#include "kit_internal.hpp"
#include "ui_widgets_internal.hpp"
#include "k2/core/hot.h"
#include "k2/drivers/lcd.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui_viewport.hpp"

namespace k2::kit {

namespace {
constexpr int kTailMaxPx = 112;
constexpr int16_t kChevW = 6;
constexpr int kMaxHoles = 6;

// Where a tail of `vw` px starts so it ends on kTailX. The landscape map stretches origins but not glyphs, so the logical
// kTailX - vw would end further left the wider the text: anchor on the mapped right edge instead (to within a source column).
int16_t tail_origin(const Surface &s, int16_t y, int vw) {
    const int plain = kTailX - vw;
    if (!ContentViewport::scaled()) return static_cast<int16_t>(plain);
    const int edge = ContentViewport::mapped_rect({static_cast<int16_t>(s.x + kTailX), static_cast<int16_t>(s.y + y), 1, 1}).x;
    const int col = ContentViewport::source_x(edge - vw);
    return static_cast<int16_t>(col < 0 ? plain : col - s.x);
}
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

// The PC-link icon in its box, upright on every edge (never turned with the text): one window write. Glass + Sand is the
// two-colour hourglass.
K2_HOT static void paint_link_icon(const Surface &s, Box b, pclink::State st, uint16_t ground, uint16_t ink, uint16_t sand) {
    constexpr int kN = kIconSize;
    if (!k2::detail::fits(s, b.x, b.y, kN, kN)) return;
    int r0 = 0, r1 = kN;
    if (!k2::detail::clip_box_to_band(s, b.x, b.y, kN, kN, r0, r1)) return;
    const uint16_t *shape = kIconBits[static_cast<int>(st == pclink::State::Power ? Icon::Socket
                                                       : st == pclink::State::Host ? Icon::Monitor : Icon::Glass)];
    const uint16_t *fill = kIconBits[static_cast<int>(Icon::Sand)];
    uint16_t line[kN];
    lcd::stream_begin(static_cast<uint16_t>(s.x + b.x), static_cast<uint16_t>(s.y + b.y + r0),
                      static_cast<uint16_t>(s.x + b.x + kN - 1), static_cast<uint16_t>(s.y + b.y + r1 - 1));
    for (int r = r0; r < r1; r++) {
        for (int c = 0; c < kN; c++) {
            const int sh = kN - 1 - c;
            line[c] = (st == pclink::State::Shell && (fill[r] >> sh & 1)) ? sand : (shape[r] >> sh & 1) ? ink : ground;
        }
        lcd::stream_pixels({line, static_cast<std::size_t>(kN)});
    }
    lcd::stream_end();
}

K2_HOT void draw_key_slot(const Surface &s, PanelEdge edge, TextDir dir, unsigned slot, const SlotLabel &lab, bool is_ok, bool down,
                         bool active) {
    if (s.w <= 0 || s.h <= 0) return;
    {
        uint16_t by = static_cast<uint16_t>(s.y), bh = static_cast<uint16_t>(s.h);
        if (!lcd::clip_to_band(by, bh)) return;
    }
    const bool column = panel_is_column(edge);
    Box body{0, 0, s.w, s.h};
    if (active) {
        switch (edge) {
        case PanelEdge::Left:
            gfx_fill(s, static_cast<int16_t>(s.w - 1), 0, 1, s.h, kAccent);
            body = {0, 0, static_cast<int16_t>(s.w - 1), s.h};
            break;
        case PanelEdge::Right:
            gfx_fill(s, 0, 0, 1, s.h, kAccent);
            body = {1, 0, static_cast<int16_t>(s.w - 1), s.h};
            break;
        case PanelEdge::Top:
            gfx_fill(s, 0, static_cast<int16_t>(s.h - 1), s.w, 1, kAccent);
            body = {0, 0, s.w, static_cast<int16_t>(s.h - 1)};
            break;
        case PanelEdge::Bottom:
            gfx_fill(s, 0, 0, s.w, 1, kAccent);
            body = {0, 1, s.w, static_cast<int16_t>(s.h - 1)};
            break;
        }
    }
    char text[24];
    const int along = column ? body.h : body.w;
    const LabelZone zone = label_zone(along, slot);
    const std::string_view shrt = lab.shrt;
    const bool armed = is_ok && shrt == "Confirm";
    const int tri = !down && (shrt == kUp || shrt == kDn) ? (shrt == kUp ? 1 : -1) : 0;
    const unsigned chars = tri ? 0 : label_text(text, lab, zone.max_chars);
    const std::string_view label(text, chars);
    const bool cap = is_ok && chars > 0 && !down && !lab.dim;   // a dim OK is unavailable: plain dim text, no cap
    const uint16_t ground = down ? kCap : (active ? kFocusFill : kBg2);
    const uint16_t cap_fill = armed ? kCaution : kCap;
    const uint16_t bg = cap ? (active ? cap_fill : kBar) : ground;
    const uint16_t idle_fg = active ? kOnFocusDim : kInk2;
    const uint16_t fg = down ? kOnCap : cap ? (active ? (armed ? kSpotInk : kOnCap) : kAccent)
                      : lab.dim ? kInk3 : shrt == "Stop" ? kError : idle_fg;
    Box holes[2];
    int nh = 0;
    int16_t tx = 0, ty = 0;
    const int16_t fh = static_cast<int16_t>(kLabelFont.height);
    const int16_t tw = static_cast<int16_t>(text_width(kLabelFont, label));
    if (chars > 0) {
        const int16_t a0 = static_cast<int16_t>(label_origin(zone, chars));
        tx = column ? static_cast<int16_t>(body.x + (body.w - fh) / 2) : a0;
        ty = column ? a0 : static_cast<int16_t>(body.y + (body.h - fh) / 2);
        if (cap) {
            const int16_t c0 = static_cast<int16_t>(zone.start - 2), cl = static_cast<int16_t>(zone.len + 4);
            const int16_t across = column ? body.w : body.h;
            const int16_t cap_across = active ? 13 : 14;
            const int16_t c1 = static_cast<int16_t>((column ? body.x : body.y) + (across - cap_across) / 2);
            holes[nh++] = column ? Box{c1, c0, cap_across, cl} : Box{c0, c1, cl, cap_across};
        } else {
            holes[nh++] = {tx, ty, column ? fh : tw, column ? tw : fh};
        }
    }
    Box tri_box{};
    if (tri) {
        const int16_t cx = static_cast<int16_t>(zone.start + zone.len / 2);
        tri_box = column ? Box{static_cast<int16_t>(body.x + (body.w - 9) / 2), static_cast<int16_t>(cx - 2), 9, 5}
                         : Box{static_cast<int16_t>(cx - 4), static_cast<int16_t>(body.y + (body.h - 5) / 2), 9, 5};
        holes[nh++] = tri_box;
    }
    fill_except(s, body, holes, nh, ground);
    if (cap) {
        const Box t{tx, ty, column ? fh : tw, column ? tw : fh};
        const Box &cb = holes[0];
        if (active) {
            fill_except(s, cb, &t, 1, cap_fill);
        } else {
            gfx_fill(s, cb.x, cb.y, cb.w, 1, kAccent);
            gfx_fill(s, cb.x, static_cast<int16_t>(cb.y + cb.h - 1), cb.w, 1, kAccent);
            gfx_fill(s, cb.x, static_cast<int16_t>(cb.y + 1), 1, static_cast<int16_t>(cb.h - 2), kAccent);
            gfx_fill(s, static_cast<int16_t>(cb.x + cb.w - 1), static_cast<int16_t>(cb.y + 1), 1,
                     static_cast<int16_t>(cb.h - 2), kAccent);
            const Box inner{static_cast<int16_t>(cb.x + 1), static_cast<int16_t>(cb.y + 1),
                            static_cast<int16_t>(cb.w - 2), static_cast<int16_t>(cb.h - 2)};
            fill_except(s, inner, &t, 1, kBar);
        }
    }
    if (chars > 0) {
        if (column) gfx_text_rot(s, kLabelFont, tx, ty, label, fg, bg, dir);
        else gfx_text(s, kLabelFont, tx, ty, label, fg, bg);
    }
    if (!tri) return;
    const bool up = tri > 0;
    const uint16_t ink = lab.dim ? kInk3 : (active ? kAccent : kInk2);
    for (int i = 0; i < 5; i++) {
        const int k = up ? i : 4 - i;
        const int16_t lo = static_cast<int16_t>(4 - k), span = static_cast<int16_t>(2 * k + 1);
        const int16_t ly = static_cast<int16_t>(tri_box.y + i);
        gfx_fill(s, tri_box.x, ly, lo, 1, ground);
        gfx_fill(s, static_cast<int16_t>(tri_box.x + lo), ly, span, 1, ink);
        gfx_fill(s, static_cast<int16_t>(tri_box.x + lo + span), ly, lo, 1, ground);
    }
}

K2_HOT void draw_opp_slot(const Surface &s, PanelEdge opp_edge, TextDir dir, unsigned slot, const OppCell &cell) {
    if (s.w <= 0 || s.h <= 0) return;
    {
        uint16_t by = static_cast<uint16_t>(s.y), bh = static_cast<uint16_t>(s.h);
        if (!lcd::clip_to_band(by, bh)) return;
    }
    const bool vertical = panel_is_column(opp_edge);
    const bool rev = opp_reversed(opp_edge, dir);   // high-end -> low-end placement along the rail (Left bottom-up, Bottom right-to-left)
    const bool text_rev = vertical && rev;          // horizontal text always runs left-to-right even when cells run right-to-left
    const bool active_mast = cell.masthead && cell.active;
    const uint16_t bg = active_mast ? kAccent : kBg2;
    const uint16_t border_col = active_mast ? kAccent : kRule;
    const int16_t along = vertical ? s.h : s.w;
    const int16_t fh = static_cast<int16_t>(kLabelFont.height);
    // at() maps cell progression [u, u + len) along the rail; text_rev only mirrors inner item order inside vertical Ccw cells.
    const auto at = [&](int u, int len) { return static_cast<int16_t>(rev ? along - u - len : u); };

    // Inner divider (1 px along content-facing edge) + the 1 px rule across the cell: after the masthead, before slots 2 and 3.
    Box body{0, 0, s.w, s.h};
    if (vertical) {
        const int16_t div_x = opp_edge == PanelEdge::Left ? static_cast<int16_t>(s.w - 1) : 0;
        gfx_fill(s, div_x, 0, 1, s.h, border_col);
        body.x = opp_edge == PanelEdge::Left ? 0 : 1;
        body.w = static_cast<int16_t>(s.w - 1);
        if (cell.masthead) {
            gfx_fill(s, body.x, at(along - 1, 1), body.w, 1, active_mast ? kBg : kRule);
            body.h = static_cast<int16_t>(s.h - 1);
            if (rev) body.y = 1;
        } else if (slot > 1) {
            const int16_t ry = at(0, 1);
            gfx_fill(s, body.x, ry, 2, 1, bg);
            gfx_fill(s, static_cast<int16_t>(body.x + 2), ry, static_cast<int16_t>(body.w > 4 ? body.w - 4 : body.w), 1, kRule);
            if (body.w > 4) gfx_fill(s, static_cast<int16_t>(body.x + body.w - 2), ry, 2, 1, bg);
            body.h = static_cast<int16_t>(s.h - 1);
            if (!rev) body.y = 1;
        }
    } else {
        const int16_t div_y = opp_edge == PanelEdge::Top ? static_cast<int16_t>(s.h - 1) : 0;
        gfx_fill(s, 0, div_y, s.w, 1, border_col);
        body.y = opp_edge == PanelEdge::Top ? 0 : 1;
        body.h = static_cast<int16_t>(s.h - 1);
        if (cell.masthead) {
            gfx_fill(s, at(along - 1, 1), body.y, 1, body.h, active_mast ? kBg : kRule);
            body.w = static_cast<int16_t>(s.w - 1);
            if (rev) body.x = 1;
        } else if (slot > 1) {
            const int16_t rx = at(0, 1);
            gfx_fill(s, rx, body.y, 1, 2, bg);
            gfx_fill(s, rx, static_cast<int16_t>(body.y + 2), 1, static_cast<int16_t>(body.h > 4 ? body.h - 4 : body.h), kRule);
            if (body.h > 4) gfx_fill(s, rx, static_cast<int16_t>(body.y + body.h - 2), 1, 2, bg);
            body.w = static_cast<int16_t>(s.w - 1);
            if (!rev) body.x = 1;
        }
    }

    const uint8_t dots = cell.dot_count > 5 ? 5 : cell.dot_count;
    const int spot_w = cell.spot ? 5 : 0;
    const int dots_w = dots > 0 ? static_cast<int>(dots) * 5 : 0;
    // A link cell keeps the icon at the far bezel end (1 px gap before it, 1 px after); the text centres in the rest,
    // with 2 px of padding instead of 4 (a "D+ 3.30V" cell in the narrowest slot still fits whole).
    const bool linked = cell.link != pclink::State::None && !cell.masthead;
    const int zone = linked ? along - (kLinkIconPx + 2) : along;
    const int text_offset = (!vertical && rev && linked) ? (kLinkIconPx + 2) : 0;
    const int avail = std::max<int>(zone - (linked ? 2 : 4) - spot_w - dots_w, 0);
    std::string_view tag_src = cell.tag;
    if (linked && !tag_src.empty() && tag_src.back() == ' ' &&
        text_width(kLabelFont, tag_src) + text_width(kLabelFont, cell.value) > avail)
        tag_src.remove_suffix(1);   // "D+ 3.30V" -> "D+3.30V": the separator goes before the value is cut
    const int tag_w_raw = text_width(kLabelFont, tag_src);
    std::string_view tag_v = (tag_w_raw > 0 && tag_w_raw + 12 <= avail) ? tag_src : std::string_view{};
    const int tag_w = text_width(kLabelFont, tag_v);
    char vbuf[kFitBuf];
    const std::string_view val_v = cell.value.empty() ? std::string_view{} : fit_text(kLabelFont, cell.value, std::max<int>(avail - tag_w, 6), vbuf);
    const int val_w = text_width(kLabelFont, val_v);
    const int total = spot_w + tag_w + val_w + dots_w;

    // Precompute element boxes for single-pass fill_except (zero overdraw).
    Box holes[6];
    int nh = 0;
    Box spot_box{}, tag_box{}, val_box{}, dot_box[5]{}, link_box{};
    const int16_t pp = vertical ? (opp_edge == PanelEdge::Left ? int16_t(1) : int16_t(3)) : (opp_edge == PanelEdge::Top ? int16_t(1) : int16_t(3));
    // A run of `len` px at inner item offset u, `plen` px thick at offset `po` across the cell.
    const auto run = [&](int u, int len, int po, int plen) {
        const int16_t pos = static_cast<int16_t>(text_rev ? along - u - len : text_offset + u);
        return vertical ? Box{static_cast<int16_t>(pp + po), pos, static_cast<int16_t>(plen), static_cast<int16_t>(len)}
                        : Box{pos, static_cast<int16_t>(pp + po), static_cast<int16_t>(len), static_cast<int16_t>(plen)};
    };
    int cur = (zone - total) / 2;
    if (cell.spot) {
        spot_box = run(cur, 3, 2, 8);
        holes[nh++] = spot_box;
        cur += 5;
    }
    if (tag_w > 0) {
        tag_box = run(cur, tag_w, 0, fh);
        holes[nh++] = tag_box;
        cur += tag_w;
    }
    if (val_w > 0) {
        val_box = run(cur, val_w, 0, fh);
        holes[nh++] = val_box;
        cur += val_w;
    }
    if (dots > 0) {
        holes[nh++] = run(cur + 2, dots * 5 - 2, 4, 3);
        for (uint8_t i = 0; i < dots; i++) dot_box[i] = run(cur + 2 + i * 5, 3, 4, 3);
    }

    if (linked) {
        const int16_t ac = static_cast<int16_t>(vertical ? body.x + (body.w - kLinkIconPx) / 2 : body.y + (body.h - kLinkIconPx) / 2);
        const int16_t oa = at(along - 1 - kLinkIconPx, kLinkIconPx);
        link_box = vertical ? Box{ac, oa, kLinkIconPx, kLinkIconPx} : Box{oa, ac, kLinkIconPx, kLinkIconPx};
        holes[nh++] = link_box;
    }
    fill_except(s, body, holes, nh, bg);
    if (linked) paint_link_icon(s, link_box, cell.link, bg, cell.link == pclink::State::Power ? kInk2 : kInk, kFocus);
    if (cell.spot) gfx_fill(s, spot_box.x, spot_box.y, spot_box.w, spot_box.h, cell.spot);
    if (tag_w > 0) {
        if (vertical) gfx_text_rot(s, kLabelFont, tag_box.x, tag_box.y, tag_v, cell.tag_color, bg, dir);
        else gfx_text(s, kLabelFont, tag_box.x, tag_box.y, tag_v, cell.tag_color, bg);
    }
    if (val_w > 0) {
        if (vertical) gfx_text_rot(s, kLabelFont, val_box.x, val_box.y, val_v, cell.value_color, bg, dir);
        else gfx_text(s, kLabelFont, val_box.x, val_box.y, val_v, cell.value_color, bg);
    }
    if (dots > 0) {
        // Fill the 2 px gaps between dots inside the single dot-group hole, then paint the 3x3 dots.
        for (uint8_t i = 0; i < dots; i++) {
            const uint16_t dot_col = active_mast ? (i == cell.dot_cur ? kSpotInk : kFocusFill)
                                                 : (i == cell.dot_cur ? kFocus : kInk3);
            gfx_fill(s, dot_box[i].x, dot_box[i].y, 3, 3, dot_col);
            if (i + 1 < dots) {
                if (vertical) {
                    const int16_t gy = text_rev ? static_cast<int16_t>(dot_box[i].y - 2) : static_cast<int16_t>(dot_box[i].y + 3);
                    gfx_fill(s, dot_box[i].x, gy, 3, 2, bg);
                } else {
                    gfx_fill(s, static_cast<int16_t>(dot_box[i].x + 3), dot_box[i].y, 2, 3, bg);
                }
            }
        }
    }
}

K2_HOT void paint_texts(const Surface &s, Box ground, const TextRun *runs, int n, uint16_t bg) {
    Box holes[kMaxHoles];
    int m = 0;
    for (int i = 0; i < n && m < kMaxHoles; i++)
        holes[m++] = {runs[i].x, runs[i].y, static_cast<int16_t>(runs[i].dx + text_width(*runs[i].font, runs[i].text)), static_cast<int16_t>(runs[i].font->height)};
    fill_except(s, ground, holes, m, bg);
    for (int i = 0; i < n; i++) gfx_text(s, *runs[i].font, runs[i].x, runs[i].y, runs[i].text, runs[i].ink, bg, runs[i].dx);
}

K2_HOT uint32_t RowList::row_sig_cb(void *ctx, int index) {
    auto *self = static_cast<RowList *>(ctx);
    RowSpec r;
    const bool focused = index == self->lit();
    self->fn_(self->ctx_, index, focused, r);
    uint32_t h = mix_u32(static_cast<uint32_t>(r.marker) | (static_cast<uint32_t>(r.state) << 4) |
                       (static_cast<uint32_t>(r.icon) << 8) | (static_cast<uint32_t>(r.chevron) << 16) |
                       (static_cast<uint32_t>(focused) << 17) |
                       (static_cast<uint32_t>(r.section_before) << 19));
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
    const uint16_t bar_col = kFocusBar;
    if (hi && !bar) {
        sp.text_color = kOnFocus;
        if (!sp.tail_tint) sp.tail_color = kOnFocusDim;
        if (sp.marker != Marker::Verdict) sp.marker_color = kOnFocus;
    } else if (hi && sp.marker == Marker::Icon && sp.marker_color == 0) {
        sp.marker_color = kAccent;
    }
    if (hi) {   // blocked / placeholder ink3 all but vanishes on the focus fill: the focus-dim ink reads there
        if (sp.text_color == kInk3) sp.text_color = kOnFocusDim;
        if (sp.tail_color == kInk3) sp.tail_color = kOnFocusDim;
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
    int16_t tail_x = 0;
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
        tail_x = tail_origin(s, ty, vw);
        hole(tail_x, ty, vw, kSmall.height);
        right -= vw + 6;   // 6 px gap: a 5-char col2 value (40 px) fits beside a 48 px margin tail
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
        note_v = sp.data ? fit_data(kLabelFont, sp.note, kNoteMaxPx, nbuf) : fit_text(kLabelFont, sp.note, kNoteMaxPx, nbuf);
        hole(kTextX, static_cast<int16_t>(y + kNoteDy), text_width(kLabelFont, note_v), kLabelFont.height);
    }

    const int16_t rw = self->list_.row_width();
    const int16_t top = sp.section_before ? 2 : 1;
    if (sp.section_before) gfx_fill(s, 0, y, rw, 2, kRule);
    const int16_t gx = bar ? 3 : 0;
    if (bar) gfx_fill(s, 0, static_cast<int16_t>(y + top), 3, static_cast<int16_t>(r.h - top), bar_col);
    fill_except(s, {gx, static_cast<int16_t>(y + top), static_cast<int16_t>(rw - gx), static_cast<int16_t>(r.h - top)},
                holes, nh, bg);

    draw_marker(s, kMarkX, static_cast<int16_t>(y + 7), sp, bg);
    if (sp.chevron) {
        draw_drill(s, kTailX, static_cast<int16_t>(y + 1 + (kRowH - 1) / 2), sp.tail_color);
    } else if (pill_w) {
        const int16_t px0 = static_cast<int16_t>(kTailX - pill_w);
        const int16_t py = static_cast<int16_t>(y + 1 + (kRowH - 1 - kPillH) / 2);
        gfx_fill_panel(gfx_native_rect(s, px0, py, static_cast<int16_t>(pill_w), kPillH), sp.tail_tint);
        gfx_text(s, kLabelFont, static_cast<int16_t>(px0 + 4), static_cast<int16_t>(py + 1), tail_v,
                 sp.tail_color, sp.tail_tint);
    } else if (!tail_v.empty()) {
        gfx_text(s, kSmall, tail_x, ty, tail_v, sp.tail_color, bg);
    }
    const uint16_t dim = hi ? kOnFocusDim : kDim;
    if (!col2_v.empty()) gfx_text(s, kSmall, kCol2X, ty, col2_v, dim, bg);
    gfx_text(s, kSmall, kTextX, ty, text_v, sp.text_color, bg);
    if (!note_v.empty()) gfx_text(s, kLabelFont, kTextX, static_cast<int16_t>(y + kNoteDy), note_v, dim, bg);
}

K2_HOT void RowList::draw(uint32_t now_ms, PaintBudget *bg) {
    if (!fn_) { if (bg) bg->defer(); return; }
    list_.set_count(count_, false);
    const int lit_row = lit();
    list_.set_highlight(lit_row);
    const ListState &st = list_.state();
    if (sel_ >= 0 && sel_ < count_) {
        int off = st.offset;
        if (sel_ < off) off = sel_;
        else if (sel_ >= off + st.visible) off = sel_ - st.visible + 1;
        list_.set_offset(off);
    }
    int16_t want = 0;
    if (lit_row >= 0 && lit_row < count_) {
        RowSpec probe;
        fn_(ctx_, lit_row, true, probe);
        if (!probe.note.empty()) want = kFocusExtra;
    }
    if (want != extra_) {
        const bool known = extra_ >= 0;
        if (want < kFocusExtra && bg && !bg->admit(static_cast<int>(surf_.w) * (kFocusExtra - want), true)) return;
        list_.set_focus_extra(want);
        if (known) list_.invalidate_bar();   // the rows below the focus moved: ListView repaints them by position
        if (want < kFocusExtra)
            gfx_fill(surf_, 0, static_cast<int16_t>(y_ + st.visible * kRowH + want), surf_.w,
                     static_cast<int16_t>(kFocusExtra - want), kBg);
        extra_ = want;
    }
    list_.draw(now_ms, bg);
}

}  // namespace k2::kit
