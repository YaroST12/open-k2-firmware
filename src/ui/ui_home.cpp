// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home v7 page ("Dual hero"): the VBUS and IBUS measurement cells with their patient-monitor sweeps, the
// W / Wh / Ah band and the two-row info block. The pure parts live in home_model / home_proto, the
// compositor in home_cell; this file decides what to repaint (zone signatures) and feeds them.
#include "k2/ui/ui_pages.hpp"

#include <span>
#include <string_view>
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/pd/link.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/usb_link.hpp"
#include "k2/ui/home_proto.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2 {

namespace hm = home;

namespace {

// Live colour, stale grey, or idle faint.
constexpr uint16_t tone(hm::Mode m, uint16_t live) { return m == hm::Mode::Live ? live : m == hm::Mode::Stale ? kStale : kFaint; }

constexpr int16_t kHeroX = 39, kUnitX = 201;          // hero digits and their unit letter
constexpr int16_t kDigitTop[2] = {24, 94};            // hero line-box tops (VBUS, IBUS)
constexpr int16_t kUnitTop[2] = {43, 113};
constexpr int16_t kHeroRows = 39;                     // font_64 line height
constexpr int16_t kPillX = 211, kPillY = 1, kPillBoxH = 17;
constexpr int16_t kArrowX = 8, kArrowY = 101;
constexpr int16_t kWarnX = 10, kWarnY = 74;
constexpr int16_t kInfoRow1Top = 201, kInfoRow2Top = 223;
constexpr int kUsbChars = 16, kElChars = 5, kWattChars = 4, kEnergyChars = 3;
constexpr int16_t kElX = 180, kRoleX = 196;
constexpr int16_t kRowTop[2] = {kInfoRow1Top, kInfoRow2Top};

// Repaint zones of the lower cell (logical, inclusive). A zone is wider and lower than its fields on purpose: a glyph
// keeps its native pixel size while the landscape mapping squeezes the rows, so an old text can reach a few pixels
// past the logical box of the new one, and the rect repainted for a change has to cover all of it.
struct Zone { int16_t x0, y0, x1, y1; };
constexpr Zone kZones[6] = {
    {0, 144, 128, 197},     // W number and its unit
    {118, 144, 223, 172},   // Wh
    {118, 165, 223, 197},   // Ah
    {0, 196, 186, 226},     // info row 1: tag and value
    {176, 196, 223, 226},   // info row 1: elapsed time
    {0, 220, 223, 239},     // info row 2: protocol, value, role
};
constexpr int kCatchUpSteps = 3;                      // late sweep steps taken in one update
constexpr int kReloadLagSteps = 8;                   // a sweep this many steps behind is rebuilt from the trend

constexpr int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

}  // namespace

// What one update decides before it draws: the mode, the protocol, the alarm and the display texts.
struct HomePage::Frame {
    hm::Mode mode = hm::Mode::Idle;
    hm::ProtoInfo proto{};
    hm::Alarm alarm = hm::Alarm::None;
    uint8_t pin = 0;           // fusb302::pin_rd(): a held Rd pin shows as the "Rd pin CCx" badge
    uint32_t now = 0;
    char hero[2][8] = {};      // V and A, five characters
    char watt[8] = {};         // W, four characters
};

void HomePage::draw_rules() {
    const Surface &c = ctx_.content;
    for (const int16_t y : {hm::kRule1Y, hm::kRule2Y}) gfx_fill(c, 0, y, hm::kW, 2, kRule);
}

void HomePage::draw_static() {
    invalidate();   // the layout is painted by the first update (it knows the mode)
}

void HomePage::invalidate() {
    statics_ok_ = false;
    lower_ok_ = false;
    cell_valid_[0] = cell_valid_[1] = false;
    thr_.reset();   // the digits repaint at once; mode_ and the sweep stay (a capture re-renders strips of one frame)
    hd_[0].invalidate();
    hd_[1].invalidate();
}

PanelLabels HomePage::labels() const {
    PanelLabels l = base_labels();
    l.b[static_cast<int>(Button::Ok)] = {"Zoom", "Reset", "", false};   // short: trend window; long: energy reset
    return l;
}

