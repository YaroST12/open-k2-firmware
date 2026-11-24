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

// ---- quantity cells (slot 0 = hero, 1 / 2 = side-by-side sub-heroes) ---------------------------
constexpr int16_t kHeroDigitX = 72, kHeroDigitTop = 21, kHeroUnitX = 198, kHeroUnitTop = 32;   // font_48 baseline 51
constexpr int16_t kHeroRows = 30;                     // font_48 line height
constexpr int16_t kSubDigitTop = 91, kSubUnitTop = 98;   // font_24 baseline 110, font_16 unit baseline 110
constexpr int16_t kSubRows = 24;                      // font_24 line height
constexpr int kDigitChars = 5;
constexpr int16_t kPillRight = 222, kPillY = 1, kPillBoxH = 17;
constexpr int16_t kHeroArrowX = 14, kHeroArrowY = 26, kHeroArrowBox = 18;
constexpr int16_t kSubArrowDy = 23, kSubArrowBox = 13;
constexpr int16_t kHeroLabelX = 10, kSubLabelDx = 5;
constexpr int16_t kMaxRightHero = 219;
static_assert(kHeroDigitX + kDigitChars * 24 - 1 == 191, "hero digits end at x191");
static_assert(hm::kSubX0[0] + 35 + kDigitChars * 12 - 1 <= hm::kSubX1[0] - 15, "left sub-hero digits clear unit");

// ---- 3 configurable boxes (y128..193, 74x66 each) ----------------------------------------------
constexpr int16_t kBoxPadL = 4, kBoxPadR = 4;
constexpr int16_t kBoxDecDx = 34;                     // shared decimal column inside each 74 px box
// 6-step fading cyan border palette (0x07FF -> 0x0145) over 3 s (500 ms per step).
constexpr uint16_t kGlowColors[hm::BoxCycler::kGlowSteps] = {0x0145, 0x028A, 0x03EF, 0x0534, 0x069A, 0x07FF};

// ---- info block ----------------------------------------------------------------------------------
constexpr int16_t kInfoRow1Top = 201, kInfoRow2Top = 223;
constexpr int kDiagChars = 16, kElChars = 5;
constexpr int16_t kElX = 166, kRoleX = 196;   // kElX leaves room for the PC-link icon after the elapsed time
constexpr int16_t kLinkX = 208;                 // the icon box, kLinkBox wide, at row 1's top
constexpr int16_t kRowTop[2] = {kInfoRow1Top, kInfoRow2Top};
// Row 1's diagnosis word (hm::format_diag) and its tag fit between the tag column and the elapsed time.
static_assert(pd::diag::screen::kLabelChars <= static_cast<std::size_t>(kDiagChars), "the diagnosis word runs into the elapsed time");
static_assert(hm::kInfoValX + kDiagChars * static_cast<int>(kSmallAdvPx) <= kRoleX, "the diagnosis word overlaps the boot role");
static_assert(hm::kInfoValX + 15 * static_cast<int>(kSmallAdvPx) <= kLinkX - 2, "idle wait text overlaps the PC-link icon");
static_assert(hm::kInfoValX + hm::kProtoChars * static_cast<int>(kSmallAdvPx) <= kElX, "the protocol value overlaps the elapsed time");
static_assert(kElX + kElChars * static_cast<int>(kSmallAdvPx) + 2 <= kLinkX && kLinkX + hm::kLinkBox <= hm::kW - 2, "the PC-link icon overlaps the elapsed time or runs off the card");
static_assert(hm::kInfoTagX + hm::kDiagTag.size() * kSmallAdvPx <= static_cast<unsigned>(hm::kInfoValX), "the Diag tag runs into its value");

// Repaint zones of the info cell (logical, inclusive). A zone is wider and lower than its fields on purpose: a glyph
// keeps its native pixel size while the landscape mapping squeezes the rows, so an old text can reach a few pixels
// past the logical box of the new one, and the rect repainted for a change has to cover all of it.
using Zone = HomePage::ZoneRect;
constexpr Zone kInfoZones[3] = {
    {0, 196, 170, 226},     // info row 1: tag and value
    {160, 196, 223, 226},   // info row 1: elapsed time and the PC-link icon
    {0, 220, 223, 239},     // info row 2: protocol, value, role
};
constexpr int kCatchUpSteps = 3;                      // late sweep steps taken in one update
constexpr int kReloadLagSteps = 8;                   // a sweep this many steps behind is rebuilt from the trend

constexpr int32_t abs32(int32_t v) { return v < 0 ? (v == INT32_MIN ? INT32_MAX : -v) : v; }

struct QtyMeta {
    trend::Qty tq;
    const char *label, *unit;
    char uc;
    uint16_t color, tint, line;
};
constexpr QtyMeta kQtyMeta[3] = {
    {trend::Qty::W, "W", "W", 'W', kWatt, kWattT, kWattL},   // Qty::W = 0
    {trend::Qty::V, "V", "V", 'V', kVolt, kVoltT, kVoltL},   // Qty::V = 1
    {trend::Qty::A, "A", "A", 'A', kAmp,  kAmpT,  kAmpL},    // Qty::A = 2
};
static_assert(static_cast<unsigned>(hm::Qty::W) == 0 && static_cast<unsigned>(hm::Qty::V) == 1 &&
              static_cast<unsigned>(hm::Qty::A) == 2);
inline const QtyMeta &qty_meta(hm::Qty q) { return kQtyMeta[static_cast<unsigned>(q)]; }
constexpr trend::Qty trend_qty(hm::Qty q) { return qty_meta(q).tq; }
constexpr std::string_view qty_label(hm::Qty q) { return qty_meta(q).label; }
constexpr std::string_view qty_unit(hm::Qty q) { return qty_meta(q).unit; }
constexpr char qty_unit_char(hm::Qty q) { return qty_meta(q).uc; }
inline uint16_t qty_color(hm::Qty q) { return qty_meta(q).color; }
inline uint16_t qty_tint(hm::Qty q) { return qty_meta(q).tint; }
inline uint16_t qty_line(hm::Qty q) { return qty_meta(q).line; }
// A field's look in one mode: the ground, the digit ink, the label ink. An alarm over the limit is white on red.
struct FieldLook { uint16_t fill, ink, label; };
FieldLook field_look(hm::Mode m, hm::Qty q, bool over, bool near_) {
    if (over) return {kError, kAlarmInk, kAlarmInk};
    const uint16_t ink = near_ ? kCaution : m == hm::Mode::Live ? qty_color(q) : m == hm::Mode::Stale ? kStale : kFaint;
    return {kBg, ink, kDim};
}

