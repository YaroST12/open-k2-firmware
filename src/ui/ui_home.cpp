// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home 2c page (user decision 2026-11-12): the hero cell (W, V or A at 64 px; Long Back cycles it), the ledger rows with
// the two other quantities at 48 px, the two right-column cards the cycler picks, and the two-row info block. Every
// quantity cell carries a patient-monitor sweep. The pure parts live in home_model / home_cards / home_proto, the
// compositor in home_cell; this file decides what to repaint (zone signatures) and feeds them.
#include "k2/ui/ui_pages.hpp"

#include <span>
#include <string_view>
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/pd/link.hpp"
#include "k2/pd/diag_live.hpp"
#include "k2/pd/diag_screen.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/ui/home_proto.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_emarker.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2 {

namespace hm = home;

namespace {

// Live colour, stale grey, or idle faint.
constexpr uint16_t tone(hm::Mode m, uint16_t live) { return m == hm::Mode::Live ? live : m == hm::Mode::Stale ? kStale : kFaint; }

// ---- quantity cells (slot 0 = hero, 1 / 2 = ledger rows) ---------------------------------------
constexpr int16_t kHeroDigitX = 38, kHeroDigitTop = 25, kHeroUnitX = 200, kHeroUnitTop = 44;   // baseline 63
constexpr int16_t kHeroRows = 39;                     // font_64 line height
constexpr int16_t kLedDigitX = 8, kLedUnitX = 129;    // 48 px digits right-aligned at x127, unit font 16
constexpr int16_t kLedBase[2] = {129, 186};
constexpr int16_t kLedRows = 30;                      // font_48 line height
constexpr int kDigitChars = 5;
constexpr int16_t kPillRight = 222, kPillY = 1, kPillBoxH = 17;
constexpr int16_t kHeroArrowX = 8, kHeroArrowY = 33;  // A as the hero: the 22 px arrow left of the digits
constexpr int16_t kLedArrowX = 50, kLedArrowDy = 9, kLedArrowBox = 13;
constexpr int16_t kHeroLabelX = 10, kLedLabelX = 6;
constexpr int16_t kMaxRightHero = 219, kMaxRightLed = 135;
static_assert(kHeroDigitX + kDigitChars * 32 - 1 == 197, "hero digits end at x197");
static_assert(kLedDigitX + kDigitChars * 24 - 1 == 127, "ledger digits end at x127");

// ---- cards (x141..223) -------------------------------------------------------------------------
constexpr int16_t kCardL = 146, kCardR = 220;          // text column: labels from x146, values right-aligned at x220
constexpr int16_t kDecX = 182, kEnergyUnitX = 208;     // Wh / Ah: decimal column, unit column
constexpr int16_t kDotX = 218;                         // the attention dot, 5 x 5 against the top-right corner
static_assert(kDotX + 5 - 1 <= hm::kW - 2, "the attention dot stays inside the card");

// ---- info block ----------------------------------------------------------------------------------
constexpr int16_t kInfoRow1Top = 201, kInfoRow2Top = 223;
constexpr int kDiagChars = 16, kElChars = 5;
constexpr int16_t kElX = 180, kRoleX = 196;
constexpr int16_t kRowTop[2] = {kInfoRow1Top, kInfoRow2Top};
// Row 1's diagnosis word (hm::format_diag) and its tag fit between the tag column and the elapsed time.
static_assert(pd::diag::screen::kLabelChars <= static_cast<std::size_t>(kDiagChars), "the diagnosis word runs into the elapsed time");
static_assert(hm::kInfoValX + kDiagChars * static_cast<int>(kSmallAdvPx) <= kElX, "the diagnosis word overlaps the elapsed time");
static_assert(hm::kInfoTagX + hm::kDiagTag.size() * kSmallAdvPx <= static_cast<unsigned>(hm::kInfoValX), "the Diag tag runs into its value");

// Repaint zones of the info cell (logical, inclusive). A zone is wider and lower than its fields on purpose: a glyph
// keeps its native pixel size while the landscape mapping squeezes the rows, so an old text can reach a few pixels
// past the logical box of the new one, and the rect repainted for a change has to cover all of it.
struct Zone { int16_t x0, y0, x1, y1; };
constexpr Zone kInfoZones[3] = {
    {0, 196, 186, 226},     // info row 1: tag and value
    {176, 196, 223, 226},   // info row 1: elapsed time
    {0, 220, 223, 239},     // info row 2: protocol, value, role
};
constexpr int kCatchUpSteps = 3;                      // late sweep steps taken in one update
constexpr int kReloadLagSteps = 8;                   // a sweep this many steps behind is rebuilt from the trend

constexpr int32_t abs32(int32_t v) { return v < 0 ? (v == INT32_MIN ? INT32_MAX : -v) : v; }

constexpr trend::Qty trend_qty(hm::Qty q) { return q == hm::Qty::W ? trend::Qty::W : q == hm::Qty::V ? trend::Qty::V : trend::Qty::A; }
constexpr std::string_view qty_label(hm::Qty q) { return q == hm::Qty::W ? "POWER" : q == hm::Qty::V ? "VBUS" : "IBUS"; }
constexpr std::string_view qty_unit(hm::Qty q) { return q == hm::Qty::W ? "W" : q == hm::Qty::V ? "V" : "A"; }
constexpr char qty_unit_char(hm::Qty q) { return q == hm::Qty::W ? 'W' : q == hm::Qty::V ? 'V' : 'A'; }
// Quantity fields: a black field, the quantity's own colour for its digits, and the trend as a dark tint column under a
// brighter 1 px level line (the look of the pre-Fieldbook Home, restored 2026-11-13). Stale: the grey, idle: faint.
uint16_t qty_color(hm::Qty q) { return q == hm::Qty::W ? kWatt : q == hm::Qty::V ? kVolt : kAmp; }
uint16_t qty_tint(hm::Qty q) { return q == hm::Qty::W ? kWattT : q == hm::Qty::V ? kVoltT : kAmpT; }
uint16_t qty_line(hm::Qty q) { return q == hm::Qty::W ? kWattL : q == hm::Qty::V ? kVoltL : kAmpL; }
// A field's look in one mode: the ground, the digit ink, the label ink. An alarm over the limit is white on red.
struct FieldLook { uint16_t fill, ink, label; };
FieldLook field_look(hm::Mode m, hm::Qty q, bool over, bool near_) {
    if (over) return {kError, kAlarmInk, kAlarmInk};
    const uint16_t ink = near_ ? kCaution : m == hm::Mode::Live ? qty_color(q) : m == hm::Mode::Stale ? kStale : kFaint;
    return {kBg, ink, kDim};
}

// The geometry of a quantity slot: the content rows (the rule under it belongs to the cell too).
struct Slot { int16_t top, bottom, cell_bottom, x1; };
constexpr Slot slot_of(int s) {
    return s == 0 ? Slot{hm::kHeroY0, hm::kHeroY1, static_cast<int16_t>(hm::kRule1Y + 1), static_cast<int16_t>(hm::kW - 1)}
         : s == 1 ? Slot{hm::kRowY0[0], hm::kRowY1[0], hm::kRowRuleY, hm::kVRuleX}
                  : Slot{hm::kRowY0[1], hm::kRowY1[1], hm::kRowY1[1], hm::kVRuleX};
}

uint32_t sig_items(uint32_t h, std::span<const hm::Item> items) {
    for (const hm::Item &i : items) {
        h = mix_u32(h, static_cast<uint32_t>(static_cast<uint16_t>(i.x)) | static_cast<uint32_t>(static_cast<uint16_t>(i.y)) << 16);
        h = mix_u32(mix_u32(h, i.color), static_cast<uint32_t>(i.kind) | static_cast<uint32_t>(i.aux) << 8);
        h = mix(h, i.text);
    }
    return h;
}

}  // namespace