bool HomePage::on_event(Event e) {
    if (e.button == Button::Ok && e.press == Press::Long) {
        ctx_.ui->run_action(Action::EnergyReset, ctx_.now);
        return true;
    }
    if (e.button == Button::Ok && e.press == Press::Short) {
        window_ = trend::next(window_);
        sweep_ok_ = false;   // the next update reloads the sweep for the new window and repaints the cells
        return true;
    }
    return back_event(e);
}

// ---- sampling --------------------------------------------------------------------------------

home::ProtoInfo home::live_proto(const UiContext &ctx, const power::Metrics &pm) {
    const pd::Link &link = ctx.ui->link();
    const bool pd_link = link.attached() && !link.waiting();
    ProtoSource ps;
    ps.ctr = fusb302::contract();
    ps.pd_link = pd_link;
    ps.caps = pd_link ? &pd::active_caps() : nullptr;
    ps.sink = &fusb302::sink_status();   // the K2's own sink: it never hears its own Requests
    ps.fc = ctx.fc;
    ps.vbus_mv = pm.vbus_mv;
    ps.typec_ma = fusb302::typec_ma();
    ps.req_period_ms = fusb302::request_period_ms();
    return derive_proto(ps);
}

home::ProtoInfo Ui::proto_now() const { return home::live_proto(ctx_, ctx_.pm); }

void HomePage::sample_values(Frame &f, uint32_t now_ms) {
    const bool live = f.mode == hm::Mode::Live, stale = f.mode == hm::Mode::Stale;
    f.pin = fusb302::pin_rd();
    const power::Metrics &pm = stale ? ctx_.last.pm : ctx_.pm;
    if (live) {
        if (thr_.pass(now_ms)) {   // the display filter: V, A and W change at most 4 times a second, as one set
            v_mv_ = abs32(pm.vbus_mv);
            a_ma_ = abs32(pm.current_ma);
            w_mw_ = abs32(pm.power_mw);
            flow_ = pm.flow;
        }
        f.proto = hm::live_proto(ctx_, pm);
        last_proto_ = f.proto;
        f.alarm = hm::alarm_of(static_cast<uint32_t>(a_ma_), f.proto.ma);
    } else if (stale) {
        v_mv_ = abs32(pm.vbus_mv);
        a_ma_ = abs32(pm.current_ma);
        w_mw_ = abs32(pm.power_mw);
        flow_ = pm.flow;
        f.proto = last_proto_;
    } else {
        v_mv_ = a_ma_ = w_mw_ = 0;
        flow_ = power::Flow::Idle;
    }
    if (f.mode == hm::Mode::NoMeter) {
        for (char (&h)[8] : f.hero) { h[0] = h[1] = '-'; h[2] = 0; }
        f.watt[0] = f.watt[1] = '-';
        f.watt[2] = 0;
    } else {
        hm::fmt_hero(f.hero[0], v_mv_);
        hm::fmt_hero(f.hero[1], a_ma_);
        hm::fmt_watts(f.watt, w_mw_);
    }
}

// ---- sweep -----------------------------------------------------------------------------------