// The geometry of a quantity slot: the content rows/columns (rules owned by the cell included in cell_x1 / cell_bottom).
struct Slot { int16_t x0, x1, cell_x1, top, bottom, cell_bottom, sweep_x0; };
constexpr Slot slot_of(int s) {
    return s == 0 ? Slot{0, hm::kW - 1, hm::kW - 1, hm::kHeroY0, hm::kHeroY1, static_cast<int16_t>(hm::kRule1Y + 1), hm::kColX0}
         : s == 1 ? Slot{hm::kSubX0[0], hm::kSubX1[0], hm::kSubVRuleX, hm::kSubY0, hm::kSubY1, static_cast<int16_t>(hm::kRule2Y + 1), hm::kSubColX0[0]}
                  : Slot{hm::kSubX0[1], hm::kSubX1[1], hm::kSubX1[1], hm::kSubY0, hm::kSubY1, static_cast<int16_t>(hm::kRule2Y + 1), hm::kSubColX0[1]};
}
constexpr int16_t sub_digit_x(int col) { return static_cast<int16_t>(hm::kSubX1[col] - 15 - kDigitChars * 12); }
constexpr int16_t sub_unit_x(int col) { return static_cast<int16_t>(hm::kSubX1[col] - 11); }

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
    for (bool &v : box_valid_) v = false;
    for (int16_t &y : slice_y_) y = -1;
    for (int8_t &c : painted_cursor_) c = -1;
    slice_dirty_ = 0;
    rr_slot_ = 0;
    thr_.reset();   // the digits repaint at once; mode_ and the sweeps stay (a capture re-renders strips of one frame)
    for (hm::DigitField &d : hd_) d.invalidate();
}

// Entered (the ring view owns the keys otherwise): +/- walk the hero, OK zooms the trend span, long OK resets the energy.
Strip HomePage::labels() const {
    return {{sl::Back, sl::HeroPrev, sl::ZoomReset, sl::HeroNext}};
}

