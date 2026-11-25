// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Meter > Numbers (Fieldbook; it was the Detail page): V, A and W in font 24, each with its colour-key chip and a 3 px
// underline in the quantity's spot colour, then 16 key/value receipt cells in two columns (font 12 dim keys, font 16 ink
// values, a hairline under each row). Left: Energy, Charge, Time, Temp, R, CC1, CC2, Flow. Right: PDO, Cable, Max, D+, D-,
// Fast chg, USB, Alt. The masthead carries the Meter pager instead of the live readout (the values are the page).
// Rows are pure text over the measured values; the texts come from the home_*/detail_* helpers.
#include "k2/ui/ui_pages.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/pd/link.hpp"
#include "k2/ui/home_cell.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"
#include <array>

namespace k2 {

namespace {
// The three values: font 24, five cells each (60 px of a 70 px column, so no digit touches its neighbour), a 3 px underline under
// the value and a 14 px unit tag hanging from the underline's right end (the key chip; beside the digits it had no room).
// Option B 208 px width: 3 columns x 70 px pitch, 68 px underline (2 * 70 + 68 = 208).
constexpr int16_t kValY = 2, kValPitch = 70, kValChars = 5;
constexpr int16_t kUnderY = 28, kUnderH = 3, kUnderW = 68;
constexpr int16_t kChipW = 14, kChipH = 14, kChipX = kUnderW - kChipW, kChipY = kUnderY + kUnderH;
static_assert(2 * kValPitch + kUnderW <= 208, "the W column ends inside the 208 px content");
static_assert(kValChars * 12 <= kValPitch - 8, "a value leaves a gap before the next column");
static_assert(kValY + 24 < kUnderY, "the value cell ends above its underline");

// The receipt: 8 rows of 24 px from y 47, two columns (reclaiming TopBar + Footer height).
// Keys font 12 at x 2 / x 104 on the value's baseline; values font 16 right-aligned to x 100 / x 206.
constexpr int16_t kGridY0 = kChipY + kChipH + 2, kRowPitch = 24, kRows = 8;constexpr int16_t kKeyL = 2, kKeyR = 104, kValRtL = 100, kValRtR = 206;
constexpr int16_t kTextDy = 4;                                    // value line-box top inside a row
static_assert(kGridY0 + (kRows - 1) * kRowPitch + kTextDy + 16 <= 240, "the receipt ends inside the content");
constexpr int16_t kKeyDy = kTextDy + 12 - 9;                     // font 12 on the font 16 baseline (ascent 12 vs 9)
constexpr int16_t kRuleDy = kRowPitch - 1;

enum Slot : uint8_t {
    SV, SA, SW,
    SWh, SContract, SAh, SCable, STime, SMaxW, STemp, SDp, SOhm, SDm, SCc1, SFc, SCc2, SUsb, SFlow, SAlt,
    SCount
};
constexpr int kFirstCell = SWh;

constexpr int16_t row_top(int row) { return static_cast<int16_t>(kGridY0 + row * kRowPitch); }

struct Cell { const char *key; uint8_t chars; };
// In slot order from SWh: left, right, left, right ... The value widths are the cells' worst cases.
constexpr Cell kCells[SCount - kFirstCell] = {
    {"Wh", 7}, {"PDO", 9}, {"Ah", 7}, {"Cable", 8}, {"Time", 8}, {"Max", 5}, {"Temp", 4}, {"D+", 5},
    {"R", 6}, {"D-", 5}, {"CC1", 5}, {"FC", 8}, {"CC2", 5}, {"USB", 10}, {"Flow", 8}, {"Alt", 10},
};
constexpr bool keys_clear() {   // a key never reaches into its value's cells
    for (int i = 0; i < SCount - kFirstCell; i++) {
        const int key_end = ((i & 1) ? kKeyR : kKeyL) + 6 * static_cast<int>(std::string_view(kCells[i].key).size());
        const int val_x = ((i & 1) ? kValRtR : kValRtL) - 8 * kCells[i].chars;
        if (key_end + 2 > val_x) return false;
    }
    return true;
}
static_assert(keys_clear(), "Numbers: a key overlaps its value");

struct Pos { int16_t x, y; uint8_t chars; };
constexpr Pos pos_of(int slot) {
    if (slot < kFirstCell)
        return {static_cast<int16_t>(slot * kValPitch), kValY, static_cast<uint8_t>(kValChars)};
    const int i = slot - kFirstCell, row = i / 2;
    const int16_t right = (i & 1) ? kValRtR : kValRtL;
    return {static_cast<int16_t>(right - 8 * kCells[i].chars), static_cast<int16_t>(row_top(row) + kTextDy), kCells[i].chars};
}
constexpr int16_t key_x(int slot) { return ((slot - kFirstCell) & 1) ? kKeyR : kKeyL; }

const uint16_t &chip_fill(int q) { return q == 0 ? kSpotVolt : q == 1 ? kSpotAmp : kWattFill; }
const uint16_t &chip_ink(int q) { return q == 2 ? kWattInk : kSpotInk; }
constexpr const char *kChipLetter[3] = {"V", "A", "W"};

// The ground around what the page paints, each pixel once, so Numbers covers the whole content when it opens over a live
// page (no erase pass). A value's hole is its field box; a rule's hole is the rule.
void lay_values_ground(const Surface &c) {
    kit::Box h[3];
    for (int q = 0; q < 3; q++) {
        const Pos p = pos_of(q);
        h[q] = {p.x, p.y, static_cast<int16_t>(p.chars * kMid.adv), kMid.height};
    }
    kit::fill_except(c, {0, kit::kBelowBar, c.w, static_cast<int16_t>(kUnderY - kit::kBelowBar)}, h, 3, kBg);
    kit::Box u[6];
    for (int q = 0; q < 3; q++) {
        u[2 * q] = {static_cast<int16_t>(q * kValPitch), kUnderY, kUnderW, kUnderH};
        u[2 * q + 1] = {static_cast<int16_t>(q * kValPitch + kChipX), kChipY, kChipW, kChipH};
    }
    kit::fill_except(c, {0, kUnderY, c.w, static_cast<int16_t>(kGridY0 - kUnderY)}, u, 6, kBg);
}

void lay_row_ground(const Surface &c, int r) {
    kit::Box h[4];
    int n = 0;
    for (int col = 0; col < 2; col++) {
        const int slot = kFirstCell + 2 * r + col;
        const Pos p = pos_of(slot);
        h[n++] = {p.x, p.y, static_cast<int16_t>(p.chars * kSmall.adv), kSmall.height};
        h[n++] = {key_x(slot), static_cast<int16_t>(row_top(r) + kKeyDy),
                  static_cast<int16_t>(text_width(kLabelFont, kCells[slot - kFirstCell].key)), kLabelFont.height};
    }
    const bool ruled = r < kRows - 1;
    kit::fill_except(c, {0, row_top(r), c.w, ruled ? kRuleDy : kRowPitch}, h, n, kBg);
    if (ruled) {   // the rule row: the ground in the three gaps between the two rules (exact on a scaled page too)
        const int16_t y = static_cast<int16_t>(row_top(r) + kRuleDy);
        gfx_fill(c, 0, y, kKeyL, 1, kBg);
        gfx_fill(c, kValRtL, y, static_cast<int16_t>(kKeyR - kValRtL), 1, kBg);
        gfx_fill(c, kValRtR, y, static_cast<int16_t>(c.w - kValRtR), 1, kBg);
    }
}

}  // namespace

void DetailPage::reset_cells() {
    bar_valid_ = false;
    flow_sig_ = 0xFF;
    for (auto &cell : f_) cell.valid = false;
}

bool DetailPage::put(int slot, std::string_view sv, uint16_t color) {
    CellState &cell = f_[slot];
    const Pos p = pos_of(slot);
    const Font &font = slot < kFirstCell ? kMid : kSmall;
    if (ctx_.budget && !cell.valid && !ctx_.budget->admit(static_cast<int>(p.chars) * font.adv * font.height, true))
        return false;
    if (cell.fg != color) { cell.fg = color; cell.valid = false; }
    field_draw_buf(ctx_.content, font, p.x, p.y, p.chars, Align::Right, color, kBg, cell.valid, cell.cur, sv);
    return true;
}

void DetailPage::draw_static() {
    const Surface &c = ctx_.content;
    PaintBudget *const bg = ctx_.budget;
    if (static_step_ == 0) {
        if (bg && !bg->admit(140 * kBarH, true)) return;
        title_bar();
        reset_cells();
        static_step_ = 1;
    }
    if (static_step_ == 1) {
        const int cost = ctx_.blank ? 1800 : 4800;
        if (bg && !bg->admit(cost, true)) return;
        if (!ctx_.blank) lay_values_ground(c);   // a blank screen is kBg already
        for (int q = 0; q < 3; q++) {
            const int16_t x = static_cast<int16_t>(q * kValPitch);
            static_assert(kChipH == kit::kChipH, "the unit badge is a kit chip");
            kit::draw_chip(c, static_cast<int16_t>(x + kChipX), kChipY, kChipLetter[q], chip_fill(q), chip_ink(q), kChipW);
            gfx_fill(c, x, kUnderY, kUnderW, kUnderH, chip_fill(q));
        }
        static_step_ = 2;
    }
    while (static_step_ < 2 + kRows) {
        const int r = static_step_ - 2;
        const int cost = ctx_.blank ? 1200 : 3800;
        if (bg && !bg->admit(cost, true)) return;
        if (!ctx_.blank) lay_row_ground(c, r);
        if (r < kRows - 1) {
            gfx_fill(c, kKeyL, static_cast<int16_t>(row_top(r) + kRuleDy), kValRtL - kKeyL, 1, kRule);
            gfx_fill(c, kKeyR, static_cast<int16_t>(row_top(r) + kRuleDy), kValRtR - kKeyR, 1, kRule);
        }
        for (int col = 0; col < 2; col++) {
            const int s = kFirstCell + 2 * r + col;
            gfx_text(c, kLabelFont, key_x(s), static_cast<int16_t>(row_top(r) + kKeyDy), kCells[s - kFirstCell].key,
                     kDim, kBg);
        }
        static_step_++;
    }
    if (static_step_ == 2 + kRows) {
        const int16_t below = row_top(kRows);
        if (!ctx_.blank && below < c.h) {
            const int cost = static_cast<int>(c.w) * (c.h - below);
            if (bg && !bg->admit(cost, true)) return;
            gfx_fill(c, 0, below, c.w, static_cast<int16_t>(c.h - below), kBg);
        }
        static_step_++;
    }
}

void DetailPage::invalidate() {
    static_step_ = 0;
    reset_cells();
}

// The Flow cell: the physical arrow and where the current goes ("→ to top", "← bottom"), or a dim dash in the deadband or
// with no reading. The arrow turns with the screen (it points at the port, never a minus sign). One gfx_text right-aligned to
// x 100 over the part of the box it leaves, painted only when the direction or the rotation changes.
void DetailPage::paint_flow(bool live, power::Flow flow) {
    const uint8_t sig = !live || flow == power::Flow::Idle ? 0x80
                      : static_cast<uint8_t>((flow == power::Flow::Forward ? 0x40 : 0) | (ctx_.rotation & 3u));
    if (sig == flow_sig_) return;
    const Pos p = pos_of(SFlow);
    if (ctx_.budget && !ctx_.budget->admit(static_cast<int>(p.chars) * kSmall.adv * kSmall.height, true)) return;
    flow_sig_ = sig;
    static constexpr const char *kArrow[4] = {"\xE2\x86\x91", "\xE2\x86\x90", "\xE2\x86\x93", "\xE2\x86\x92"};   // up left down right
    char buf[16];
    std::string_view s = "--";
    if (sig != 0x80) {
        const bool fwd = flow == power::Flow::Forward;
        StrBuf<16> t;
        t.put(kArrow[home::arrow_dir(fwd, ctx_.rotation)]).put(fwd ? K2_P(" to top") : K2_P(" bottom"));
        const std::string_view v = t.view();
        for (size_t i = 0; i < v.size() && i < sizeof buf; i++) buf[i] = v[i];
        s = {buf, v.size() < sizeof buf ? v.size() : sizeof buf};
    }
    const int16_t w = static_cast<int16_t>(text_width(kSmall, s));
    const int16_t box = static_cast<int16_t>(p.chars * kSmall.adv);
    if (box > w) gfx_fill(ctx_.content, p.x, p.y, static_cast<int16_t>(box - w), kSmall.height, kBg);
    gfx_text(ctx_.content, kSmall, static_cast<int16_t>(p.x + box - w), p.y, s, sig == 0x80 ? kFaint : kText, kBg);
}

void DetailPage::update(uint32_t now_ms) {
    // Numbers is a live instrument: it never shows Flow's frozen LAST snapshot (ctx_.last), so an unplugged source reads
    // 0.00 V and the lines keep moving. Only Flow holds the last state.
    PaintBudget *const bg = ctx_.budget;
    if (bg && bg->more) return;
    const bool online = ctx_.pm_online;
    const power::Metrics &pm = ctx_.pm;
    const std::optional<adc::Reading> &an = ctx_.analog;
    const bool vbus = online && pm.vbus_mv >= 1000;
    const bool live = vbus;
    const uint16_t live_col = live ? kText : kFaint;
    StrBuf<16> s;

    if (online) {
        s.put_fixed(pm.vbus_mv, 2);
        if (!put(SV, s.view(), live_col)) return;
        s.clear(); s.put_abs_fixed(pm.current_ma, 3);
        if (!put(SA, s.view(), live_col)) return;
        // Five cells: "99.99" is the last two-decimal reading, from 100 W on one decimal ("180.0").
        s.clear(); s.put_abs_fixed(pm.power_mw, pm.power_mw >= 99995 ? 1 : 2);
        if (!put(SW, s.view(), live_col)) return;
    } else {
        if (!put(SV, "--.--", kFaint) || !put(SA, "-.---", kFaint) || !put(SW, "--.--", kFaint)) return;
    }

    // PDO: the negotiated contract ("20V 5A", PPS "5.4V 2.2A").
    const pd::Contract &ctr = fusb302::contract();
    {
        StrBuf<13> ct;
        home_contract_short(ctr, ctx_.fc, vbus ? pm.vbus_mv : 0, ct);
        const bool none = ct.view().empty();
        if (!put(SContract, none ? "--" : ct.view(), none ? kFaint : live_col)) return;
    }

    // Max: the largest offer of the attached source ("100W").
    {
        const pd::SourceCaps &caps = pd::active_caps();
        uint32_t mw = 0;
        for (int i = 0; i < caps.num_pdos && caps.valid; i++) mw = caps.pdos[i].max_mw > mw ? caps.pdos[i].max_mw : mw;
        s.clear();
        if (caps.valid && mw >= 1000) {
            s.put_u32((mw + 500) / 1000).put("W");
            if (!put(SMaxW, s.view(), live_col)) return;
        } else {
            if (!put(SMaxW, "--", kFaint)) return;
        }
    }

    // Cable: the e-marker's rating ("5A 100W"); EPR / active tags stay with the Cable place.
    {
        const pd::Emarker &em = fusb302::emarker();
        s.clear();
        if (em.valid && em.max_current_a) {
            s.put_u32(em.max_current_a).put_char('A');
            if (em.max_power_w) s.put_char(' ').put_u32(em.max_power_w).put_char('W');
        } else {
            s.put("--");
        }
        if (!put(SCable, s.view(), em.valid ? live_col : kFaint)) return;
    }

    {   // The lines, one cell each. They are measured whether or not VBUS is up (a waiting charger shows its Rp with VBUS dark).
        const int32_t mv[4] = {an ? an->cc1_mv : 0, an ? an->cc2_mv : 0, an ? an->dp_mv : 0, an ? an->dm_mv : 0};
        const int slot[4] = {SCc1, SCc2, SDp, SDm};
        for (int i = 0; i < 4; i++) {
            pg::put_unit_or_dash(s, an.has_value(), mv[i], 2, 'V');
            if (!put(slot[i], s.view(), an ? kText : kFaint)) return;
        }
    }

    {   // USB data link as the sniffed PD traffic describes it, and the alt mode: ink when negotiated, dim when only advertised.
        const usblink::Link ul = fusb302::usb_link(an ? an->dp_mv : -1, an ? an->dm_mv : -1);
        std::array<char, usblink::kUiMaxChars + 1> ub{}, ab{};
        usblink::format_speed(ul, ub);
        if (!put(SUsb, ub.data(), ul.conf == usblink::Confidence::Negotiated ? kText
                                : ul.conf == usblink::Confidence::Advertised ? kDim : kFaint)) return;
        const usblink::AltState as = usblink::format_alt(ul, ab, false);
        if (!put(SAlt, as == usblink::AltState::None ? "--" : ab.data(), as == usblink::AltState::Entered ? kText : as == usblink::AltState::Advertised ? kDim : kFaint)) return;
    }

    {   // Energy and charge, each with its unit.
        char whb[16], ahb[16];
        fmt_energy_pair(whb, ahb, ctx_.meter.mwh(), ctx_.meter.mah(), 5);
        s.clear(); s.put(pg::trim_left(whb)).put("Wh");
        if (!put(SWh, s.view(), kText)) return;
        s.clear(); s.put(pg::trim_left(ahb)).put("Ah");
        if (!put(SAh, s.view(), kText)) return;
    }

    {
        s.clear();
        s.put_hms(ctx_.meter.elapsed_s(now_ms));
        if (!put(STime, s.view(), kText)) return;
        StrBuf<8> tmp;
        temp_short_text(an, tmp);
        std::string_view ts = tmp.view();
        if (ts.substr(0, 3) == "MCU") ts.remove_prefix(3);
        const bool none = ts.empty() || ts == " --" || ts == "--";
        if (!put(STemp, none ? "--" : ts, none ? kFaint : kText)) return;
    }

    {   // Fast chg: the passive D+/D- verdict; R: the load resistance ("--" without a load).
        s.clear();
        if (ctx_.fc != fastcharge::Protocol::NoInput && ctx_.fc != fastcharge::Protocol::NoQuick &&
            ctx_.fc != fastcharge::Protocol::Pending)
            s.put(fastcharge::short_name(ctx_.fc));
        const bool none = s.view().empty();
        if (!put(SFc, none ? "--" : s.view(), none || !live ? kFaint : kText)) return;
        if (live && pm.current_ma) {
            char r[16];
            const int n = fmt_ohms(r, pm.vbus_mv, pm.current_ma);
            std::string_view rv = pg::trim_left({r, static_cast<size_t>(n)});
            s.clear();
            s.put(rv);
            if (rv != "--") s.put_char('R');
            if (!put(SOhm, s.view(), rv == "--" ? kFaint : kText)) return;
        } else {
            if (!put(SOhm, "--", kFaint)) return;
        }
    }

    paint_flow(live, pm.flow);
}

}  // namespace k2