void HomePage::update_sweep(const Frame &f, uint32_t now_ms, int &step_col, int &step_n) {
    step_col = -1;
    step_n = 0;
    if (f.mode != hm::Mode::Live && f.mode != hm::Mode::Stale) return;
    const uint32_t sv = hm::scale_v10(f.proto.max_mv), sa = hm::scale_a10(f.proto.ma);
    const uint32_t sig = mix_u32(mix_u32(mix_init(), sv), sa);
    const uint32_t step = hm::step_ms(static_cast<int>(window_));
    if (sweep_ok_ && (sig != sweep_scale_ || (f.mode == hm::Mode::Live && now_ms - sweep_t_ >= kReloadLagSteps * step)))
        sweep_ok_ = false;
    constexpr int kLoad = hm::kCols;
    const int span[2] = {hm::kVoltCell.span(), hm::kAmpCell.span()};
    const uint32_t scale[2] = {sv, sa};
    const trend::Qty qty[2] = {trend::Qty::V, trend::Qty::A};
    if (!sweep_ok_) {   // rebuild both sweeps from the trend: newest sample at the column before the cursor
        uint16_t col[kLoad];
        uint8_t lv[kLoad];
        for (int i = 0; i < 2; i++) {
            ctx_.trend.columns(window_, qty[i], col, kLoad);
            for (int k = 0; k < kLoad; k++) lv[k] = static_cast<uint8_t>(hm::sweep_rows(col[k], scale[i], span[i]));
            sw_[i].load(lv);
        }
        sweep_ok_ = true;
        sweep_t_ = now_ms;
        sweep_scale_ = sig;
        sweep_epoch_++;
        return;
    }
    const bool replay = now_ms == sweep_seen_;   // a capture re-renders the same frame: no second step
    sweep_seen_ = now_ms;
    // One column per step. An update catches up on a few late steps at once (one repaint strip covers them);
    // a longer lag reloads the sweep above.
    while (f.mode == hm::Mode::Live && !replay && step_n < kCatchUpSteps && now_ms - sweep_t_ >= step) {
        sweep_t_ += step;
        for (int i = 0; i < 2; i++) {
            const uint8_t lv = static_cast<uint8_t>(hm::sweep_rows(ctx_.trend.latest(window_, qty[i]), scale[i], span[i]));
            const int first = sw_[i].step(lv);
            if (i == 0 && step_n == 0) step_col = first;
        }
        step_n++;
    }
}

// ---- cells -----------------------------------------------------------------------------------

