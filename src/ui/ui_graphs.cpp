// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Graphs page: four stacked graphs (W, V, A, Wh) over the shared trend, their header rows and one info line
// (include/k2/ui/ui_graphs.hpp). Pure parts: graphs_model.hpp (tests/test_graphs_model.cpp); the scroll-diff draw is
// checked pixel for pixel against a full repaint in tests/test_ui_graphs.cpp.
#include "k2/ui/ui_graphs.hpp"

#include <span>
#include <string_view>
#include "k2/drivers/lcd.hpp"
#include "k2/pd/diag_live.hpp"
#include "k2/pd/diag_screen.hpp"
#include "k2/ui/home_cards.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_viewport.hpp"

namespace k2 {

namespace gr = graphs;
namespace hm = home;

namespace {

using Band = gr::Band;
constexpr int kBands = gr::kBands, kCols = gr::kCols;
constexpr const char *kLabel[kBands] = {"POWER", "VBUS", "IBUS", "ENERGY"};
constexpr int kElChars = 5;   // hm::fmt_elapsed: never over five characters
constexpr uint8_t kNoRow = GraphsPage::kNoRow, kUnknown = GraphsPage::kUnknown;

int16_t text_px(std::string_view s) { return static_cast<int16_t>(s.size() * kSmall.adv); }

// The cost of a ground with text on it: the fill around the holes is a solid stream (a quarter of the units, PaintBudget::units),
// the glyph cells are composed pixels.
int ground_units(int area_px, int text_px) { return (area_px - text_px + 3) / 4 + text_px; }

}  // namespace

// What one update shows: the values (as the display filter let them through), the ceilings and the info line's texts.
struct GraphsPage::Shown {
    hm::Mode mode = hm::Mode::Idle;
    int32_t milli[kBands] = {};
    uint32_t max[kBands] = {};      // the ceilings after the slash: W (mW), V (mV), A (mA); 0 = none
    StrBuf<17> word;                // the diagnosis word
    uint16_t word_ink = kDim;
    StrBuf<27> proto;               // "PD 20V5A"
    uint32_t elapsed_s = 0;
    bool has_elapsed = false;
};

// ---- sampling --------------------------------------------------------------------------------

void GraphsPage::sample_shown(Shown &s, uint32_t now_ms) {
    const bool live = s.mode == hm::Mode::Live, stale = s.mode == hm::Mode::Stale;
    const power::Metrics &pm = stale ? ctx_.last.pm : ctx_.pm;
    if (live) {
        if (thr_.pass(now_ms)) {
            v_mv_ = static_cast<int32_t>(power::magnitude(pm.vbus_mv));
            a_ma_ = static_cast<int32_t>(power::magnitude(pm.current_ma));
            w_mw_ = static_cast<int32_t>(power::magnitude(pm.power_mw));
        }
    } else if (stale) {
        v_mv_ = static_cast<int32_t>(power::magnitude(pm.vbus_mv));
        a_ma_ = static_cast<int32_t>(power::magnitude(pm.current_ma));
        w_mw_ = static_cast<int32_t>(power::magnitude(pm.power_mw));
    } else {
        v_mv_ = a_ma_ = w_mw_ = 0;
    }
    s.milli[0] = w_mw_;
    s.milli[1] = v_mv_;
    s.milli[2] = a_ma_;
    const auto mwh = ctx_.meter.mwh();
    s.milli[3] = mwh > 0 ? static_cast<int32_t>(mwh) : 0;
    if (live || stale) {
        const hm::ProtoInfo p = live ? hm::live_proto(ctx_, pm) : ctx_.last.proto;
        // The ceilings Home's badges show: the contract's power, the source's top voltage (a PPS / AVS range's top), the
        // current rating.
        s.max[0] = hm::contract_mw(p);
        s.max[1] = (p.kind == hm::ProtoKind::Pps || p.kind == hm::ProtoKind::Avs) && p.hi_mv ? p.hi_mv : p.max_mv;
        s.max[2] = p.ma;
        hm::ProtoText pt;
        hm::format_proto(p, pt);
        if (!pt.tag.view().empty()) s.proto.put(pt.tag.view()).put_char(' ');
        s.proto.put(pt.value.view());
        s.elapsed_s = live ? ctx_.meter.elapsed_s(now_ms) : ctx_.last.elapsed_s;
        s.has_elapsed = true;
        hm::DiagText dt;
        if (hm::format_diag(s.mode, pd::diag::live::report(), dt)) {
            const std::string_view w = dt.value.view();
            // Nothing wrong reads "Fine": the word column is narrow, a finding keeps its own words.
            if (w == "Healthy" || w == "Link healthy") s.word.put("Fine");
            else s.word.put(w);
            s.word_ink = dt.tone == hm::DiagTone::Error ? kError : dt.tone == hm::DiagTone::Caution ? kCaution : kDim;
        }
        if (stale) s.word_ink = kStale;
    } else {
        s.word.put(s.mode == hm::Mode::NoMeter ? "No meter" : "No source");
        s.word_ink = kDim;
    }
}

// ---- static parts --------------------------------------------------------------------------------

// The 1 px rules between the bands, the info line's 2 px rule, the left margin beside each graph and the ground row under
// it: everything no header, graph or info paint covers.
bool GraphsPage::paint_frame() {
    const Surface &S = ctx_.screen;
    const int16_t gh = g_.graph_rows();
    if (!admit(ctx_.budget, ground_units(g_.w * (kBands * 2 + 2) + gr::kX0 * gh * kBands, 0))) return false;
    for (int k = 0; k < kBands; k++) {
        gfx_fill(S, 0, g_.graph_y0(k), gr::kX0, gh, kBg);                         // left margin
        gfx_fill(S, 0, g_.band_y1(k), g_.w, 1, kBg);                              // ground row under the graph
        if (k < kBands - 1) gfx_fill(S, 0, static_cast<int16_t>(g_.band_y1(k) + 1), g_.w, 1, kRule);
    }
    const int16_t gap0 = static_cast<int16_t>(g_.band_y1(kBands - 1) + 1);
    if (gap0 < g_.info_rule_y()) gfx_fill(S, 0, gap0, g_.w, static_cast<int16_t>(g_.info_rule_y() - gap0), kBg);
    gfx_fill(S, 0, g_.info_rule_y(), g_.w, 2, kRule);
    frame_ok_ = true;
    return true;
}

// ---- header rows -----------------------------------------------------------------------------------

// Label left (font 12, dim), span right, and in the middle the live value in the band colour, its unit, then "/max" in dim,
// the group centred. A changed span or ceiling repaints the row (the group moves); the digits repaint per character.
bool GraphsPage::paint_head(int k, const Shown &s, bool digits) {
    const Surface &S = ctx_.screen;
    const Band b = static_cast<Band>(k);
    const int16_t y0 = g_.band_y0(k);
    const hm::Mode m = s.mode;
    const uint16_t ink = tone(m, band_color(k)), dim = m == hm::Mode::Stale ? kStale : kDim;
    char sp[8];
    const uint32_t span_s = trend::window_s(window_) * (b == Band::Wh ? 10u : 1u);
    const std::string_view span{sp, static_cast<size_t>(gr::span_text(sp, span_s))};
    StrBuf<8> mx;
    gr::max_text(b, m == hm::Mode::Live || m == hm::Mode::Stale ? s.max[k] : 0u, mx);
    const std::string_view unit = gr::unit_text(b);
    const int vc = gr::value_chars(b);
    const int chars = vc + static_cast<int>(unit.size() + mx.view().size());
    const int16_t gx = g_.group_x(chars);
    const uint32_t sig = mix_u32(mix(mix(mix_init(), span), mx.view()), (static_cast<uint32_t>(ink) << 16) | dim);
    if (!head_ok_[k] || sig != head_sig_[k]) {
        const int16_t ltop = top_for_baseline(kLabelFont, static_cast<int16_t>(y0 + gr::kLabelBase));
        const std::string_view label = kLabel[k];
        const int16_t lw = static_cast<int16_t>(label.size() * kLabelFont.adv);
        const int16_t sw = static_cast<int16_t>(span.size() * kLabelFont.adv);
        const int16_t sx = static_cast<int16_t>(g_.right() + 1 - sw);
        const int16_t gw = static_cast<int16_t>(chars * kSmall.adv);
        const int text = (lw + sw) * kLabelFont.height + gw * kSmall.height;
        if (!admit(ctx_.budget, ground_units(g_.w * gr::kHeadRows, text))) return false;
        const kit::Box holes[3] = {{gr::kLabelX, ltop, lw, static_cast<int16_t>(kLabelFont.height)},
                                   {sx, ltop, sw, static_cast<int16_t>(kLabelFont.height)},
                                   {gx, y0, gw, static_cast<int16_t>(kSmall.height)}};
        kit::fill_except(S, {0, y0, g_.w, gr::kHeadRows}, holes, 3, kBg);
        gfx_text(S, kLabelFont, gr::kLabelX, ltop, label, kDim, kBg);
        gfx_text(S, kLabelFont, sx, ltop, span, kDim, kBg);
        const int16_t ux = static_cast<int16_t>(gx + vc * kSmall.adv);
        gfx_text(S, kSmall, ux, y0, unit, ink, kBg);
        gfx_text(S, kSmall, static_cast<int16_t>(ux + text_px(unit)), y0, mx.view(), dim, kBg);
        val_[k].init(S, kSmall, gx, y0, static_cast<uint8_t>(vc), Align::Right, ink, kBg);
        head_ok_[k] = true;
        head_sig_[k] = sig;
    }
    if (!digits) return true;
    char vb[8];
    const std::string_view val = m == hm::Mode::NoMeter ? std::string_view("--")
                                                         : std::string_view{vb, static_cast<size_t>(gr::value_text(b, s.milli[k], vb))};
    if (!admit(ctx_.budget, val_[k].valid() ? kSmall.adv * kSmall.height : val_[k].box_px())) return false;
    val_[k].draw(val);
    return true;
}

// ---- info line ---------------------------------------------------------------------------------------

// The diagnosis word at x4, the protocol row at x52 (or behind a long word), the elapsed time right-aligned. A changed word,
// row or layout repaints the line; the elapsed time repaints per character.
bool GraphsPage::paint_info(const Shown &s, bool digits) {
    const Surface &S = ctx_.screen;
    const int16_t top = g_.info_top();
    char eb[8];
    const std::string_view el{eb, s.has_elapsed ? static_cast<size_t>(hm::fmt_elapsed(eb, s.elapsed_s)) : 0u};
    // The layout counts the elapsed time's own characters ("3:07" leaves the protocol one more cell than "12:34"); its field
    // keeps five cells, right-aligned, so the blank ones lie in the gap the layout keeps.
    const gr::InfoLayout l = gr::info_layout(s.word.view().size(), s.proto.view().size(), el.size(), g_.right());
    const uint16_t pink = s.mode == hm::Mode::Stale ? kStale : kText;
    uint32_t sig = mix_u32(mix(mix(mix_init(), s.word.view()), s.proto.view()), (static_cast<uint32_t>(s.word_ink) << 16) | pink);
    sig = mix_u32(sig, (static_cast<uint32_t>(static_cast<uint16_t>(l.proto_x)) << 1) | (l.elapsed ? 1u : 0u));
    const int16_t ex = static_cast<int16_t>(g_.right() + 1 - kElChars * kSmall.adv);
    if (!info_ok_ || sig != info_sig_) {
        const int16_t y = static_cast<int16_t>(g_.info_rule_y() + 2);
        const int text = (text_px(s.word.view()) + (l.proto_x >= 0 ? text_px(s.proto.view()) : 0)) * kSmall.height;
        if (!admit(ctx_.budget, ground_units(g_.w * (g_.h - y), text + (l.elapsed ? el_.box_px() : 0)))) return false;
        kit::Box holes[3];
        int n = 0;
        holes[n++] = {gr::kWordX, top, text_px(s.word.view()), static_cast<int16_t>(kSmall.height)};
        if (l.proto_x >= 0) holes[n++] = {l.proto_x, top, text_px(s.proto.view()), static_cast<int16_t>(kSmall.height)};
        if (l.elapsed) holes[n++] = {ex, top, static_cast<int16_t>(kElChars * kSmall.adv), static_cast<int16_t>(kSmall.height)};
        kit::fill_except(S, {0, y, g_.w, static_cast<int16_t>(g_.h - y)}, holes, n, kBg);
        gfx_text(S, kSmall, gr::kWordX, top, s.word.view(), s.word_ink, kBg);
        if (l.proto_x >= 0) gfx_text(S, kSmall, l.proto_x, top, s.proto.view(), pink, kBg);
        el_.init(S, kSmall, ex, top, kElChars, Align::Right, s.mode == hm::Mode::Stale ? kStale : kDim, kBg);
        el_shown_ = l.elapsed;
        info_ok_ = true;
        info_sig_ = sig;
    }
    if (!el_shown_ || !digits) return true;
    if (!admit(ctx_.budget, el_.valid() ? kSmall.adv * kSmall.height : el_.box_px())) return false;
    el_.draw(el);
    return true;
}

// ---- graphs -------------------------------------------------------------------------------------------

// The columns band k should show now: per column the vertical run from the previous point to this one, the W band's
// limit row (dotted: odd columns only, anchored to the screen, so a flat limit never repaints), and the newest point's
// 3 x 3 dot on the columns it covers.
[[gnu::noinline]] void GraphsPage::want(int k, Col (&out)[kCols], uint8_t (&lim_out)[kCols / 2],
                                      uint8_t &dot_out) const {
    const auto &t = ctx_.trend;
    const trend::Window w = window_;
    const int gh = g_.graph_rows();
    uint16_t v[kCols];
    uint8_t lim[kCols] = {};
    uint32_t top = 0;
    dot_out = kNoRow;
    if (k == 3) {
        t.energy(w, v, kCols);
        uint32_t mx = 0;
        for (uint16_t e : v) if (e != trend::kNone && e > mx) mx = e;
        top = gr::wh_top(mx, trend::Trend::energy_unit_mwh(w));
    } else {
        const trend::Qty q = k == 0 ? trend::Qty::W : k == 1 ? trend::Qty::V : trend::Qty::A;
        t.columns(w, q, v, kCols);
        uint32_t mx = 0;
        for (uint16_t x : v) if (x != trend::kNone && x > mx) mx = x;
        if (k == 0) {
            t.limits(w, lim, kCols);
            uint32_t lmax = 0;
            for (uint8_t l : lim) if (l > lmax) lmax = l;
            top = gr::scale_w10(mx, lmax);
        } else {
            top = k == 1 ? gr::scale_v10(mx) : gr::scale_a10(mx);
        }
    }
    int prev = -1, newest = -1;
    for (int c = 0; c < kCols; c++) {
        Col &o = out[c];
        o = Col{kNoRow, kNoRow};
        if (k == 0 && (c & 1))
            lim_out[c >> 1] = lim[c] ? static_cast<uint8_t>(gh - 1 - gr::level(lim[c] * 100u, top, gh)) : kNoRow;
        if (v[c] == trend::kNone) { prev = -1; continue; }
        const int row = gh - 1 - gr::level(v[c], top, gh);
        const int a = prev < 0 ? row : prev;
        o.lo = static_cast<uint8_t>(a < row ? a : row);
        o.hi = static_cast<uint8_t>(a < row ? row : a);
        prev = row;
        newest = c;
    }
    if (newest != kCols - 1) return;   // the newest point is always the right-most column (Trend::columns)
    int dr = prev - 1;
    if (dr > gh - 3) dr = gh - 3;
    if (dr < 0) dr = 0;
    dot_out = static_cast<uint8_t>(dr);
}

// ---- update ----------------------------------------------------------------------------------------------

void GraphsPage::update(uint32_t now_ms) {
    ContentViewport::Pause unscaled;   // panel pixels: the page lays its own grid on the rotated content
    const Surface &S = ctx_.screen;
    if (S.w != g_.w || S.h != g_.h) {
        g_ = gr::Geom{S.w, S.h};
        invalidate();
    }
    Shown s;
    s.mode = !ctx_.pm_online ? hm::Mode::NoMeter
           : ctx_.pm.vbus_mv >= 1000 ? hm::Mode::Live
           : ctx_.stale() ? hm::Mode::Stale : hm::Mode::Idle;
    if (static_cast<uint8_t>(s.mode) != mode_) {   // every colour changes: the lines go grey on a stale screen
        mode_ = static_cast<uint8_t>(s.mode);
        thr_.reset();
        for (auto &band : col_) for (Col &c : band) c.lo = kUnknown;
        graph_dirty_ = true;
    }
    if (window_ != shown_w_) {   // Zoom: another tier, another scale
        shown_w_ = window_;
        for (auto &band : col_) for (Col &c : band) c.lo = kUnknown;
        graph_dirty_ = true;
    }
    sample_shown(s, now_ms);
    // The grounds first (a pass whose budget is already spent refuses a big item, so they never wait behind the digits),
    // then the digits, then the graphs.
    if (!frame_ok_ && !paint_frame()) return;
    for (int k = 0; k < kBands; k++)
        if (!paint_head(k, s, false)) return;
    if (!paint_info(s, false)) return;
    for (int k = 0; k < kBands; k++)
        if (!paint_head(k, s, true)) return;
    if (!paint_info(s, true)) return;
    const uint32_t seq = ctx_.trend.seq(window_);
    if (seq != seq_seen_) { seq_seen_ = seq; graph_dirty_ = true; }
    if (!graph_dirty_) return;
    for (int i = 0; i < kBands; i++) {
        const int k = (rr_ + i) % kBands;
        if (!paint_graph(k)) {
            rr_ = static_cast<uint8_t>(k);   // the band that ran out goes first next time
            return;
        }
    }
    graph_dirty_ = false;
}

void GraphsPage::invalidate() {
    frame_ok_ = false;
    info_ok_ = false;
    for (bool &h : head_ok_) h = false;
    for (auto &f : val_) f.invalidate();
    el_.invalidate();
    for (auto &band : col_) for (Col &c : band) c.lo = kUnknown;
    for (uint8_t &d : dot_row_) d = kNoRow;
    graph_dirty_ = true;
}

// ---- keys ---------------------------------------------------------------------------------------------------

bool GraphsPage::on_event(Event e) {
    if (e.button == Button::Ok && e.press == Press::Short) {   // Zoom: the span 10s / 60s / 10m, shared with Home
        window_ = trend::next(window_);
        return true;
    }
    return back_event(e);   // Up / Dn flip the faces, BACK goes to Flow
}

Strip GraphsPage::labels() const {
    Strip l = base_labels();
    l[Button::Ok] = sl::Zoom;
    return l;
}

}  // namespace k2