// What one update decides before it draws: the mode, the protocol, the alarms and the display texts.
struct HomePage::Frame {
    hm::Mode mode = hm::Mode::Idle;
    hm::ProtoInfo proto{};
    uint32_t max[3] = {};       // per hm::Qty: the contract maximum in milli-units (0 = unknown: no bar, no alarm)
    hm::Alarm alarm[3] = {};    // per hm::Qty (V never alarms)
    uint8_t pin = 0;            // fusb302::pin_rd(): a held Rd pin shows as the "Rd pin CCx" badge
    uint32_t now = 0;
    char text[3][8] = {};       // per hm::Qty: five characters
};

void HomePage::draw_static() {
    invalidate();   // the layout is painted by the first update (it knows the mode)
}

void HomePage::invalidate() {
    statics_ok_ = false;
    info_ok_ = false;
    for (bool &v : cell_valid_) v = false;
    for (bool &v : card_valid_) v = false;
    for (int16_t &y : slice_y_) y = -1;
    for (int8_t &c : painted_cursor_) c = -1;
    slice_dirty_ = 0;
    rr_slot_ = 0;
    thr_.reset();   // the digits repaint at once; mode_ and the sweeps stay (a capture re-renders strips of one frame)
    for (hm::DigitField &d : hd_) d.invalidate();
}

PanelLabels HomePage::labels() const {
    PanelLabels l = base_labels();                                        // Up / Dn flip the faces (long: the trend span)
    l.b[static_cast<int>(Button::Back)] = {"", "Hero", "", false};       // no level above Flow; long: the hero cycles W -> V -> A
    l.b[static_cast<int>(Button::Ok)] = {"Bench", "Reset", "", false};   // short: the Bench; long: energy reset
    return l;
}