void HomePage::paint_cell(int ci, const Frame &f, int step_col, int step_n, bool digits_changed, int16_t px0, int16_t px1) {
    const bool amp = ci == 1;
    const hm::CellGeom g = amp ? hm::kAmpCell : hm::kVoltCell;
    const hm::Mode m = f.mode;
    const bool live = m == hm::Mode::Live, shown = live || m == hm::Mode::Stale;
    const bool over = amp && f.alarm == hm::Alarm::Over, near_ = amp && f.alarm == hm::Alarm::Near;
    const uint16_t tok = amp ? kAmp : kVolt;
    const uint16_t ink = over ? kBg : tone(m, tok);                    // digits and unit
    const uint16_t edge = over ? kText : near_ ? kCaution : tone(m, tok);
    const uint16_t label = over ? kBg : kDim;

    StrBuf<12> ptxt, mtxt;
    StrBuf<16> atxt;
    hm::Item it[12];
    int n = 0;
    it[n++] = hm::fill(0, g.top, 4, static_cast<int16_t>(g.bottom - g.top + 1), edge);
    uint32_t head = mix_init();
    int16_t head_x0, head_y0, head_y1;
    if (!amp) {
        it[n++] = hm::text(kLabelFont, 10, 3, "VBUS", label);
        if (shown) it[n++] = hm::text(kLabelFont, 40, 3, trend::label(window_), kFaint);
        const hm::Pill pill = hm::pill_of(m, f.alarm, f.pin);
        const bool blink_a = ((f.now / 500u) & 1u) == 0;               // OVER blinks at 1 Hz
        uint16_t fillc = kStale, inkc = kBg;
        switch (pill) {
        case hm::Pill::Last:   ptxt.put("LAST ").put_age(ctx_.live.age_s(f.now)); break;
        case hm::Pill::NoVbus: ptxt.put("NO VBUS"); fillc = kSurface; inkc = kDim; break;
        case hm::Pill::Over:   ptxt.put("OVER"); fillc = blink_a ? kError : kCaution; inkc = blink_a ? kText : kBg; break;
        case hm::Pill::Pin:    ptxt.put("Rd pin CC").put_char(static_cast<char>('0' + f.pin)); fillc = kCaution; inkc = kBg; break;
        case hm::Pill::None:   break;
        }
        if (pill != hm::Pill::None) {
            const int16_t pn = static_cast<int16_t>(ptxt.view().size());
            const int16_t x0 = static_cast<int16_t>(kPillX - 8 * pn);
            it[n++] = hm::fill(x0, kPillY, static_cast<int16_t>(8 * pn + 11), kPillBoxH, fillc);
            it[n++] = hm::text(kSmall, static_cast<int16_t>(x0 + 5), 2, ptxt.view(), inkc);
        } else if (hm::max_volts_text(m, f.proto, mtxt)) {
            const int16_t mn = static_cast<int16_t>(mtxt.view().size());
            it[n++] = hm::text(kLabelFont, static_cast<int16_t>(219 - 6 * mn), 3, mtxt.view(), kDim);
        }
        head = mix_u32(head, static_cast<uint32_t>(pill) | (pill == hm::Pill::Over && blink_a ? 0x10u : 0u));
        head = mix(mix(head, ptxt.view()), mtxt.view());
        head_x0 = 100; head_y0 = 0; head_y1 = 18;
    } else {
        if (f.alarm != hm::Alarm::None) {
            it[n++] = hm::warn(kWarnX, kWarnY, near_ ? kCaution : kBg, near_ ? kBg : kError);
            hm::alarm_text(f.alarm, static_cast<uint32_t>(a_ma_), f.proto.ma, atxt);
            it[n++] = hm::text(kLabelFont, 25, 75, atxt.view(), near_ ? kCaution : kBg);
        } else {
            it[n++] = hm::text(kLabelFont, 10, 75, "IBUS", label);
            if (hm::max_amps_text(m, f.proto, mtxt)) {
                const int16_t mn = static_cast<int16_t>(mtxt.view().size());
                it[n++] = hm::text(kLabelFont, static_cast<int16_t>(219 - 6 * mn), 75, mtxt.view(), kDim);
            }
        }
        head = mix_u32(head, static_cast<uint32_t>(f.alarm));
        head = mix(mix(head, atxt.view()), mtxt.view());
        head_x0 = 4; head_y0 = 72; head_y1 = 90;
    }
    it[n++] = hm::text(kMid, kUnitX, kUnitTop[ci], amp ? "A" : "V", ink);

    uint32_t arrow_sig = 0, bar_sig = 0;
    if (amp) {
        if (shown && f.proto.ma > 0) {   // limit bar: track, fill, 90 % tick
            const int fw = hm::bar_fill_w(static_cast<uint32_t>(a_ma_), f.proto.ma);
            const uint16_t fc = over ? kBg : near_ ? kCaution : m == hm::Mode::Stale ? kStale : kAmp;
            const uint16_t tc = m == hm::Mode::Stale ? kDim : kText;
            it[n++] = hm::fill(hm::kBarX0, hm::kBarY, hm::kBarTrack, hm::kBarH, kRule);
            if (fw > 0) it[n++] = hm::fill(hm::kBarX0, hm::kBarY, static_cast<int16_t>(fw), hm::kBarH, fc);
            it[n++] = hm::fill(hm::bar_tick_x(), hm::kBarY - 2, 1, hm::kBarH + 4, tc);
            bar_sig = mix_u32(mix_u32(mix_u32(1, static_cast<uint32_t>(fw)), fc), tc);
        }
        if (shown) {
            const bool dash = flow_ == power::Flow::Idle;
            const uint8_t aux = dash ? hm::dash_dir(ctx_.rotation) : hm::arrow_dir(flow_ == power::Flow::Forward, ctx_.rotation);
            const uint16_t ac = over ? kBg : dash ? kFaint : live ? kFlow : kStale;   // the stale arrow is the frozen grey
            it[n++] = hm::arrow(kArrowX, kArrowY, aux, ac);
            arrow_sig = mix_u32(mix_u32(1, aux), ac);
        }
    }
    it[n++] = hm::text(kHero, kHeroX, kDigitTop[ci], hd_[ci].view(), ink);

    hm::Cell cell;
    cell.geom = g;
    cell.bg = over ? kError : kBg;
    cell.sweep = shown && !over && sweep_ok_;
    cell.tint = live ? (amp ? kAmpT : kVoltT) : kPanelBg;
    cell.line = live ? (amp ? kAmpL : kVoltL) : kFaint;
    cell.cursor_line = kRule;
    cell.sw = &sw_[ci];
    cell.items = std::span<const hm::Item>(it, static_cast<size_t>(n));

    uint32_t base = mix_u32(mix_init(), static_cast<uint32_t>(m));
    base = mix_u32(base, (over ? 1u : 0u) | (near_ ? 2u : 0u) | (cell.sweep ? 4u : 0u));
    base = mix_u32(mix_u32(base, static_cast<uint32_t>(window_)), sweep_epoch_);
    base = mix_u32(base, ctx_.rotation & 3u);

    uint32_t *sig = cell_sig_[ci];
    if (!cell_valid_[ci] || sig[0] != base) {   // a new state: the whole cell
        sig[0] = base; sig[1] = head; sig[2] = arrow_sig; sig[3] = bar_sig;
        cell_valid_[ci] = true;
        hm::paint(cell, 0, g.top, hm::kW - 1, g.bottom);
        return;
    }
    if (sig[1] != head) {
        sig[1] = head;
        hm::paint(cell, head_x0, head_y0, hm::kW - 1, head_y1);
    }
    if (amp && sig[2] != arrow_sig) {
        sig[2] = arrow_sig;
        hm::paint(cell, kArrowX, kArrowY, kArrowX + hm::kArrowBox - 1, kArrowY + hm::kArrowBox - 1);
    }
    if (amp && sig[3] != bar_sig) {
        sig[3] = bar_sig;
        hm::paint(cell, hm::kBarX0, hm::kBarY - 2, hm::kBarX1, hm::kBarY + hm::kBarH + 1);
    }
    if (digits_changed) hm::paint(cell, px0, kDigitTop[ci], px1, kDigitTop[ci] + kHeroRows - 1);
    if (step_col >= 0 && cell.sweep) {
        const hm::Strip s = hm::strip_of(step_col, step_n);
        for (int i = 0; i < s.n; i++) hm::paint(cell, s.r[i].x0, g.t0, s.r[i].x1, g.t1);
    }
}