bool HomePage::on_event(Event e) {
    if (e.press == Press::Long && e.button == Button::Ok) {
        ctx_.ui->run_action(Action::EnergyReset, ctx_.now);   // energy, peaks, sag and the card cycler
        return true;
    }
    if (e.press != Press::Short) return false;
    if (e.button == Button::Ok) {   // the trend span 10s / 60s / 10m
        window_ = trend::next(window_);
        sweep_ok_ = false;   // the next update reloads the sweeps for the new window and repaints the cells
        return true;
    }
    if (e.button == Button::Plus || e.button == Button::Minus) {
        const home::Qty q = ctx_.keep.home_hero;
        ctx_.ui->set_home_hero(e.button == Button::Plus ? hm::next(q) : hm::prev(q));
        return true;
    }
    return false;
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
    // The box & peak values, as one set with the digits.
    if (fresh) {
        const hm::Extremes &x = ctx_.extremes;
        card_.peak_mw = x.peak_mw();
        card_.peak_mv = x.peak_mv();
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

void HomePage::update_sweep(const Frame &f, uint32_t now_ms) {
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
        sw_[0].step(lv);
        hero_n++;
        const int target = sw_[0].cursor * hm::kLedgerCols / hm::kCols;
        for (int i = 1; i < 3; i++) {
            if (sw_[i].cursor == target) continue;
            const uint8_t l = static_cast<uint8_t>(hm::sweep_rows(ctx_.trend.latest(window_, trend_qty(q[i])), scale[i], span[i]));
            sw_[i].step(l);
        }
    }
}

// ---- quantity cells ----------------------------------------------------------------------------

struct HomePage::QtyHead {
    StrBuf<12> ptxt;
    StrBuf<20> mtxt;
    StrBuf<20> atxt;
};

namespace {
// Format a compact peak/max value without trailing zeros ("20", "20.05", "3.4", "68.5", "100").
[[gnu::noinline]] void put_compact_val(StrBuf<20> &out, uint32_t milli, char unit) {
    char tmp[12];
    const int n = hm::fmt_level(tmp, milli);
    out.put({tmp, static_cast<size_t>(n)}).put_char(unit);
}
}  // namespace

// At most 4 head items: the warning triangle and the alarm line (or the letter W/V/A), then the pill's plate and
// text (or the unified Max/Peak / Peak readout).
uint32_t HomePage::qty_head(int si, hm::Qty q, const Frame &f, QtyHead &h, hm::Item *it, int &n) {
    const bool hero = si == 0;
    const Slot s = slot_of(si);
    const hm::Mode m = f.mode;
    const bool live = m == hm::Mode::Live, shown = live || m == hm::Mode::Stale;
    const int qi = static_cast<int>(q);
    const hm::Alarm al = f.alarm[qi];
    const bool over = al == hm::Alarm::Over, near_ = al == hm::Alarm::Near;
    const uint32_t mx = f.max[qi];
    const uint32_t pk = q == hm::Qty::W ? card_.peak_mw : q == hm::Qty::V ? card_.peak_mv : card_.peak_ma;
    const FieldLook look = field_look(m, q, over, near_);
    const uint16_t label_col = look.label;
    const char uc = qty_unit_char(q);
    // A W maximum shows in whole watts ("100W", "OVER 100W LIMIT"); the alarm itself compares the exact figure.
    const uint32_t shown_max = q == hm::Qty::W ? (mx + 500) / 1000 * 1000 : mx;
    // W peak rounds to 0.1 W above 10 W (whole W >= 100 W); V/A keep up to 2 decimals via fmt_level.
    const uint32_t shown_pk = q == hm::Qty::W ? (pk >= 100000 ? (pk + 500) / 1000 * 1000 : (pk + 50) / 100 * 100) : pk;
    StrBuf<12> &ptxt = h.ptxt;
    StrBuf<20> &mtxt = h.mtxt;
    StrBuf<20> &atxt = h.atxt;
    const int16_t lx = hero ? kHeroLabelX : static_cast<int16_t>(s.x0 + kSubLabelDx);
    const int16_t ly = static_cast<int16_t>(s.top + 3);
    uint32_t head = mix_init();
    const int16_t right_x = hero ? kMaxRightHero : static_cast<int16_t>(s.x1 - 4);
    if (over) {   // the whole field is the alarm: triangle and "OVER 5A LIMIT" in white where the label was
        it[n++] = hm::warn(static_cast<int16_t>(lx - 1), static_cast<int16_t>(s.top + 2), kAlarmInk, kError);
        hm::alarm_text(al, static_cast<uint32_t>(q == hm::Qty::W ? w_mw_ : a_ma_), shown_max, atxt, uc, !hero);
        it[n++] = hm::text(kLabelFont, static_cast<int16_t>(lx + 13), ly, atxt.view(), kAlarmInk);
    } else {
        const std::string_view lab = qty_label(q);
        it[n++] = hm::text(kLabelFont, lx, ly, lab, shown ? look.ink : label_col);
        if (near_) {   // "93% OF 100W" (hero) or "92%" (sub-hero) as an orange chip at the right end of the label line
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
        pill = near_ ? hm::Pill::None : hm::pill_of(m, hm::Alarm::None, f.pin);
        switch (pill) {
        case hm::Pill::Last:   ptxt.put("LAST ").put_age(ctx_.live.age_s(f.now)); break;
        case hm::Pill::NoVbus: ptxt.put("NO VBUS"); break;
        case hm::Pill::Pin:    ptxt.put("Rd pin CC").put_char(static_cast<char>('0' + f.pin)); break;
        case hm::Pill::Over: case hm::Pill::None: break;
        }
        if (pill != hm::Pill::None) {
            const uint16_t fillc = pill == hm::Pill::NoVbus ? kSurface : pill == hm::Pill::Pin ? kCaution : kStale;
            const uint16_t inkc = pill == hm::Pill::NoVbus ? kDim : kBg;
            const TextBounds tb = badge_text_bounds(kSmall, ptxt.view());
            const int16_t bw = static_cast<int16_t>(tb.w + 10);
            const int16_t x0 = static_cast<int16_t>(kPillRight - bw);
            it[n++] = hm::fill(x0, kPillY, bw, kPillBoxH, fillc);
            it[n++] = hm::badge_text(kSmall, x0, kPillY, bw, kPillBoxH, ptxt.view(), inkc);
        }
    }
    if (pill == hm::Pill::None && al == hm::Alarm::None && shown) {
        const bool has_max = live && shown_max > 0;
        const bool has_pk = shown_pk > 0;
        if (hero) {
            if (has_max && has_pk) {
                mtxt.put("Max/Peak ");
                put_compact_val(mtxt, shown_max, uc);
                mtxt.put_char('/');
                put_compact_val(mtxt, shown_pk, uc);
            } else if (has_pk) {
                mtxt.put("Peak ");
                put_compact_val(mtxt, shown_pk, uc);
            } else if (has_max) {
                mtxt.put("Max ");
                put_compact_val(mtxt, shown_max, uc);
            }
        } else {
            if (has_max && has_pk) {
                put_compact_val(mtxt, shown_max, uc);
                mtxt.put_char('/');
                put_compact_val(mtxt, shown_pk, uc);
            } else if (has_pk) {
                put_compact_val(mtxt, shown_pk, uc);
            } else if (has_max) {
                put_compact_val(mtxt, shown_max, uc);
            }
        }
        if (!mtxt.view().empty()) {
            const int16_t mn = static_cast<int16_t>(mtxt.view().size());
            it[n++] = hm::text(kLabelFont, static_cast<int16_t>(right_x + 1 - 6 * mn), ly, mtxt.view(),
                               m == hm::Mode::Stale ? kStale : label_col);
        }
    }
    head = mix_u32(mix_u32(head, static_cast<uint32_t>(al)), static_cast<uint32_t>(pill));
    return pg::mix_str(pg::mix_str(pg::mix_str(head, ptxt.view()), mtxt.view()), atxt.view());
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

struct HomePage::QtyCommit {
    uint32_t base, head, arrow_sig, bar_sig;
    int16_t x0, x1, cell_x1, top, cell_bottom, sweep_x0;
    int16_t t0, t1;
    int16_t ax, ay, abox;
    hm::BarGeom bg;
    int16_t dtop, drows, px0, px1;
    bool digits_changed, sweep;
    hm::DigitField prev_hd;
};

[[gnu::noinline]] bool HomePage::commit_zoned_cell(int slot_id, uint32_t base, uint32_t zone_seed,
                                                   const hm::Cell &cell, const int *zone_start,
                                                   const ZoneRect *zones, int nz,
                                                   bool &valid, uint32_t *old_sig, uint32_t *slice_zones,
                                                   int16_t cx0, int16_t cy0, int16_t cx1, int16_t cy1,
                                                   uint32_t *saved_base) {
    uint32_t sig[4] = {};
    for (int z = 0; z < nz; z++)
        sig[z] = sig_items(zone_seed, cell.items.subspan(static_cast<size_t>(zone_start[z]),
                                                          static_cast<size_t>(zone_start[z + 1] - zone_start[z])));
    PaintBudget *const bg_budget = ctx_.budget;
    if (!valid || (saved_base && *saved_base != base)) {
        if (slice_y_[slot_id] < 0 || slice_sig_[slot_id] != base) {
            for (int z = 0; z < nz; z++) slice_zones[z] = sig[z];
        }
        if (!paint_cell_sliced(slot_id, base, cell, cx0, cy0, cx1, cy1)) return false;
        valid = true;
        if (saved_base) *saved_base = base;
        bool delta = false;
        for (int z = 0; z < nz; z++) {
            old_sig[z] = slice_zones[z];
            if (old_sig[z] != sig[z]) delta = true;
        }
        if (!delta) return true;
    }
    for (int z = 0; z < nz; z++) {
        if (sig[z] == old_sig[z]) continue;
        const int cost = static_cast<int>(zones[z].x1 - zones[z].x0 + 1) * (zones[z].y1 - zones[z].y0 + 1);
        if (bg_budget && !bg_budget->admit(cost, true)) return false;
        old_sig[z] = sig[z];
        hm::paint(cell, zones[z].x0, zones[z].y0, zones[z].x1, zones[z].y1);
    }
    return true;
}

[[gnu::noinline]] void HomePage::commit_qty_cell(int si, const QtyCommit &qc, const hm::Cell &cell) {
    PaintBudget *const bg_budget = ctx_.budget;
    uint32_t *sig = cell_sig_[si];
    if (!cell_valid_[si] || sig[0] != qc.base) {   // a new state: the whole cell
        if (slice_y_[si] < 0 || slice_sig_[si] != qc.base) {
            painted_cursor_[si] = qc.sweep ? static_cast<int8_t>(sw_[si].cursor) : -1;
            slice_dirty_ &= static_cast<uint8_t>(~(1u << si));
            slice_parts_[si][0] = qc.head;
            slice_parts_[si][1] = qc.arrow_sig;
            slice_parts_[si][2] = qc.bar_sig;
        } else if (qc.digits_changed) {
            slice_dirty_ |= static_cast<uint8_t>(1u << si);
        }
        if (!paint_cell_sliced(si, qc.base, cell, qc.x0, qc.top, qc.cell_x1, qc.cell_bottom)) return;
        sig[0] = qc.base;
        sig[1] = slice_parts_[si][0];
        sig[2] = slice_parts_[si][1];
        sig[3] = slice_parts_[si][2];
        cell_valid_[si] = true;
        if (slice_dirty_ & (1u << si)) {
            slice_dirty_ &= static_cast<uint8_t>(~(1u << si));
            hd_[si].invalidate();
        }
        if (!qc.digits_changed && sig[1] == qc.head && sig[2] == qc.arrow_sig && sig[3] == qc.bar_sig &&
            (!qc.sweep || painted_cursor_[si] == static_cast<int8_t>(sw_[si].cursor)))
            return;
    }
    auto abort_delta = [&]() {
        if (qc.digits_changed) hd_[si] = qc.prev_hd;
    };
    if (sig[1] != qc.head) {
        const int16_t hx0 = si == 0 ? 4 : qc.x0;
        const int cost = static_cast<int>(qc.x1 - hx0 + 1) * 19;
        if (bg_budget && !bg_budget->admit(cost, true)) { abort_delta(); return; }
        sig[1] = qc.head;
        hm::paint(cell, hx0, qc.top, qc.x1, static_cast<int16_t>(qc.top + 18));
    }
    if (sig[2] != qc.arrow_sig) {
        const int cost = qc.abox * qc.abox;
        if (bg_budget && !bg_budget->admit(cost, true)) { abort_delta(); return; }
        sig[2] = qc.arrow_sig;
        hm::paint(cell, qc.ax, qc.ay, static_cast<int16_t>(qc.ax + qc.abox - 1), static_cast<int16_t>(qc.ay + qc.abox - 1));
    }
    if (sig[3] != qc.bar_sig) {
        const int cost = static_cast<int>(qc.bg.x1 - qc.bg.x0 + 1) * (hm::kBarH + 4);
        if (bg_budget && !bg_budget->admit(cost, true)) { abort_delta(); return; }
        sig[3] = qc.bar_sig;
        hm::paint(cell, qc.bg.x0, static_cast<int16_t>(qc.bg.y - 2), qc.bg.x1, static_cast<int16_t>(qc.bg.y + hm::kBarH + 1));
    }
    if (qc.sweep) {
        const int cur = static_cast<int>(sw_[si].cursor);
        if (painted_cursor_[si] < 0) painted_cursor_[si] = static_cast<int8_t>(cur);
        else if (painted_cursor_[si] != cur) {
            const int first = painted_cursor_[si];
            const int steps = (cur - first + sw_[si].n) % sw_[si].n;
            const hm::Strip st = hm::strip_of(first, steps, sw_[si].n, qc.sweep_x0);
            int cost = 0;
            for (int i = 0; i < st.n; i++)
                cost += static_cast<int>(st.r[i].x1 - st.r[i].x0 + 1) * (qc.t1 - qc.t0 + 1);
            if (bg_budget && !bg_budget->admit(cost, true)) { abort_delta(); return; }
            for (int i = 0; i < st.n; i++)
                hm::paint(cell, st.r[i].x0, qc.t0, st.r[i].x1, qc.t1);
            painted_cursor_[si] = static_cast<int8_t>(cur);
        }
    }
    if (qc.digits_changed) {
        const int cost = static_cast<int>(qc.px1 - qc.px0 + 1) * qc.drows;
        if (bg_budget && !bg_budget->admit(cost, true)) { abort_delta(); return; }
        hm::paint(cell, qc.px0, qc.dtop, qc.px1, static_cast<int16_t>(qc.dtop + qc.drows - 1));
    }
}

void HomePage::paint_qty(int si, hm::Qty q, const Frame &f) {
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

    QtyCommit qc{};
    qc.x0 = s.x0;
    qc.x1 = s.x1;
    qc.cell_x1 = s.cell_x1;
    qc.top = s.top;
    qc.cell_bottom = s.cell_bottom;
    qc.sweep_x0 = s.sweep_x0;
    qc.t0 = g0.t0;
    qc.t1 = g0.t1;
    qc.dtop = hero ? kHeroDigitTop : kSubDigitTop;
    qc.drows = hero ? kHeroRows : kSubRows;
    qc.bg = hero ? hm::kHeroBar : hm::sub_bar(si - 1);
    qc.ax = hero ? kHeroArrowX : static_cast<int16_t>(s.x0 + 8);
    qc.ay = hero ? kHeroArrowY : static_cast<int16_t>(s.top + kSubArrowDy);
    qc.abox = hero ? kHeroArrowBox : kSubArrowBox;
    qc.prev_hd = hd_[si];
    qc.digits_changed = hd_[si].set(f.text[qi], qc.px0, qc.px1);   // before the items: the digit item views hd_'s text

    // At most 12 items: 2 rules, status edge (hero), 4 head items, unit, 3 bar items, arrow, digits.
    QtyHead texts;
    hm::Item it[12];
    int n = 0;
    if (hero) {
        it[n++] = hm::fill(0, hm::kRule1Y, hm::kW, 2, kRule);
        it[n++] = hm::fill(0, s.top, 4, static_cast<int16_t>(s.bottom - s.top + 1), over ? kAlarmInk : ink);
    } else {
        it[n++] = hm::fill(s.x0, hm::kRule2Y, static_cast<int16_t>(s.cell_x1 - s.x0 + 1), 2, kRule);
        if (si == 1) it[n++] = hm::fill(hm::kSubVRuleX, s.top, 1, static_cast<int16_t>(s.bottom - s.top + 1), kRule);
    }
    qc.head = qty_head(si, q, f, texts, it, n);

    // Unit, bar, arrow, digits.
    if (hero) it[n++] = hm::text(kMid, kHeroUnitX, kHeroUnitTop, qty_unit(q), ink);
    else it[n++] = hm::text(kSmall, sub_unit_x(si - 1), kSubUnitTop, qty_unit(q), ink);
    if (bar && shown) {   // contract bar: track, fill, 90 % tick
        const uint32_t abs = static_cast<uint32_t>(q == hm::Qty::W ? w_mw_ : a_ma_);
        const int fw = hm::bar_fill_w(abs, mx, qc.bg);
        const uint16_t fc = ink;
        const uint16_t tc = over ? kAlarmInk : m == hm::Mode::Stale ? kDim : kText;
        const uint16_t track = over ? kErrD : kRule;
        it[n++] = hm::fill(qc.bg.x0, qc.bg.y, static_cast<int16_t>(hm::bar_track(qc.bg)), hm::kBarH, track);
        if (fw > 0) it[n++] = hm::fill(qc.bg.x0, qc.bg.y, static_cast<int16_t>(fw), hm::kBarH, fc);
        it[n++] = hm::fill(hm::bar_tick_x(qc.bg), static_cast<int16_t>(qc.bg.y - 2), 1, hm::kBarH + 4, tc);
        qc.bar_sig = mix_u32(mix_u32(mix_u32(1, static_cast<uint32_t>(fw)), fc), tc);
    }
    if (q == hm::Qty::A && shown) {
        const bool dash = flow_ == power::Flow::Idle;
        const uint8_t aux = dash ? hm::dash_dir(ctx_.rotation) : hm::arrow_dir(flow_ == power::Flow::Forward, ctx_.rotation);
        const uint16_t ac = over ? kAlarmInk : dash ? kFaint : live ? kFlow : kStale;
        it[n++] = hm::arrow(qc.ax, qc.ay, aux, ac, qc.abox);
        qc.arrow_sig = mix_u32(mix_u32(1, aux), ac);
    }
    it[n++] = hm::text(hero ? kHero : kMid, hero ? kHeroDigitX : sub_digit_x(si - 1), qc.dtop, hd_[si].view(), ink);

    hm::Cell cell;
    cell.geom = g0;
    cell.geom.bottom = s.cell_bottom;
    cell.x0 = s.x0;
    cell.x1 = s.cell_x1;
    cell.sweep_x0 = s.sweep_x0;
    cell.bg = look.fill;
    cell.sweep = shown && !over && sweep_ok_;
    qc.sweep = cell.sweep;
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
    qc.base = mix_u32(mix_u32(base, ctx_.rotation & 3u), static_cast<uint32_t>(q));
    commit_qty_cell(si, qc, cell);
}

// ---- 3 configurable 74x66 boxes (y128..193) --------------------------------------------------
// One compositor cell per box; boxes 0 and 1 own their right 1 px vertical divider (x74, x149).
// A metric swap or glow step repaints the whole box; a value change repaints only its zone.

void HomePage::paint_box(int bi, const Frame &f) {
    const hm::Mode m = f.mode;
    const bool live = m == hm::Mode::Live, shown = live || m == hm::Mode::Stale;
    const hm::BoxCycler &cy = ctx_.keep.boxes;
    const hm::BoxKind box = cy.shown(bi);
    const uint8_t glow = cy.glow_level(bi, f.now);
    const int16_t x0 = hm::kBoxX0[bi], x1 = hm::kBoxX1[bi];
    const int16_t cell_x1 = bi < 2 ? hm::kBoxVRuleX[bi] : x1;
    const int16_t y0 = hm::kBoxY0, y1 = hm::kBoxY1;
    const int16_t rx = static_cast<int16_t>(x1 - kBoxPadR);
    const int16_t dec_x = static_cast<int16_t>(x0 + kBoxDecDx);

    hm::Item it[16];
    int n = 0;
    const Zone zy[3] = {
        {x0, y0, x1, static_cast<int16_t>(y0 + 18)},
        {x0, static_cast<int16_t>(y0 + 19), x1, static_cast<int16_t>(y0 + 41)},
        {x0, static_cast<int16_t>(y0 + 42), x1, y1},
    };
    int zone_start[4] = {};
    auto push_text = [&](const Font &font, int16_t x, int16_t y, std::string_view s, uint16_t col) {
        if (!s.empty()) it[n++] = hm::text(font, x, y, s, col);
    };
    auto right = [&](const Font &font, int16_t dy, std::string_view s, uint16_t col) {
        push_text(font, static_cast<int16_t>(rx + 1 - static_cast<int>(s.size()) * font.adv), static_cast<int16_t>(y0 + dy), s, col);
    };
    auto fit_right = [&](int r, std::string_view s, uint16_t col) {
        const bool sm = s.size() <= 8;
        right(sm ? kSmall : kLabelFont, static_cast<int16_t>((r == 0 ? 21 : 43) + (sm ? 0 : 3)), s, col);
    };
    auto dec_row = [&](int r, std::string_view num, std::string_view unit, uint16_t col) {
        const hm::DecSplit sp = hm::split_decimal(num);
        const int16_t top = static_cast<int16_t>(y0 + (r == 0 ? 21 : 43));
        push_text(kSmall, static_cast<int16_t>(dec_x - static_cast<int>(sp.int_part.size()) * kSmall.adv), top, sp.int_part, col);
        push_text(kSmall, dec_x, top, sp.rest, col);
        const int16_t ux = static_cast<int16_t>(rx + 1 - static_cast<int>(unit.size()) * kLabelFont.adv);
        push_text(kLabelFont, ux, static_cast<int16_t>(top + kSmall.ascent - kLabelFont.ascent), unit, col == kInk ? kDim : col);
    };

    if (bi < 2) it[n++] = hm::fill(hm::kBoxVRuleX[bi], y0, 1, static_cast<int16_t>(y1 - y0 + 1), kRule);

    static constexpr const char *kBoxTitles[static_cast<unsigned>(hm::BoxKind::Count)] = {
        "", "PD CHG/SNK", "ENERGY", "DROP", "E-MARKER", "CC1/2", "D+/D-", "BATT", "LOAD R", ""
    };
    zone_start[0] = n;
    const unsigned bi_kind = static_cast<unsigned>(box);
    if (bi_kind < static_cast<unsigned>(hm::BoxKind::Count) && kBoxTitles[bi_kind][0]) {
        push_text(kLabelFont, static_cast<int16_t>(x0 + kBoxPadL), static_cast<int16_t>(y0 + 4), kBoxTitles[bi_kind], glow > 1 ? kAccent : kDim);
    }

    StrBuf<20> sb[2];
    zone_start[1] = n;
    const uint16_t ink = tone(m, kInk);
    // Row 1 items
    switch (box) {
    case hm::BoxKind::Pd: {
        const hm::ProtoInfo &p = f.proto;
        const bool is_pd = p.kind == hm::ProtoKind::PdFixed || p.kind == hm::ProtoKind::PdEpr ||
                           p.kind == hm::ProtoKind::Pps || p.kind == hm::ProtoKind::Avs;
        if (shown && is_pd) {
            if (p.kind == hm::ProtoKind::Pps || p.kind == hm::ProtoKind::Avs) {
                if (p.lo_mv && p.hi_mv && p.lo_mv != p.hi_mv) {
                    put_compact_val(sb[0], p.lo_mv, '-');
                    put_compact_val(sb[0], p.hi_mv, 'V');
                } else {
                    put_compact_val(sb[0], p.hi_mv ? p.hi_mv : p.mv, 'V');
                }
                if (p.kind == hm::ProtoKind::Avs && p.mw) sb[0].put_char(' ').put_u32((p.mw + 500) / 1000).put_char('W');
                else if (p.ma) { sb[0].put_char(' '); put_compact_val(sb[0], p.ma, 'A'); }
            } else {
                put_compact_val(sb[0], p.max_mv ? p.max_mv : p.mv, 'V');
                if (p.ma) { sb[0].put_char(' '); put_compact_val(sb[0], p.ma, 'A'); }
            }
            fit_right(0, sb[0].view(), ink);
        } else if (shown && p.kind != hm::ProtoKind::None) {
            hm::ProtoText pt;
            hm::format_proto(p, pt);
            sb[0].put(pt.tag.view());
            if (!pt.value.view().empty() && sb[0].view().size() + 1 + pt.value.view().size() <= 11)
                sb[0].put_char(' ').put(pt.value.view());
            fit_right(0, sb[0].view(), kDim);
        } else {
            right(kSmall, 21, "--", kFaint);
        }
        break;
    }
    case hm::BoxKind::Energy:
        if (m == hm::Mode::NoMeter) { right(kSmall, 21, "--", kFaint); }
        else {
            const int32_t mwh = ctx_.meter.mwh();
            if (mwh >= 100000) { char t[12]; sb[0].put({t, static_cast<size_t>(hm::fmt_energy(t, mwh))}); }
            else sb[0].put_fixed(mwh, 2);
            dec_row(0, sb[0].view(), "Wh", ink);
        }
        break;
    case hm::BoxKind::Drop:
        if (card_.drop.state == hm::DropState::Value && card_.drop.mv > 0) {
            sb[0].put_fixed(static_cast<int32_t>(card_.drop.mv), 2);
            dec_row(0, sb[0].view(), "V", ink);
        } else {
            right(kSmall, 21, "--", kFaint);
        }
        break;
    case hm::BoxKind::Cable: {
        const pd::Emarker &em = fusb302::emarker();
        if (em.valid) {
            StrBuf<8> s0, s1;
            emarker_amps_text(em, s0);
            emarker_volts_text(em, s1);
            sb[0].put(s0.view()).put_char(' ').put(s1.view());
            right(kSmall, 21, sb[0].view(), ink);
        } else {
            right(kSmall, 21, "--", kFaint);
        }
        break;
    }
    case hm::BoxKind::Cc:
    case hm::BoxKind::Dpdm: {
        const int32_t mv = card_.lines[box == hm::BoxKind::Cc ? 0 : 2];
        if (mv < 0 || m == hm::Mode::Stale || m == hm::Mode::NoMeter) right(kSmall, 21, "--", kFaint);
        else { sb[0].put_fixed(mv, 2); dec_row(0, sb[0].view(), "V", live ? kInk : kDim); }
        break;
    }
    case hm::BoxKind::Battery: {
        const pd::ExtInfo &ext = fusb302::ext();
        const pd::IqStatus &iq = fusb302::interrogate_status();
        const pd::BatteryCaps &bat = ext.battery.valid ? ext.battery : iq.battery;
        const pd::BatteryStatus &bst = ext.battery_status.valid ? ext.battery_status : iq.battery_status;
        const int soc = pd::battery_soc_pct(bat, bst);
        if (soc >= 0) { sb[0].put_u32(static_cast<uint32_t>(soc)).put_char('%'); right(kSmall, 21, sb[0].view(), bst.charging == 0 ? kOk : ink); }
        else right(kSmall, 21, "--", kFaint);
        break;
    }
    case hm::BoxKind::LoadR:
        if (live && a_ma_ >= 50 && v_mv_ > 0) {
            const uint32_t mohm = math::mul_div_u32(static_cast<uint32_t>(v_mv_), 1000u, static_cast<uint32_t>(a_ma_));
            if (mohm < 1000000u) { sb[0].put_fixed(static_cast<int32_t>(mohm), 2); dec_row(0, sb[0].view(), "R", kInk); }
            else right(kSmall, 21, ">1kR", kDim);
        } else {
            right(kSmall, 21, "--", kFaint);
        }
        break;
    default: break;
    }

    zone_start[2] = n;
    // Row 2 items
    switch (box) {
    case hm::BoxKind::Pd: {
        const hm::ProtoInfo &p = f.proto;
        const bool is_pd = p.kind == hm::ProtoKind::PdFixed || p.kind == hm::ProtoKind::PdEpr ||
                           p.kind == hm::ProtoKind::Pps || p.kind == hm::ProtoKind::Avs;
        if (shown && is_pd) {
            const pd::Contract &ctr = fusb302::contract();
            const uint32_t req_mv = ctr.valid && ctr.mv ? ctr.mv : p.mv;
            const uint32_t req_ma = ctr.valid && ctr.op_ma ? ctr.op_ma : p.ma;
            if (p.kind == hm::ProtoKind::Pps && req_mv) sb[1].put_fixed(static_cast<int32_t>(req_mv), 2).put_char('V');
            else if (req_mv) put_compact_val(sb[1], req_mv, 'V');
            if (req_ma) { if (!sb[1].view().empty()) sb[1].put_char(' '); put_compact_val(sb[1], req_ma, 'A'); }
            fit_right(1, sb[1].view(), live ? kAccent : kDim);
        } else if (shown && p.kind != hm::ProtoKind::None) {
            right(kLabelFont, 46, "no PD", kFaint);
        }
        break;
    }
    case hm::BoxKind::Energy:
        if (m == hm::Mode::NoMeter) { right(kSmall, 43, "--", kFaint); }
        else {
            const int32_t mah = ctx_.meter.mah();
            if (mah >= 100000) { char t[12]; sb[1].put({t, static_cast<size_t>(hm::fmt_energy(t, mah))}); }
            else sb[1].put_fixed(mah, 2);
            dec_row(1, sb[1].view(), "Ah", ink);
        }
        break;
    case hm::BoxKind::Drop:
        if (card_.has_sag && card_.sag_bp > 0 && card_.sag_level_mv > 0) {
            sb[1].put_fixed(static_cast<int32_t>(card_.sag_bp * 10u), 2);
            dec_row(1, sb[1].view(), "%", kDim);
        } else {
            right(kSmall, 43, "--", kFaint);
        }
        break;
    case hm::BoxKind::Cable: {
        const pd::Emarker &em = fusb302::emarker();
        if (em.valid) { StrBuf<12> sp; emarker_speed_pill(em, sp); sb[1].put(sp.view()); right(kLabelFont, 46, sb[1].view(), kDim); }
        break;
    }
    case hm::BoxKind::Cc:
    case hm::BoxKind::Dpdm: {
        const int32_t mv = card_.lines[box == hm::BoxKind::Cc ? 1 : 3];
        if (mv < 0 || m == hm::Mode::Stale || m == hm::Mode::NoMeter) right(kSmall, 43, "--", kFaint);
        else { sb[1].put_fixed(mv, 2); dec_row(1, sb[1].view(), "V", live ? kInk : kDim); }
        break;
    }
    case hm::BoxKind::Battery: {
        const pd::ExtInfo &ext = fusb302::ext();
        const pd::IqStatus &iq = fusb302::interrogate_status();
        const pd::BatteryStatus &bst = ext.battery_status.valid ? ext.battery_status : iq.battery_status;
        if (bst.capacity_known()) sb[1].put_u32(bst.present_dwh / 10u).put_char('.').put_u32(bst.present_dwh % 10u).put("Wh");
        else if (bst.valid && bst.present) sb[1].put(bst.charging == 0 ? "chg" : bst.charging == 1 ? "dis" : bst.charging == 2 ? "idle" : "");
        right(kLabelFont, 46, sb[1].view(), kDim);
        break;
    }
    default: break;
    }

    // 2 px fading cyan attention border (6 steps over 3 s)
    if (glow > 0) {
        const uint16_t gc = kGlowColors[glow - 1];
        const int16_t bw = static_cast<int16_t>(x1 - x0 + 1);
        const int16_t bh = static_cast<int16_t>(y1 - y0 + 1);
        it[n++] = hm::fill(x0, y0, bw, 2, gc);
        it[n++] = hm::fill(x0, static_cast<int16_t>(y1 - 1), bw, 2, gc);
        it[n++] = hm::fill(x0, y0, 2, bh, gc);
        it[n++] = hm::fill(static_cast<int16_t>(x1 - 1), y0, 2, bh, gc);
    }

    hm::Cell cell;
    cell.geom = {y0, y1, y0, y0};
    cell.x0 = x0;
    cell.x1 = cell_x1;
    cell.bg = kBg;
    cell.items = std::span<const hm::Item>(it, static_cast<size_t>(n));
    zone_start[3] = n;
    uint32_t base = mix_u32(mix_u32(mix_init(), static_cast<uint32_t>(box)), static_cast<uint32_t>(m));
    base = mix_u32(mix_u32(base, glow), ctx_.rotation & 3u);
    commit_zoned_cell(3 + bi, base, mix_init(), cell, zone_start, zy, 3,
                      box_valid_[bi], box_sig_[bi], slice_box_[bi],
                      x0, y0, cell_x1, y1, &box_base_[bi]);
}

// ---- info block --------------------------------------------------------------------------------
// One compositor cell (y194..239): the rule above it and the two info rows. Every row carries data: row 1 is the PD
// diagnosis word (live or stale), or CC1/CC2 at rest, or D+/D- for a live session without PD; row 2 is the protocol and
// contract summary. (The USB link speed left Home: its detection is parked.)

void HomePage::paint_info(const Frame &f) {
    const hm::Mode m = f.mode;
    const bool live = m == hm::Mode::Live, stale = m == hm::Mode::Stale;

    // Row 1: the Black Box diagnosis of the PD session while live or stale (hm::format_diag: its worst finding, or the
    // state, in the finding's tone; an event finding only for one dwell, hm::DiagDwell); otherwise, and for a session
    // without PD (a legacy charger: the lines tell more), the measured lines (CC1/CC2 at rest, D+/D- live).
    hm::LinesText lt;
    hm::DiagText dt;
    std::string_view row1_tag = hm::kDiagTag, row1_val;
    const uint16_t row1_tag_col = (m == hm::Mode::Idle || m == hm::Mode::NoMeter) ? kFaint : kDim;
    uint16_t row1_val_col = kDim;
    const int32_t dp = ctx_.analog ? ctx_.analog->dp_mv : -1, dm = ctx_.analog ? ctx_.analog->dm_mv : -1;
    const pd::diag::Report &rep = pd::diag::live::report();
    if (hm::format_diag(m, rep, dt, ctx_.keep.diag_dwell.update(rep, f.now))) {
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

    // Row 1 (y201): graph window length (regular font_16 at x=4) + compact link/protocol summary at x=52 + elapsed + PC-link icon.
    hm::ProtoText pt;
    const bool starting = m == hm::Mode::NoMeter && hm::meter_starting(false, f.now);
    if (m == hm::Mode::NoMeter) {
        pt.value.put(starting ? "Power meter starting" : "Power meter not found");
    } else if (m == hm::Mode::Idle) {
        hm::format_idle_wait(ctx_.ui->link().wait_kind(), pt);
    } else {
        hm::format_proto(f.proto, pt);
        // Replace verbose "20V 5A" contract text with compact protocol mode + CC / limit diag (contract now lives in PD CHG/SNK box).
        const bool is_pd = f.proto.kind == hm::ProtoKind::PdFixed || f.proto.kind == hm::ProtoKind::PdEpr ||
                           f.proto.kind == hm::ProtoKind::Pps || f.proto.kind == hm::ProtoKind::Avs;
        if (is_pd) {
            pt.value.clear();
            pt.value.put(pt.tag.view());
            const uint8_t cc = fusb302::active_cc();
            if (cc == 1 || cc == 2) pt.value.put(" CC").put_char(static_cast<char>('0' + cc));
            if (live) {
                const pd::LimStatus &ls = fusb302::limit_status();
                if (ls.state != pd::LimState::Off) {
                    char lb[pd::kLimLabelMax + 1]{};
                    const std::size_t ln = pd::lim_label(ls.cfg, lb);
                    pt.value.put(" lim").put({lb, ln});
                }
            }
        } else if (!pt.tag.view().empty()) {
            // Non-PD: show tag + short value ("QC2 9V", "Type-C 3A")
            StrBuf<20> tmp;
            tmp.put(pt.tag.view());
            if (!pt.value.view().empty() && tmp.view().size() + 1 + pt.value.view().size() <= hm::kProtoChars)
                tmp.put_char(' ').put(pt.value.view());
            pt.value = tmp;
        }
    }
    uint16_t vcol = m == hm::Mode::NoMeter ? (starting ? kDim : kError)
                  : stale ? kStale
                  : (live && f.proto.kind != hm::ProtoKind::None && f.proto.kind != hm::ProtoKind::Detecting
                     && f.proto.kind != hm::ProtoKind::Unknown) ? kText : kDim;
    const raguard::CrossKind cross = (m == hm::Mode::Idle || m == hm::Mode::Stale) ? ctx_.ui->link().cc_cross() : raguard::CrossKind::None;
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

    hm::Item it[11];
    int n = 0;
    int zone_start[4];
    auto add = [&](const hm::Item &item) {
        if (item.kind == hm::ItemKind::Text && item.text.empty()) return;
        it[n++] = item;
    };
    add(hm::fill(0, hm::kRule3Y, hm::kW, 2, kRule));
    zone_start[0] = n;
    // Regular font_16 window length at x=4, aligned with Diag below.
    if (m != hm::Mode::NoMeter) add(hm::text(kSmall, hm::kInfoTagX, kRowTop[0], trend::label(window_), kDim));
    add(hm::text(kSmall, m == hm::Mode::NoMeter ? hm::kInfoTagX : hm::kInfoValX, kRowTop[0], pt.value.view(), vcol));
    zone_start[1] = n;
    {
        const int pad = static_cast<int>(eln) < kElChars ? kElChars - static_cast<int>(eln) : 0;
        add(hm::text(kSmall, static_cast<int16_t>(kElX + pad * kSmall.adv), kRowTop[0], {el, eln}, kDim));
    }
    if (const pclink::State pcs = ctx_.ui->pc_link();
        pcs != pclink::State::None && !ctx_.ui->uses_opp_rail() &&
        hm::kInfoValX + static_cast<int>(pt.value.view().size()) * static_cast<int>(kSmallAdvPx) <= kLinkX - 2)
        add(hm::link(kLinkX, kRowTop[0], static_cast<uint8_t>(pcs), pcs == pclink::State::Power ? kDim : kText, kFocus));
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

    commit_zoned_cell(6, static_cast<uint32_t>(m), mix_u32(mix_init(), static_cast<uint32_t>(m)),
                      cell, zone_start, kInfoZones, 3,
                      info_ok_, info_sig_, slice_info_, 0, hm::kRule3Y, hm::kW - 1, 239);
}

// ---- statics ---------------------------------------------------------------------------------

void HomePage::paint_statics() {
    statics_ok_ = true;
    info_ok_ = false;
    for (bool &v : cell_valid_) v = false;
    for (bool &v : box_valid_) v = false;
    for (int16_t &y : slice_y_) y = -1;
    for (int8_t &c : painted_cursor_) c = -1;
    slice_dirty_ = 0;
    rr_slot_ = 0;
    hd_[0].init(kHero, kHeroDigitX, kHeroDigitTop, kDigitChars);
    hd_[1].init(kMid, sub_digit_x(0), kSubDigitTop, kDigitChars);
    hd_[2].init(kMid, sub_digit_x(1), kSubDigitTop, kDigitChars);
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
    if (hero != shown_hero_) {
        shown_hero_ = hero;
        statics_ok_ = false;
        sweep_ok_ = false;
    }
    sample_values(f, now_ms);
    if (!statics_ok_) paint_statics();

    // Box availability mask & cycler step.
    const bool live = f.mode == hm::Mode::Live;
    const hm::Drop drop = hm::drop_of(live, ctx_.cable.has_idle(), ctx_.cable.idle_mv(), ctx_.cable.idle_ma(),
                                      abs32(ctx_.pm.vbus_mv), abs32(ctx_.pm.current_ma));
    const bool has_drop = drop.state == hm::DropState::Value && drop.mv > 0;
    const bool has_sag = ctx_.extremes.has_sag() && ctx_.extremes.sag_bp() > 0 && ctx_.extremes.sag_level_mv() > 0;
    const std::optional<adc::Reading> &an = ctx_.analog;
    const bool has_cc = an && (an->cc1_mv >= 0 || an->cc2_mv >= 0);
    const bool has_dpdm = an && (an->dp_mv >= 0 || an->dm_mv >= 0);
    hm::BoxMask ok = hm::box_bit(hm::BoxKind::Energy);
    if (f.proto.kind != hm::ProtoKind::None) ok |= hm::box_bit(hm::BoxKind::Pd);
    if (live && (has_drop || has_sag)) ok |= hm::box_bit(hm::BoxKind::Drop);
    if (fusb302::emarker().valid) ok |= hm::box_bit(hm::BoxKind::Cable);
    if (has_cc) ok |= hm::box_bit(hm::BoxKind::Cc);
    if (has_dpdm) ok |= hm::box_bit(hm::BoxKind::Dpdm);
    if (live && abs32(ctx_.pm.current_ma) >= 50) ok |= hm::box_bit(hm::BoxKind::LoadR);
    {
        const pd::ExtInfo &ext = fusb302::ext();
        const pd::IqStatus &iq = fusb302::interrogate_status();
        const pd::BatteryCaps &bat = ext.battery.valid ? ext.battery : iq.battery;
        const pd::BatteryStatus &bst = ext.battery_status.valid ? ext.battery_status : iq.battery_status;
        if (pd::battery_soc_pct(bat, bst) >= 0) ok |= hm::box_bit(hm::BoxKind::Battery);
    }
    ctx_.keep.boxes.tick(now_ms, live, ctx_.keep.home_boxes, ok);
    {   // Attention: when Drop or CC moves, highlight its pinned box or pull it into a Rotating box with 3s cyan fade.
        hm::Attention &att = ctx_.keep.attention;
        const hm::Card was = att.want();
        const hm::Card want = att.update(now_ms, live, ctx_.pm.vbus_mv, an ? an->cc1_mv : -1, an ? an->cc2_mv : -1,
                                         ctx_.extremes.peak_mw());
        if (want != was && want != hm::Card::Count) {
            const hm::BoxKind bk = want == hm::Card::Drop ? hm::BoxKind::Drop
                                 : want == hm::Card::Lines ? hm::BoxKind::Cc : hm::BoxKind::Off;
            if (bk != hm::BoxKind::Off) ctx_.keep.boxes.trigger_attention(now_ms, bk, ctx_.keep.home_boxes, ok);
        }
    }

    update_sweep(f, now_ms);
    const hm::Ledger led = hm::ledger_of(hero);
    const hm::Qty q[3] = {hero, led.row[0], led.row[1]};
    int start = rr_slot_ % 7;
    for (int k = 0; k < 7; ++k) {
        if (slice_y_[k] >= 0) { start = k; break; }
    }
    for (int off = 0; off < 7; ++off) {
        const int s = (start + off) % 7;
        if (s < 3) paint_qty(s, q[s], f);
        else if (s < 6) paint_box(s - 3, f);
        else paint_info(f);
        if (ctx_.budget && ctx_.budget->more) {
            rr_slot_ = static_cast<uint8_t>(slice_y_[s] >= 0 ? s : (s + 1) % 7);
            return;
        }
    }
}

}  // namespace k2