bool HomePage::on_event(Event e) {
    if (e.button == Button::Ok && e.press == Press::Long) {
        ctx_.ui->run_action(Action::EnergyReset, ctx_.now);   // energy, peaks, sag and the card cycler
        return true;
    }
    if ((e.button == Button::Plus || e.button == Button::Minus) && e.press == Press::Long) {   // the trend span 10s / 60s / 10m
        window_ = e.button == Button::Plus ? trend::next(window_) : trend::prev(window_);
        sweep_ok_ = false;   // the next update reloads the sweeps for the new window and repaints the cells
        return true;
    }
    if (e.button == Button::Back && e.press == Press::Long) {
        ctx_.ui->set_home_hero(hm::next(ctx_.keep.home_hero));
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
    ps.vooc_live = ctx.vooc.seen;
    ps.ibus_ma = pm.current_ma;
    return derive_proto(ps);
}

home::ProtoInfo Ui::proto_now() const { return home::live_proto(ctx_, ctx_.pm); }

void HomePage::sample_values(Frame &f, uint32_t now_ms) {
    const bool live = f.mode == hm::Mode::Live, stale = f.mode == hm::Mode::Stale;
    f.pin = fusb302::pin_rd();
    const power::Metrics &pm = stale ? ctx_.last.pm : ctx_.pm;
    bool fresh = false;
    if (live) {
        if (thr_.pass(now_ms)) {   // the display filter: V, A, W and the card values change at most 4 times a second, as one set
            v_mv_ = abs32(pm.vbus_mv);
            a_ma_ = abs32(pm.current_ma);
            w_mw_ = abs32(pm.power_mw);
            flow_ = pm.flow;
            fresh = true;
        }
        f.proto = hm::live_proto(ctx_, pm);
    } else if (stale) {
        v_mv_ = abs32(pm.vbus_mv);
        a_ma_ = abs32(pm.current_ma);
        w_mw_ = abs32(pm.power_mw);
        flow_ = pm.flow;
        f.proto = ctx_.last.proto;
        fresh = true;
    } else {
        v_mv_ = a_ma_ = w_mw_ = 0;
        flow_ = power::Flow::Idle;
        fresh = true;
    }
    const uint32_t cmw = hm::contract_mw(f.proto);
    f.max[static_cast<int>(hm::Qty::W)] = cmw;
    f.max[static_cast<int>(hm::Qty::V)] = f.proto.max_mv;
    f.max[static_cast<int>(hm::Qty::A)] = f.proto.ma;
    if (live) {
        f.alarm[static_cast<int>(hm::Qty::W)] = hm::alarm_of(static_cast<uint32_t>(w_mw_), cmw);
        f.alarm[static_cast<int>(hm::Qty::A)] = hm::alarm_of(static_cast<uint32_t>(a_ma_), f.proto.ma);
    }
    if (f.mode == hm::Mode::NoMeter) {
        for (char (&h)[8] : f.text) { h[0] = h[1] = '-'; h[2] = 0; }
    } else {
        hm::fmt_hero(f.text[static_cast<int>(hm::Qty::W)], w_mw_);
        hm::fmt_hero(f.text[static_cast<int>(hm::Qty::V)], v_mv_);
        hm::fmt_hero(f.text[static_cast<int>(hm::Qty::A)], a_ma_);
    }
    // The card values, as one set with the digits.
    if (fresh) {
        const hm::Extremes &x = ctx_.extremes;
        card_.peak_mw = x.peak_mw();
        card_.peak_ma = x.peak_ma();
        card_.has_sag = x.has_sag();
        card_.sag_bp = x.sag_bp();
        card_.sag_level_mv = x.sag_level_mv();
        card_.drop = hm::drop_of(live, ctx_.cable.has_idle(), ctx_.cable.idle_mv(), ctx_.cable.idle_ma(), v_mv_, a_ma_);
        const std::optional<adc::Reading> &an = ctx_.analog;
        card_.lines[0] = an ? an->cc1_mv : -1;
        card_.lines[1] = an ? an->cc2_mv : -1;
        card_.lines[2] = an ? an->dp_mv : -1;
        card_.lines[3] = an ? an->dm_mv : -1;
    }
}

// ---- sweeps ----------------------------------------------------------------------------------
// One sweep per slot: the hero's 110 columns, the ledger rows' 66 over the same window. The hero steps on the clock; a
// ledger row steps when the hero cursor crosses one of its columns (cursor x 66 / 110), so all three show one span.

void HomePage::update_sweep(const Frame &f, uint32_t now_ms, int (&step_col)[3], int (&step_n)[3]) {
    for (int i = 0; i < 3; i++) { step_col[i] = -1; step_n[i] = 0; }
    if (f.mode != hm::Mode::Live && f.mode != hm::Mode::Stale) return;
    const hm::Qty hero = ctx_.keep.home_hero;
    const hm::Ledger led = hm::ledger_of(hero);
    const hm::Qty q[3] = {hero, led.row[0], led.row[1]};
    uint32_t scale[3];
    int span[3];
    uint32_t sig = mix_u32(mix_init(), static_cast<uint32_t>(hero));
    for (int i = 0; i < 3; i++) {
        const uint32_t mx = f.max[static_cast<int>(q[i])];
        scale[i] = q[i] == hm::Qty::W ? hm::scale_w10(mx) : q[i] == hm::Qty::V ? hm::scale_v10(mx) : hm::scale_a10(mx);
        const Slot s = slot_of(i);
        span[i] = hm::cell_geom(s.top, s.bottom, q[i] != hm::Qty::V && mx > 0).span();
        sig = mix_u32(mix_u32(sig, scale[i]), static_cast<uint32_t>(span[i]));
    }
    const uint32_t step = hm::step_ms(static_cast<int>(window_));
    if (sweep_ok_ && (sig != sweep_scale_ || (f.mode == hm::Mode::Live && now_ms - sweep_t_ >= kReloadLagSteps * step)))
        sweep_ok_ = false;
    if (!sweep_ok_) {   // rebuild the sweeps from the trend: newest sample at the column before the cursor
        uint16_t col[hm::kCols];
        uint8_t lv[hm::kCols];
        for (int i = 0; i < 3; i++) {
            const int n = i == 0 ? hm::kCols : hm::kLedgerCols;
            sw_[i].n = static_cast<uint8_t>(n);
            ctx_.trend.columns(window_, trend_qty(q[i]), col, n);
            for (int k = 0; k < n; k++) lv[k] = static_cast<uint8_t>(hm::sweep_rows(col[k], scale[i], span[i]));
            sw_[i].load(std::span<const uint8_t>(lv, static_cast<size_t>(n)));
        }
        sweep_ok_ = true;
        sweep_t_ = now_ms;
        sweep_scale_ = sig;
        sweep_epoch_++;
        return;
    }
    const bool replay = now_ms == sweep_seen_;   // a capture re-renders the same frame: no second step
    sweep_seen_ = now_ms;
    // One hero column per step. An update catches up on a few late steps at once (one repaint strip covers them);
    // a longer lag reloads the sweeps above.
    int hero_n = 0;
    while (f.mode == hm::Mode::Live && !replay && hero_n < kCatchUpSteps && now_ms - sweep_t_ >= step) {
        sweep_t_ += step;
        const uint8_t lv = static_cast<uint8_t>(hm::sweep_rows(ctx_.trend.latest(window_, trend_qty(q[0])), scale[0], span[0]));
        const int first = sw_[0].step(lv);
        if (step_n[0] == 0) step_col[0] = first;
        step_n[0]++;
        hero_n++;
        const int target = sw_[0].cursor * hm::kLedgerCols / hm::kCols;
        for (int i = 1; i < 3; i++) {
            if (sw_[i].cursor == target) continue;
            const uint8_t l = static_cast<uint8_t>(hm::sweep_rows(ctx_.trend.latest(window_, trend_qty(q[i])), scale[i], span[i]));
            const int fc = sw_[i].step(l);
            if (step_n[i] == 0) step_col[i] = fc;
            step_n[i]++;
        }
    }
}

// ---- quantity cells ----------------------------------------------------------------------------

struct HomePage::QtyHead {
    StrBuf<12> ptxt, mtxt;
    StrBuf<20> atxt;
};

// At most 4 head items: the warning triangle and the alarm line (or the label and the span), then the pill's plate and
// text (or the max badge).
uint32_t HomePage::qty_head(int si, hm::Qty q, const Frame &f, QtyHead &h, hm::Item *it, int &n) {
    const bool hero = si == 0;
    const Slot s = slot_of(si);
    const hm::Mode m = f.mode;
    const bool shown = m == hm::Mode::Live || m == hm::Mode::Stale;
    const int qi = static_cast<int>(q);
    const hm::Alarm al = f.alarm[qi];
    const bool over = al == hm::Alarm::Over, near_ = al == hm::Alarm::Near;
    const uint32_t mx = f.max[qi];
    const FieldLook look = field_look(m, q, over, near_);
    const uint16_t label = look.label;
    const char uc = qty_unit_char(q);
    // A W maximum shows in whole watts ("max 36W", "OVER 36W LIMIT"); the alarm itself compares the exact figure.
    const uint32_t shown_max = q == hm::Qty::W ? (mx + 500) / 1000 * 1000 : mx;
    StrBuf<12> &ptxt = h.ptxt, &mtxt = h.mtxt;
    StrBuf<20> &atxt = h.atxt;
    const int16_t lx = hero ? kHeroLabelX : kLedLabelX;
    const int16_t ly = static_cast<int16_t>(s.top + 3);
    uint32_t head = mix_init();
    const int16_t right_x = hero ? kMaxRightHero : kMaxRightLed;
    if (over) {   // the whole field is the alarm: triangle and "OVER 5A LIMIT" in white where the label was
        it[n++] = hm::warn(static_cast<int16_t>(lx - 1), static_cast<int16_t>(s.top + 2), kAlarmInk, kError);
        hm::alarm_text(al, static_cast<uint32_t>(q == hm::Qty::W ? w_mw_ : a_ma_), shown_max, atxt, uc, !hero);
        it[n++] = hm::text(kLabelFont, static_cast<int16_t>(lx + 13), ly, atxt.view(), kAlarmInk);
    } else {
        const std::string_view lab = qty_label(q);
        it[n++] = hm::text(kLabelFont, lx, ly, lab, label);
        if (hero && shown)
            it[n++] = hm::text(kLabelFont, static_cast<int16_t>(lx + (lab.size() + 1) * kLabelFont.adv), ly, trend::label(window_), kFaint);
        if (near_) {   // "93% OF 100W" (hero) or "92%" (ledger) as an orange chip at the right end of the label line
            hm::alarm_text(al, static_cast<uint32_t>(q == hm::Qty::W ? w_mw_ : a_ma_), shown_max, atxt, uc, !hero);
            const TextBounds tb = badge_text_bounds(kLabelFont, atxt.view());
            const int16_t bw = static_cast<int16_t>(tb.w + 8);
            const int16_t bx = static_cast<int16_t>(right_x + 1 - bw);
            const int16_t by = static_cast<int16_t>(s.top + 2);
            it[n++] = hm::fill(bx, by, bw, 13, kCaution);
            it[n++] = hm::badge_text(kLabelFont, bx, by, bw, 13, atxt.view(), kSpotInk);
        }
    }
    hm::Pill pill = hm::Pill::None;
    if (hero) {
        // The alarm line replaces the OVER pill; the near chip takes the right end, so no pill beside it.
        pill = near_ ? hm::Pill::None : hm::pill_of(m, hm::Alarm::None, f.pin);
        switch (pill) {
        case hm::Pill::Last:   ptxt.put("LAST ").put_age(ctx_.live.age_s(f.now)); break;
        case hm::Pill::NoVbus: ptxt.put("NO VBUS"); break;
        case hm::Pill::Pin:    ptxt.put("Rd pin CC").put_char(static_cast<char>('0' + f.pin)); break;
        case hm::Pill::Over: case hm::Pill::None: break;
        }
        if (pill != hm::Pill::None) {   // chips: LAST grey, NO VBUS on the surface, a held pin orange
            const uint16_t fillc = pill == hm::Pill::NoVbus ? kSurface : pill == hm::Pill::Pin ? kCaution : kStale;
            const uint16_t inkc = pill == hm::Pill::NoVbus ? kDim : kBg;
            const TextBounds tb = badge_text_bounds(kSmall, ptxt.view());
            const int16_t bw = static_cast<int16_t>(tb.w + 10);
            const int16_t x0 = static_cast<int16_t>(kPillRight - bw);
            it[n++] = hm::fill(x0, kPillY, bw, kPillBoxH, fillc);
            it[n++] = hm::badge_text(kSmall, x0, kPillY, bw, kPillBoxH, ptxt.view(), inkc);
        }
    }
    if (pill == hm::Pill::None && al == hm::Alarm::None && hm::max_text(m, shown_max, uc, mtxt)) {
        const int16_t mn = static_cast<int16_t>(mtxt.view().size());
        it[n++] = hm::text(kLabelFont, static_cast<int16_t>(right_x + 1 - 6 * mn), ly, mtxt.view(), label);
    }
    head = mix_u32(mix_u32(head, static_cast<uint32_t>(al)), static_cast<uint32_t>(pill));
    return mix(mix(mix(head, ptxt.view()), mtxt.view()), atxt.view());
}

bool HomePage::paint_cell_sliced(int slot_id, uint32_t base_sig, const home::Cell &cell,
                                 int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    PaintBudget *const bg = ctx_.budget;
    if (!bg) {
        slice_y_[slot_id] = -1;
        home::paint(cell, x0, y0, x1, y1);
        return true;
    }
    if (slice_y_[slot_id] >= 0 && slice_sig_[slot_id] != base_sig) slice_y_[slot_id] = -1;
    int16_t y = slice_y_[slot_id];
    if (y < y0 || y > y1) y = y0;
    slice_sig_[slot_id] = base_sig;
    const int w = static_cast<int>(x1 - x0 + 1);
    const int max_rows = std::max(1, bg->left() / w);
    const int rem_rows = static_cast<int>(y1 - y + 1);
    const int rows = std::min(max_rows, rem_rows);
    const int cost = rows * w;
    if (!bg->admit(cost, true)) {
        slice_y_[slot_id] = y;
        return false;
    }
    const int16_t y_end = static_cast<int16_t>(y + rows - 1);
    home::paint(cell, x0, y, x1, y_end);
    if (y_end < y1) {
        slice_y_[slot_id] = static_cast<int16_t>(y_end + 1);
        bg->defer();
        return false;
    }
    slice_y_[slot_id] = -1;
    return true;
}

void HomePage::paint_qty(int si, hm::Qty q, const Frame &f, int step_col, int step_n) {
    const bool hero = si == 0;
    const Slot s = slot_of(si);
    const hm::Mode m = f.mode;
    const bool live = m == hm::Mode::Live, shown = live || m == hm::Mode::Stale;
    const int qi = static_cast<int>(q);
    const hm::Alarm al = f.alarm[qi];
    const bool over = al == hm::Alarm::Over, near_ = al == hm::Alarm::Near;
    const uint32_t mx = f.max[qi];
    const bool bar = q != hm::Qty::V && mx > 0;
    const hm::CellGeom g0 = hm::cell_geom(s.top, s.bottom, bar);
    const FieldLook look = field_look(m, q, over, near_);
    const uint16_t ink = look.ink;   // digits and unit
    int16_t px0 = 0, px1 = 0;
    const hm::DigitField prev_hd = hd_[si];
    const bool digits_changed = hd_[si].set(f.text[qi], px0, px1);   // before the items: the digit item views hd_'s text

    // At most 12 items: 2 gaps (ledger) or gap + key (hero), 4 head items, the unit, 3 bar items, the arrow, the digits.
    QtyHead texts;
    hm::Item it[12];
    int n = 0;
    // Structure: the rules this cell owns.
    if (hero) {
        it[n++] = hm::fill(0, hm::kRule1Y, hm::kW, 2, kRule);
    } else {
        it[n++] = hm::fill(hm::kVRuleX, s.top, 1, static_cast<int16_t>(s.cell_bottom - s.top + 1), kRule);
        if (si == 1) it[n++] = hm::fill(0, hm::kRowRuleY, hm::kVRuleX, 1, kRule);
    }
    if (hero)   // the 4 px status edge in the field's ink (white on the alarm red)
        it[n++] = hm::fill(0, s.top, 4, static_cast<int16_t>(s.bottom - s.top + 1), over ? kAlarmInk : ink);
    const uint32_t head = qty_head(si, q, f, texts, it, n);

    // Unit, bar, arrow, digits.
    const int16_t dtop = hero ? kHeroDigitTop : static_cast<int16_t>(kLedBase[si - 1] - 30);
    if (hero) it[n++] = hm::text(kMid, kHeroUnitX, kHeroUnitTop, qty_unit(q), ink);
    else it[n++] = hm::text(kSmall, kLedUnitX, static_cast<int16_t>(kLedBase[si - 1] - 12), qty_unit(q), ink);
    const hm::BarGeom bg = hero ? hm::kHeroBar : hm::ledger_bar(si - 1);
    uint32_t bar_sig = 0, arrow_sig = 0;
    if (bar && shown) {   // contract bar: track, fill, 90 % tick
        const uint32_t abs = static_cast<uint32_t>(q == hm::Qty::W ? w_mw_ : a_ma_);
        const int fw = hm::bar_fill_w(abs, mx, bg);
        // The quantity's colour on a rule-grey track, the 90 % tick in text (dim when stale); orange from 90 %; white on red.
        const uint16_t fc = ink;
        const uint16_t tc = over ? kAlarmInk : m == hm::Mode::Stale ? kDim : kText;
        const uint16_t track = over ? kErrD : kRule;
        it[n++] = hm::fill(bg.x0, bg.y, static_cast<int16_t>(hm::bar_track(bg)), hm::kBarH, track);
        if (fw > 0) it[n++] = hm::fill(bg.x0, bg.y, static_cast<int16_t>(fw), hm::kBarH, fc);
        it[n++] = hm::fill(hm::bar_tick_x(bg), static_cast<int16_t>(bg.y - 2), 1, hm::kBarH + 4, tc);
        bar_sig = mix_u32(mix_u32(mix_u32(1, static_cast<uint32_t>(fw)), fc), tc);
    }
    const int16_t ax = hero ? kHeroArrowX : kLedArrowX;
    const int16_t ay = hero ? kHeroArrowY : static_cast<int16_t>(s.top + kLedArrowDy);
    const int16_t abox = hero ? static_cast<int16_t>(hm::kArrowBox) : kLedArrowBox;
    if (q == hm::Qty::A && shown) {
        const bool dash = flow_ == power::Flow::Idle;
        const uint8_t aux = dash ? hm::dash_dir(ctx_.rotation) : hm::arrow_dir(flow_ == power::Flow::Forward, ctx_.rotation);
        const uint16_t ac = over ? kAlarmInk : dash ? kFaint : live ? kFlow : kStale;   // a resting dash faint, the stale arrow grey
        it[n++] = hm::arrow(ax, ay, aux, ac, hero ? 0 : kLedArrowBox);
        arrow_sig = mix_u32(mix_u32(1, aux), ac);
    }
    it[n++] = hm::text(hero ? kHero : kBig, hero ? kHeroDigitX : kLedDigitX, dtop, hd_[si].view(), ink);

    hm::Cell cell;
    cell.geom = g0;
    cell.geom.bottom = s.cell_bottom;
    cell.x1 = s.x1;
    cell.bg = look.fill;
    cell.sweep = shown && !over && sweep_ok_;
    // The trend: a dark tint column under a brighter 1 px line, live in the quantity's own shades, stale in grey. The
    // cursor is a 1 px rule-coloured gap in it.
    cell.tint = live ? qty_tint(q) : kPanelBg;
    cell.line = live ? qty_line(q) : kFaint;
    cell.cursor_line = kRule;
    cell.sw = &sw_[si];
    cell.items = std::span<const hm::Item>(it, static_cast<size_t>(n));

    uint32_t base = mix_u32(mix_init(), static_cast<uint32_t>(m));
    base = mix_u32(base, (over ? 1u : 0u) | (near_ ? 2u : 0u) | (cell.sweep ? 4u : 0u) | (bar ? 8u : 0u));
    base = mix_u32(mix_u32(base, static_cast<uint32_t>(window_)), sweep_epoch_);
    base = mix_u32(mix_u32(base, ctx_.rotation & 3u), static_cast<uint32_t>(q));

    PaintBudget *const bg_budget = ctx_.budget;
    uint32_t *sig = cell_sig_[si];
    if (!cell_valid_[si] || sig[0] != base) {   // a new state: the whole cell
        if (slice_y_[si] < 0 || slice_sig_[si] != base) {
            painted_cursor_[si] = cell.sweep ? static_cast<int8_t>(sw_[si].cursor) : -1;
            slice_dirty_ &= static_cast<uint8_t>(~(1u << si));
            slice_parts_[si][0] = head;
            slice_parts_[si][1] = arrow_sig;
            slice_parts_[si][2] = bar_sig;
        } else if (digits_changed) {
            slice_dirty_ |= static_cast<uint8_t>(1u << si);
        }
        if (!paint_cell_sliced(si, base, cell, 0, s.top, s.x1, s.cell_bottom)) return;
        sig[0] = base;
        sig[1] = slice_parts_[si][0];
        sig[2] = slice_parts_[si][1];
        sig[3] = slice_parts_[si][2];
        cell_valid_[si] = true;
        if (slice_dirty_ & (1u << si)) {
            slice_dirty_ &= static_cast<uint8_t>(~(1u << si));
            hd_[si].invalidate();
        }
        if (!digits_changed && sig[1] == head && sig[2] == arrow_sig && sig[3] == bar_sig &&
            (!cell.sweep || painted_cursor_[si] == static_cast<int8_t>(sw_[si].cursor)))
            return;
    }
    const bool just_painted_cell = !sig[0];
    (void)just_painted_cell;
    auto abort_delta = [&]() {
        if (digits_changed) hd_[si] = prev_hd;
    };
    if (sig[1] != head) {
        const int cost = static_cast<int>(s.x1 - 3) * 19;
        if (bg_budget && !bg_budget->admit(cost, true)) { abort_delta(); return; }
        sig[1] = head;
        hm::paint(cell, 4, s.top, s.x1, static_cast<int16_t>(s.top + 18));
    }
    if (sig[2] != arrow_sig) {
        const int cost = abox * abox;
        if (bg_budget && !bg_budget->admit(cost, true)) { abort_delta(); return; }
        sig[2] = arrow_sig;
        hm::paint(cell, ax, ay, static_cast<int16_t>(ax + abox - 1), static_cast<int16_t>(ay + abox - 1));
    }
    if (sig[3] != bar_sig) {
        const int cost = static_cast<int>(bg.x1 - bg.x0 + 1) * (hm::kBarH + 4);
        if (bg_budget && !bg_budget->admit(cost, true)) { abort_delta(); return; }
        sig[3] = bar_sig;
        hm::paint(cell, bg.x0, static_cast<int16_t>(bg.y - 2), bg.x1, static_cast<int16_t>(bg.y + hm::kBarH + 1));
    }
    if (cell.sweep) {
        const int cur = static_cast<int>(sw_[si].cursor);
        if (painted_cursor_[si] < 0) painted_cursor_[si] = static_cast<int8_t>(cur);
        else if (painted_cursor_[si] != cur) {
            const int first = painted_cursor_[si];
            const int steps = (cur - first + sw_[si].n) % sw_[si].n;
            const hm::Strip st = hm::strip_of(first, steps, sw_[si].n);
            int cost = 0;
            for (int i = 0; i < st.n; i++)
                cost += static_cast<int>(st.r[i].x1 - st.r[i].x0 + 1) * (g0.t1 - g0.t0 + 1);
            if (bg_budget && !bg_budget->admit(cost, true)) { abort_delta(); return; }
            for (int i = 0; i < st.n; i++)
                hm::paint(cell, st.r[i].x0, g0.t0, st.r[i].x1, g0.t1);
            painted_cursor_[si] = static_cast<int8_t>(cur);
        }
    } else if (step_col >= 0 && cell.sweep) {
        const hm::Strip st = hm::strip_of(step_col, step_n, sw_[si].n);
        for (int i = 0; i < st.n; i++) {
            const int cost = static_cast<int>(st.r[i].x1 - st.r[i].x0 + 1) * (g0.t1 - g0.t0 + 1);
            if (bg_budget) bg_budget->spent += cost;
            hm::paint(cell, st.r[i].x0, g0.t0, st.r[i].x1, g0.t1);
        }
    }
    if (digits_changed) {
        const int cost = static_cast<int>(px1 - px0 + 1) * (hero ? kHeroRows : kLedRows);
        if (bg_budget && !bg_budget->admit(cost, true)) { abort_delta(); return; }
        hm::paint(cell, px0, dtop, px1, static_cast<int16_t>(dtop + (hero ? kHeroRows : kLedRows) - 1));
    }
}

// ---- cards -----------------------------------------------------------------------------------
// One compositor cell per card (x141..223); the upper one owns the rule at y136. A card swap repaints the whole cell,
// a value change only its zone (one per text row).

void HomePage::paint_card(int ci, const Frame &f) {
    const hm::Mode m = f.mode;
    const bool live = m == hm::Mode::Live;
    const hm::CardCycler &cy = ctx_.keep.cards;
    const hm::Card card = ci == 0 ? cy.top() : cy.bottom();
    const int16_t y0 = hm::kRowY0[ci], y1 = hm::kRowY1[ci];
    const int16_t cell_bottom = ci == 0 ? hm::kRowRuleY : y1;
    const bool dot = ci == 0 && cy.forced();

    hm::Item it[14];
    int n = 0;
    struct ZoneY { int16_t a, b; } zy[4] = {};
    int zone_start[5] = {};
    int nz = 0;
    auto add = [&](const hm::Item &item) {
        if (item.kind == hm::ItemKind::Text && item.text.empty()) return;
        it[n++] = item;
    };
    auto zone = [&](int16_t a, int16_t b) { zy[nz] = {static_cast<int16_t>(y0 + a), b < 0 ? y1 : static_cast<int16_t>(y0 + b)}; zone_start[nz++] = n; };
    auto right = [&](const Font &font, int16_t base, std::string_view s, uint16_t col) {
        add(hm::text(font, static_cast<int16_t>(kCardR + 1 - static_cast<int>(s.size()) * font.adv), static_cast<int16_t>(y0 + base - font.ascent), s, col));
    };
    auto label = [&](std::string_view s, int16_t dy) { add(hm::text(kLabelFont, kCardL, static_cast<int16_t>(y0 + dy), s, kDim)); };
    if (ci == 0) add(hm::fill(hm::kCardX0, hm::kRowRuleY, static_cast<int16_t>(hm::kW - hm::kCardX0), 1, kRule));
    if (dot) add(hm::fill(kDotX, static_cast<int16_t>(y0 + 1), 5, 5, kAccent));

    char b0[12], b1[12];
    StrBuf<8> s0, s1, lines[4];
    StrBuf<12> sp, speed;
    switch (card) {
    case hm::Card::Energy: {   // ENERGY title, then Wh over Ah, decimal-aligned on x182
        label("ENERGY", 3);
        hm::DecSplit d[2] = {{"--", {}}, {"--", {}}};
        if (m != hm::Mode::NoMeter) {
            const int nw = hm::fmt_energy(b0, ctx_.meter.mwh());
            const int na = hm::fmt_energy(b1, ctx_.meter.mah());
            d[0] = hm::split_decimal({b0, static_cast<size_t>(nw)});
            d[1] = hm::split_decimal({b1, static_cast<size_t>(na)});
        }
        const uint16_t col[2] = {tone(m, kInk), tone(m, kInk)};   // Fieldbook: card values are ink, never a hue
        const std::string_view unit[2] = {"Wh", "Ah"};
        const int16_t base[2] = {29, 51};
        const int16_t za[2] = {16, 37};
        const int16_t zb[2] = {36, -1};
        for (int r = 0; r < 2; r++) {
            zone(za[r], zb[r]);
            const int16_t top = static_cast<int16_t>(y0 + base[r] - kSmall.ascent);
            add(hm::text(kSmall, static_cast<int16_t>(kDecX - static_cast<int>(d[r].int_part.size()) * kSmall.adv), top, d[r].int_part, col[r]));
            add(hm::text(kSmall, kDecX, top, d[r].rest, col[r]));
            add(hm::text(kLabelFont, kEnergyUnitX, static_cast<int16_t>(y0 + base[r] - kLabelFont.ascent), unit[r], col[r]));
        }
        break;
    }
    case hm::Card::Peak: {
        label("PEAK", 3);
        zone(16, 36);
        if (card_.peak_mw > 0) {
            const int k = hm::fmt_watts(b0, static_cast<int32_t>(card_.peak_mw));
            b0[k] = 'W';
            right(kSmall, 29, {b0, static_cast<size_t>(k + 1)}, tone(m, kInk));
        } else {
            right(kSmall, 29, "--", kFaint);
        }
        zone(37, -1);
        if (card_.peak_ma > 0) {
            const int k = hm::fmt_hero(b1, static_cast<int32_t>(card_.peak_ma));
            b1[k] = 'A';
            right(kSmall, 51, {b1, static_cast<size_t>(k + 1)}, tone(m, kInk));
        } else {
            right(kSmall, 51, "--", kFaint);
        }
        break;
    }
    case hm::Card::Drop: {
        label("DROP", 3);
        zone(15, 31);
        if (card_.drop.state == hm::DropState::Value && card_.drop.mv > 0) {
            const int k = hm::fmt_drop(b0, card_.drop.mv);
            right(kSmall, 27, {b0, static_cast<size_t>(k)}, tone(m, kInk));
        } else {
            right(kSmall, 27, "--", kFaint);
        }
        zone(32, -1);
        label("SAG", 32);
        if (card_.has_sag && card_.sag_bp > 0 && card_.sag_level_mv > 0) {
            const int k = hm::fmt_sag(b1, card_.sag_bp, card_.sag_level_mv);
            right(kSmall, 53, {b1, static_cast<size_t>(k)}, tone(m, kInk));
        } else {
            right(kSmall, 53, "--", kFaint);
        }
        break;
    }
    case hm::Card::Cable: {
        const pd::Emarker &em = fusb302::emarker();
        label("CABLE", 3);
        zone(16, 36);
        if (em.valid) {
            emarker_amps_text(em, s0);
            emarker_volts_text(em, s1);
            sp.put(s0.view()).put_char(' ').put(s1.view());
        }
        right(kSmall, 29, em.valid ? sp.view() : std::string_view("--"), em.valid ? tone(m, kText) : kFaint);
        zone(37, -1);
        if (em.valid) emarker_speed_pill(em, speed);
        right(kLabelFont, 50, speed.view(), kDim);
        break;
    }
    case hm::Card::Lines: {
        static constexpr std::string_view kName[4] = {"CC1", "CC2", "D+", "D-"};
        for (int r = 0; r < 4; r++) {
            const int16_t base = static_cast<int16_t>(11 + 13 * r);
            zone(r == 0 ? 0 : static_cast<int16_t>(base - 10), r == 3 ? -1 : static_cast<int16_t>(base + 2));
            label(kName[r], static_cast<int16_t>(base - kLabelFont.ascent));
            const int32_t mv = card_.lines[r];
            if (mv < 0 || !live) {
                right(kLabelFont, base, "--", kFaint);
            } else {
                lines[r].put_fixed(mv, 2).put_char('V');
                right(kLabelFont, base, lines[r].view(), kText);
            }
        }
        break;
    }
    case hm::Card::Count: break;
    }

    hm::Cell cell;
    cell.geom = {y0, cell_bottom, y0, y0};
    cell.x0 = hm::kCardX0;
    cell.bg = kBg;
    cell.items = std::span<const hm::Item>(it, static_cast<size_t>(n));
    zone_start[nz] = n;
    uint32_t base = mix_u32(mix_u32(mix_init(), static_cast<uint32_t>(card)), static_cast<uint32_t>(m));
    base = mix_u32(mix_u32(base, dot ? 1u : 0u), ctx_.rotation & 3u);
    uint32_t sig[4] = {};
    for (int z = 0; z < nz; z++)
        sig[z] = sig_items(mix_init(), cell.items.subspan(static_cast<size_t>(zone_start[z]), static_cast<size_t>(zone_start[z + 1] - zone_start[z])));
    PaintBudget *const bg_budget = ctx_.budget;
    uint32_t *old = card_sig_[ci];
    if (!card_valid_[ci] || card_base_[ci] != base) {   // a new card or state: the whole cell
        if (slice_y_[3 + ci] < 0 || slice_sig_[3 + ci] != base) {
            for (int z = 0; z < 4; z++) slice_card_[ci][z] = sig[z];
        }
        if (!paint_cell_sliced(3 + ci, base, cell, hm::kCardX0, y0, hm::kW - 1, cell_bottom)) return;
        card_valid_[ci] = true;
        card_base_[ci] = base;
        bool delta = false;
        for (int z = 0; z < 4; z++) {
            old[z] = slice_card_[ci][z];
            if (z < nz && old[z] != sig[z]) delta = true;
        }
        if (!delta) return;
    }
    for (int z = 0; z < nz; z++) {
        if (sig[z] == old[z]) continue;
        const int cost = static_cast<int>(hm::kW - hm::kCardX0) * (zy[z].b - zy[z].a + 1);
        if (bg_budget && !bg_budget->admit(cost, true)) return;
        old[z] = sig[z];
        hm::paint(cell, hm::kCardX0, zy[z].a, hm::kW - 1, zy[z].b);
    }
}

// ---- info block --------------------------------------------------------------------------------
// One compositor cell (y194..239): the rule above it and the two info rows. Every row carries data: row 1 is the PD
// diagnosis word (live or stale), or CC1/CC2 at rest, or D+/D- for a live session without PD; row 2 is the protocol and
// contract summary. (The USB link speed left Home: its detection is parked.)

void HomePage::paint_info(const Frame &f) {
    const hm::Mode m = f.mode;
    const bool live = m == hm::Mode::Live, stale = m == hm::Mode::Stale;

    // Row 1: the Black Box diagnosis of the PD session while live or stale (hm::format_diag: its worst finding, or the
    // state, in the finding's tone); otherwise, and for a session without PD (a legacy charger: the lines tell more), the
    // measured lines (CC1/CC2 at rest, D+/D- live).
    hm::LinesText lt;
    hm::DiagText dt;
    std::string_view row1_tag = hm::kDiagTag, row1_val;
    const uint16_t row1_tag_col = (m == hm::Mode::Idle || m == hm::Mode::NoMeter) ? kFaint : kDim;
    uint16_t row1_val_col = kDim;
    const int32_t dp = ctx_.analog ? ctx_.analog->dp_mv : -1, dm = ctx_.analog ? ctx_.analog->dm_mv : -1;
    if (hm::format_diag(m, pd::diag::live::report(), dt)) {
        row1_val = dt.value.view();
        row1_val_col = dt.tone == hm::DiagTone::Stale ? kStale : dt.tone == hm::DiagTone::Error ? kError
                     : dt.tone == hm::DiagTone::Caution ? kCaution : kDim;
    } else {
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
    // A finding in red or amber fills the diagnosis row; the hint to flip a crossed cable takes that row the same way.
    uint16_t diag_fill = kBg;
    if (cross != raguard::CrossKind::None) {
        row1_tag = {};
        row1_val = cross == raguard::CrossKind::Crossed ? "CROSSED: flip one" : "Flip a cable?";
        diag_fill = cross == raguard::CrossKind::Crossed ? kError : kCaution;
    } else if (row1_val_col == kError || row1_val_col == kCaution) {
        diag_fill = row1_val_col;
    }
    const uint16_t diag_ink = diag_fill == kError ? kErrInk : kSpotInk;
    const std::string_view role = m == hm::Mode::NoMeter ? "" : fusb302::self_powered() ? "SNK" : "PAS";

    // ---- the items: structure first, then the three zones in kInfoZones order. Fieldbook session block: row 1 is the
    // protocol (its tag a solid chip) and the elapsed time, row 2 the diagnosis word (or the lines) and the boot role.
    hm::Item it[10];
    int n = 0;
    int zone_start[4];
    auto add = [&](const hm::Item &item) {
        if (item.kind == hm::ItemKind::Text && item.text.empty()) return;
        it[n++] = item;
    };
    add(hm::fill(0, hm::kRule3Y, hm::kW, 2, kRule));
    zone_start[0] = n;
    if (!pt.tag.view().empty()) {   // the chip: font 12 centered inside 13 px box; grey with the rest of a stale page
        const TextBounds tb = badge_text_bounds(kLabelFont, pt.tag.view());
        const int16_t cw = static_cast<int16_t>(std::max(tb.w + 8, 20));
        const uint16_t chip = !known ? kBg2 : stale ? kStaleInk : kChip;
        const uint16_t chip_ink = !known ? tcol : stale ? kStaleFill : kOnChip;
        const int16_t cy = static_cast<int16_t>(kRowTop[0] + 1);
        add(hm::fill(hm::kInfoTagX, cy, cw, 13, chip));
        add(hm::badge_text(kLabelFont, hm::kInfoTagX, cy, cw, 13, pt.tag.view(), chip_ink));
    }
    add(hm::text(kSmall, hm::kInfoValX, kRowTop[0], pt.value.view(), vcol));
    zone_start[1] = n;
    {   // the elapsed time, right-aligned in its five-character field
        const int pad = static_cast<int>(eln) < kElChars ? kElChars - static_cast<int>(eln) : 0;
        add(hm::text(kSmall, static_cast<int16_t>(kElX + pad * kSmall.adv), kRowTop[0], {el, eln}, kDim));
    }
    zone_start[2] = n;
    if (diag_fill != kBg) add(hm::fill(0, static_cast<int16_t>(kRowTop[1] - 1), hm::kW, static_cast<int16_t>(240 - kRowTop[1] + 1), diag_fill));
    add(hm::text(kSmall, hm::kInfoTagX, kRowTop[1], row1_tag, diag_fill != kBg ? diag_ink : row1_tag_col));
    add(hm::text(kSmall, row1_tag.empty() ? hm::kInfoTagX : hm::kInfoValX, kRowTop[1], row1_val, diag_fill != kBg ? diag_ink : row1_val_col));
    add(hm::text(kLabelFont, kRoleX + 8, static_cast<int16_t>(kRowTop[1] + 3), role, diag_fill != kBg ? diag_ink : kFaint));
    zone_start[3] = n;

    hm::Cell cell;
    cell.geom = {hm::kRule3Y, 239, 0, 0};
    cell.bg = kBg;
    cell.items = std::span<const hm::Item>(it, static_cast<size_t>(n));

    uint32_t sig[3];
    for (int z = 0; z < 3; z++)
        sig[z] = sig_items(mix_u32(mix_init(), static_cast<uint32_t>(m)),
                           cell.items.subspan(static_cast<size_t>(zone_start[z]), static_cast<size_t>(zone_start[z + 1] - zone_start[z])));
    PaintBudget *const bg_budget = ctx_.budget;
    if (!info_ok_) {   // a new state: the whole cell
        if (slice_y_[5] < 0 || slice_sig_[5] != static_cast<uint32_t>(m)) {
            for (int z = 0; z < 3; z++) slice_info_[z] = sig[z];
        }
        if (!paint_cell_sliced(5, static_cast<uint32_t>(m), cell, 0, hm::kRule3Y, hm::kW - 1, 239)) return;
        info_ok_ = true;
        bool delta = false;
        for (int z = 0; z < 3; z++) {
            info_sig_[z] = slice_info_[z];
            if (info_sig_[z] != sig[z]) delta = true;
        }
        if (!delta) return;
    }
    for (int z = 0; z < 3; z++) {
        if (sig[z] == info_sig_[z]) continue;
        const int cost = static_cast<int>(kInfoZones[z].x1 - kInfoZones[z].x0 + 1) * (kInfoZones[z].y1 - kInfoZones[z].y0 + 1);
        if (bg_budget && !bg_budget->admit(cost, true)) return;
        info_sig_[z] = sig[z];
        hm::paint(cell, kInfoZones[z].x0, kInfoZones[z].y0, kInfoZones[z].x1, kInfoZones[z].y1);
    }
}

// ---- statics ---------------------------------------------------------------------------------

// Everything that depends on the mode and the hero only. Runs after an invalidate, a mode change and a hero change.
// Home is opaque: every pixel belongs to one of the six cells (hero, two ledger rows, two cards, info), each painted
// whole on its first update, so nothing is drawn here.
void HomePage::paint_statics() {
    statics_ok_ = true;
    info_ok_ = false;
    for (bool &v : cell_valid_) v = false;
    for (bool &v : card_valid_) v = false;
    for (int16_t &y : slice_y_) y = -1;
    for (int8_t &c : painted_cursor_) c = -1;
    slice_dirty_ = 0;
    rr_slot_ = 0;
    hd_[0].init(kHero, kHeroDigitX, kHeroDigitTop, kDigitChars);
    hd_[1].init(kBig, kLedDigitX, static_cast<int16_t>(kLedBase[0] - 30), kDigitChars);
    hd_[2].init(kBig, kLedDigitX, static_cast<int16_t>(kLedBase[1] - 30), kDigitChars);
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
    const hm::Qty hero = ctx_.keep.home_hero;
    if (hero != shown_hero_) {   // Long Back: every quantity moves to another slot
        shown_hero_ = hero;
        statics_ok_ = false;
        sweep_ok_ = false;
    }
    sample_values(f, now_ms);
    if (!statics_ok_) paint_statics();
    // The cards: what has real non-zero data right now, then one cycler step (the clock runs while live only).
    const bool live = f.mode == hm::Mode::Live;
    const hm::Drop drop = hm::drop_of(live, ctx_.cable.has_idle(), ctx_.cable.idle_mv(), ctx_.cable.idle_ma(),
                                      abs32(ctx_.pm.vbus_mv), abs32(ctx_.pm.current_ma));
    const bool has_drop = drop.state == hm::DropState::Value && drop.mv > 0;
    const bool has_sag = ctx_.extremes.has_sag() && ctx_.extremes.sag_bp() > 0 && ctx_.extremes.sag_level_mv() > 0;
    const bool has_peak = ctx_.extremes.peak_mw() > 0 || ctx_.extremes.peak_ma() > 0;
    const std::optional<adc::Reading> &an = ctx_.analog;
    const bool has_lines = an && (an->cc1_mv >= 0 || an->cc2_mv >= 0 || an->dp_mv >= 0 || an->dm_mv >= 0);
    hm::CardMask ok = 0;
    if (has_peak) ok |= hm::card_bit(hm::Card::Peak);
    if (live && (has_drop || has_sag)) ok |= hm::card_bit(hm::Card::Drop);
    if (fusb302::emarker().valid) ok |= hm::card_bit(hm::Card::Cable);
    if (has_lines) ok |= hm::card_bit(hm::Card::Lines);
    ctx_.keep.cards.tick(now_ms, live, ok);
    {   // Attention: a card whose signal is moving right now takes the upper cell, with the dot (home_cards.hpp)
        // While a card wants the cell it is forced every tick (it may have gained its data since); the release goes
        // to the cycler once, when the attention ends.
        hm::Attention &att = ctx_.keep.attention;
        const hm::Card was = att.want();
        const hm::Card want = att.update(now_ms, live, ctx_.pm.vbus_mv, an ? an->cc1_mv : -1, an ? an->cc2_mv : -1,
                                         ctx_.extremes.peak_mw());
        if (want != hm::Card::Count || was != hm::Card::Count) ctx_.keep.cards.force_top(want, ok);
    }

    int step_col[3], step_n[3];
    update_sweep(f, now_ms, step_col, step_n);
    const hm::Ledger led = hm::ledger_of(hero);
    const hm::Qty q[3] = {hero, led.row[0], led.row[1]};
    int start = rr_slot_ % 6;
    for (int k = 0; k < 6; ++k) {
        if (slice_y_[k] >= 0) { start = k; break; }
    }
    for (int off = 0; off < 6; ++off) {
        const int s = (start + off) % 6;
        if (s < 3) paint_qty(s, q[s], f, step_col[s], step_n[s]);
        else if (s < 5) paint_card(s - 3, f);
        else paint_info(f);
        if (ctx_.budget && ctx_.budget->more) {
            rr_slot_ = static_cast<uint8_t>(slice_y_[s] >= 0 ? s : (s + 1) % 6);
            return;
        }
    }
}

}  // namespace k2