// ---- lower cell: band and info block -----------------------------------------------------------
// One compositor cell (y144..239): the third rule, the band's divider and split line, the W / Wh / Ah numbers and the two
// info rows are items over the base colour, so a text repaint composes the structure under it again (the landscape
// mapping makes a native glyph box taller than its logical row; a field that cleared its box would cut the rule below).
// Every row of the info block carries data: row 1 is the USB link, or CC1/CC2 outside a live session, or D+/D- while the
// link is still unknown; row 2 is the protocol and contract summary.

namespace {

uint32_t sig_items(uint32_t h, std::span<const hm::Item> items) {
    for (const hm::Item &i : items) {
        h = mix_u32(h, static_cast<uint32_t>(i.x) | static_cast<uint32_t>(i.y) << 16);
        h = mix_u32(h, i.color);
        h = mix(h, i.text);
    }
    return h;
}

}  // namespace

void HomePage::paint_lower(const Frame &f) {
    const hm::Mode m = f.mode;
    const bool live = m == hm::Mode::Live, stale = m == hm::Mode::Stale;

    // ---- the texts
    char eb[2][12];
    hm::DecSplit wh{}, ah{};
    if (m == hm::Mode::NoMeter) {
        wh = {"--", {}};
        ah = {"--", {}};
    } else {
        const int nw = hm::fmt_energy(eb[0], ctx_.meter.mwh());
        const int na = hm::fmt_energy(eb[1], ctx_.meter.mah());
        wh = hm::split_decimal({eb[0], static_cast<size_t>(nw)});
        ah = hm::split_decimal({eb[1], static_cast<size_t>(na)});
    }

    // Row 1: the USB data link while live; otherwise the measured lines (CC1/CC2 at rest, D+/D- while the link is unknown).
    hm::LinesText lt;
    std::string_view row1_tag = "USB", row1_val;
    uint16_t row1_tag_col = (m == hm::Mode::Idle || m == hm::Mode::NoMeter) ? kFaint : kDim;
    uint16_t row1_val_col = kDim;
    char ub[usblink::kUiMaxChars + 1];
    char fit[kFitBuf];
    const int32_t dp = ctx_.analog ? ctx_.analog->dp_mv : -1, dm = ctx_.analog ? ctx_.analog->dm_mv : -1;
    bool link_known = false;
    if (live) {
        const usblink::Link ul = fusb302::usb_link(dp, dm);
        const size_t len = usblink::format_ui(ul, ub);
        const std::string_view s{ub, len};
        if (!s.empty() && s != "Unkn" && s != "--") {
            link_known = true;
            row1_val = len > static_cast<size_t>(kUsbChars) ? fit_data(kSmall, s, kUsbChars * kSmall.adv, fit) : s;
            row1_val_col = ul.conf == usblink::Confidence::Negotiated ? kText
                         : (ul.conf == usblink::Confidence::Advertised || ul.conf == usblink::Confidence::Hint) ? kDim : kFaint;
        }
    }
    if (!link_known) {
        if (live) hm::format_lines(true, dp, dm, lt);
        else hm::format_lines(false, ctx_.analog ? ctx_.analog->cc1_mv : -1, ctx_.analog ? ctx_.analog->cc2_mv : -1, lt);
        row1_tag = lt.tag.view();
        row1_val = lt.value.view();
        row1_val_col = stale ? kStale : kDim;
    }
    char el[8];
    const size_t eln = live ? static_cast<size_t>(hm::fmt_elapsed(el, ctx_.meter.elapsed_s(f.now)))
                     : stale ? static_cast<size_t>(hm::fmt_elapsed(el, ctx_.last.elapsed_s)) : 0;

    // Row 2: protocol tag, value, role.
    hm::ProtoText pt;
    const bool starting = m == hm::Mode::NoMeter && hm::meter_starting(false, f.now);
    if (m == hm::Mode::NoMeter) pt.value.put(starting ? "Power meter starting" : "Power meter not found");
    else hm::format_proto(f.proto, pt);
    const bool known = (live || stale) && f.proto.kind != hm::ProtoKind::None;
    const uint16_t tcol = known ? kDim : kFaint;
    uint16_t vcol = m == hm::Mode::NoMeter ? (starting ? kDim : kError)
                  : stale ? kStale
                  : (live && f.proto.kind != hm::ProtoKind::None && f.proto.kind != hm::ProtoKind::Detecting
                     && f.proto.kind != hm::ProtoKind::Unknown) ? kText : kDim;
    // Crossed CC through the K2 (pd::Link::cc_cross, VBUS dark): row 2 tells the user to flip a cable instead of "No source".
    // The value is 17 characters wide at most, hence the short texts.
    const raguard::CrossKind cross = (m == hm::Mode::Idle || m == hm::Mode::Stale) ? ctx_.ui->link().cc_cross() : raguard::CrossKind::None;
    if (cross != raguard::CrossKind::None) {
        pt.value.clear();
        pt.value.put(cross == raguard::CrossKind::Crossed ? "CROSSED: flip one" : "Flip a cable?");
        vcol = cross == raguard::CrossKind::Crossed ? kError : kCaution;
    }
    const std::string_view role = m == hm::Mode::NoMeter ? "" : fusb302::self_powered() ? "SNK" : "PAS";

    // ---- the items: structure first, then the six zones in kZones order
    hm::Item it[20];
    int n = 0;
    int zone_end[6];
    auto add = [&](const hm::Item &item) {
        if (item.kind == hm::ItemKind::Text && item.text.empty()) return;
        it[n++] = item;
    };
    // A fixed-width field: right-aligned numbers start where their cell does, so a text never needs padding.
    auto field = [&](const Font &font, int16_t x, int chars, bool right, int16_t top, std::string_view s, uint16_t col) {
        const int pad = right && static_cast<int>(s.size()) < chars ? chars - static_cast<int>(s.size()) : 0;
        add(hm::text(font, static_cast<int16_t>(x + pad * font.adv), top, s, col));
    };
    add(hm::fill(0, hm::kRule3Y, hm::kW, 2, kRule));
    add(hm::fill(hm::kBandMidX, hm::kBandY0, 1, hm::kBandY1 - hm::kBandY0 + 1, kRule));
    add(hm::fill(hm::kBandMidX + 1, hm::kBandSplitY, hm::kW - hm::kBandMidX - 1, 1, kRule));
    int zone_start[6];
    zone_start[0] = n;
    add(hm::text(kMid, 103, 164, "W", tone(m, kWatt)));
    field(kBig, 4, kWattChars, true, 153, f.watt, tone(m, kWatt));
    zone_end[0] = n;
    zone_start[1] = n;
    add(hm::text(kSmall, 199, 152, "Wh", tone(m, kEnergy)));
    field(kMid, 124, kEnergyChars, true, 145, wh.int_part, tone(m, kEnergy));
    field(kMid, 160, kEnergyChars, false, 145, wh.rest, tone(m, kEnergy));
    zone_end[1] = n;
    zone_start[2] = n;
    add(hm::text(kSmall, 199, 177, "Ah", tone(m, kCharge)));
    field(kMid, 124, kEnergyChars, true, 170, ah.int_part, tone(m, kCharge));
    field(kMid, 160, kEnergyChars, false, 170, ah.rest, tone(m, kCharge));
    zone_end[2] = n;
    zone_start[3] = n;
    add(hm::text(kSmall, hm::kInfoTagX, kRowTop[0], row1_tag, row1_tag_col));
    add(hm::text(kSmall, hm::kInfoValX, kRowTop[0], row1_val, row1_val_col));
    zone_end[3] = n;
    zone_start[4] = n;
    field(kSmall, kElX, kElChars, true, kRowTop[0], {el, eln}, kDim);
    zone_end[4] = n;
    zone_start[5] = n;
    add(hm::text(kSmall, hm::kInfoTagX, kRowTop[1], pt.tag.view(), tcol));
    add(hm::text(kSmall, hm::kInfoValX, kRowTop[1], pt.value.view(), vcol));
    add(hm::text(kSmall, kRoleX, kRowTop[1], role, kDim));
    zone_end[5] = n;

    hm::Cell cell;
    cell.geom = {hm::kBandY0, 239, 0, 0};
    cell.bg = kBg;
    cell.items = std::span<const hm::Item>(it, static_cast<size_t>(n));

    uint32_t sig[6];
    for (int z = 0; z < 6; z++)
        sig[z] = sig_items(mix_u32(mix_init(), static_cast<uint32_t>(m)),
                           cell.items.subspan(static_cast<size_t>(zone_start[z]), static_cast<size_t>(zone_end[z] - zone_start[z])));
    if (!lower_ok_) {   // a new state: the whole cell
        lower_ok_ = true;
        for (int z = 0; z < 6; z++) lower_sig_[z] = sig[z];
        hm::paint(cell, 0, hm::kBandY0, hm::kW - 1, 239);
        return;
    }
    for (int z = 0; z < 6; z++) {
        if (sig[z] == lower_sig_[z]) continue;
        lower_sig_[z] = sig[z];
        hm::paint(cell, kZones[z].x0, kZones[z].y0, kZones[z].x1, kZones[z].y1);
    }
}

