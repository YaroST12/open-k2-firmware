// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Hot-path UI widgets placed in the zero-wait flash window (K2_HOT).
#include "k2/ui/ui_widgets.hpp"
#include "ui_widgets_internal.hpp"
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

namespace detail {

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

namespace {

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

}  // namespace

[[gnu::noinline]] K2_HOT void draw_band(const Surface &s, int x, int y, int h, const BandOp &op, int max_w, BandClip clip) {
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

}  // namespace detail

K2_HOT void draw_icon(const Surface &s, int16_t x, int16_t y, Icon icon, uint16_t fg, uint16_t bg) {
    detail::stream_mono(s, x, y, kIconSize, kIconSize, kIconBits[static_cast<int>(icon)], kIconSize - 1, fg, bg);
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
    detail::BandOp op{};
    op.kind = detail::BandKind::Footer;
    op.fst = st;
    op.s0 = fitted;
    op.fy = fy;
    detail::draw_band(s, 0, fy, s.h - fy, op);
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
    detail::BandOp op{};
    op.kind = detail::BandKind::StatusLeft;
    op.font = &tf;
    op.s0 = fitted;
    op.s1 = badge;
    op.tx = static_cast<int16_t>(x);
    op.ty = static_cast<int16_t>(ty);
    op.sep = static_cast<int16_t>(sep);
    op.flag = has_icon && detail::fits(s, kMastIconX, kBarTextY, kIconSize, kIconSize);
    op.icon = icon;
    detail::draw_band(s, 0, 0, kBarH - 1, op, readout_follows ? detail::kReadoutX : kScreenW);
    gfx_fill(s, 0, static_cast<int16_t>(kBarH - 1), s.w, 1, kAccent);   // the masthead rule
}

K2_HOT void draw_top_readout(const Surface &s, StatusReadout mode, std::string_view volts, std::string_view amps,
                      Arrow flow, uint8_t rotation, std::string_view last_text) {
    const std::string_view v = detail::slot_text(volts), a = detail::slot_text(amps);
    if (mode == StatusReadout::Live) {
        if (v.data() != volts.data()) K2_TRUNC_NOTE_HERE(trunc::Kind::Data, volts, detail::kSlotPx);   // a measurement, not copy
        if (a.data() != amps.data()) K2_TRUNC_NOTE_HERE(trunc::Kind::Data, amps, detail::kSlotPx);
    }
    detail::BandOp op{};
    op.kind = detail::BandKind::Readout;
    op.mode = mode;
    op.s0 = v;
    op.s1 = a;
    op.s2 = last_text;
    op.flow = flow;
    op.rotation = rotation;
    detail::draw_band(s, detail::kReadoutX, 0, kBarH - 1, op);
}

K2_HOT void draw_top_readout(const Surface &s, StatusReadout mode, std::string_view volts, std::string_view amps, Arrow flow,
                      uint8_t rotation, ReadoutShown &shown) {
    ReadoutShown now;
    now.mode = mode;
    now.flow = mode == StatusReadout::Live ? flow : Arrow::Idle;
    now.rotation = rotation;
    const std::string_view v = detail::slot_text(volts), a = detail::slot_text(amps);
    const bool cells = ReadoutShown::set(now.volts, v) & ReadoutShown::set(now.amps, a);
    const bool delta = cells && shown.valid && mode == StatusReadout::Live && shown.mode == StatusReadout::Live &&
                       shown.rotation == rotation;
    if (!delta) {
        draw_top_readout(s, mode, volts, amps, flow, rotation);
    } else {
        const ReadoutDelta d = readout_delta(shown, now);
        detail::BandOp op{};
        op.kind = detail::BandKind::Readout;
        op.mode = mode;
        op.s0 = v;
        op.s1 = a;
        op.flow = flow;
        op.rotation = rotation;
        for (int i = 0; i < d.n; i++)
            detail::draw_band(s, detail::kReadoutX, 0, kBarH - 1, op, kScreenW,
                              detail::BandClip{d.span[i].x0 - detail::kReadoutX, d.span[i].x1 - detail::kReadoutX, kBarTextY,
                                               kBarTextY + static_cast<int>(kLabelFont.height)});
    }
    now.valid = cells;
    shown = now;
}

}  // namespace k2