// ---- statics ---------------------------------------------------------------------------------

// Everything that depends on the mode only. Runs after an invalidate and on every mode change.
void HomePage::paint_statics() {
    statics_ok_ = true;
    lower_ok_ = false;   // the next paint_lower() repaints the whole lower cell
    draw_rules();
    hd_[0].init(kHero, kHeroX, kDigitTop[0], 5);
    hd_[1].init(kHero, kHeroX, kDigitTop[1], 5);
}

// ---- update ----------------------------------------------------------------------------------

void HomePage::update(uint32_t now_ms) {
    Frame f;
    f.now = now_ms;
    f.mode = !ctx_.pm_online ? hm::Mode::NoMeter
           : ctx_.pm.vbus_mv >= 1000 ? hm::Mode::Live
           : ctx_.stale() ? hm::Mode::Stale : hm::Mode::Idle;
    if (static_cast<uint8_t>(f.mode) != mode_) {
        const bool keep_sweep = mode_ == static_cast<uint8_t>(hm::Mode::Live) && f.mode == hm::Mode::Stale;
        mode_ = static_cast<uint8_t>(f.mode);
        thr_.reset();
        statics_ok_ = false;
        if (!keep_sweep) sweep_ok_ = false;
    }
    sample_values(f, now_ms);
    if (!statics_ok_) paint_statics();
    int step_col = -1;
    int step_n = 0;
    update_sweep(f, now_ms, step_col, step_n);
    for (int i = 0; i < 2; i++) {
        int16_t px0 = 0, px1 = 0;
        const bool changed = hd_[i].set(f.hero[i], px0, px1);
        paint_cell(i, f, step_col, step_n, changed, px0, px1);
    }
    paint_lower(f);
}

}  // namespace k2
