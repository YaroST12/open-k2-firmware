// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Detail page (k2-micro-gui phase 1, mock detail-mock2): a dense close-up grid. 8 rows x 25 px,
// 2 columns, font 16 values with 12 px dim labels and a 1 px hairline grid.
// Sections: POWER (V|Contract, A|Cable, W|MaxW), LINES (CC1/2|D+/D-), USB (USB|Alt), ENERGY (Wh|Ah),
// then Time|Temp and FastChg|R. The old flow row is gone (the load R answers "is anything drawn?").
// Rows are pure text over the measured values; the texts come from the home_*/detail_* helpers.
#include "k2/ui/ui_pages.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/pd/link.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"
#include <array>

namespace k2 {

namespace {
// Grid: the first boundary sits right under the chrome bar's hairline (y 21), 25 px pitch, 8 rows.
constexpr int16_t kGridY0 = 21, kRowPitch = 25, kRows = 8;
constexpr int16_t kMidX = 112;                    // the column hairline (mock x 120 of 240)
constexpr int16_t kLabL = 4, kLabR = 116;         // 12 px dim labels
constexpr int16_t kValRtL = 108, kValRtR = 220;   // value right edges of the two cells
constexpr int16_t kTextY = 5;                     // value line-box top inside a 25 px row (centre)
constexpr int16_t kLine1Y = 3, kLine2Y = 13;      // the two 12 px lines of the LINES row

enum Slot : uint8_t {
    SV, SContract, SA, SCable, SW, SMaxW,
    SCC, SDm,
    SUsb, SAlt,
    SWh, SAh,
    STime, STemp,
    SFc, SOhm,
    SCount
};

constexpr int16_t row_top(int row) { return static_cast<int16_t>(kGridY0 + row * kRowPitch); }

struct Pos { int16_t x, y; uint8_t chars; };
constexpr Pos kPos[SCount] = {
    {kValRtL - 7 * 8, row_top(0) + kTextY, 7},      // V            | contract (below)
    {kValRtR - 9 * 8, row_top(0) + kTextY, 9},      // contract "20V 5A" / "5.4V 2.2A"
    {kValRtL - 7 * 8, row_top(1) + kTextY, 7},      // A            | cable
    {kValRtR - 8 * 8, row_top(1) + kTextY, 8},      // cable "5A 240W"
    {kValRtL - 7 * 8, row_top(2) + kTextY, 7},      // W            | maxW
    {kValRtR - 5 * 8, row_top(2) + kTextY, 5},      // maxW "100W"
    {kValRtL - 9 * 8, row_top(3) + kTextY, 9},      // CC1/CC2 "1.62/0.02" | D+/D-
    {kValRtR - 9 * 8, row_top(3) + kTextY, 9},      // D+/D- "0.60/0.58"
    {kValRtL - 10 * 8, row_top(4) + kTextY, 10},    // USB          | alt mode
    {kValRtR - 8 * 8, row_top(4) + kTextY, 8},      // alt
    {kValRtL - 6 * 8, row_top(5) + kTextY, 6},      // Wh           | Ah
    {kValRtR - 6 * 8, row_top(5) + kTextY, 6},      // Ah
    {kValRtL - 8 * 8, row_top(6) + kTextY, 8},      // Time         | temp
    {kValRtR - 5 * 8, row_top(6) + kTextY, 5},      // temp
    {kValRtL - 5 * 8, row_top(7) + kTextY, 5},      // FastChg      | R
    {kValRtR - 7 * 8, row_top(7) + kTextY, 7},      // load R
};
static_assert(SCount <= 16, "DetailPage::kFields (8 rows x 2 columns)");

}  // namespace

void DetailPage::draw_static() {
    const Surface &c = ctx_.content;
    title_bar();
    // Hairline grid: one 1 px kRule line at every row boundary (the theme's only divider; sections read from the labels).
    for (int i = 0; i <= kRows; i++) gfx_fill(c, 0, row_top(i), c.w, 1, kRule);
    gfx_fill(c, kMidX, kGridY0, 1, static_cast<int16_t>(kRows * kRowPitch), kRule);
    struct Lab { int16_t x, y; const char *t; uint16_t col; };
    const Lab labs[] = {
        {kLabL, row_top(0) + kTextY, "V", kDim},
        {kLabL, row_top(1) + kTextY, "A", kDim},
        {kLabL, row_top(2) + kTextY, "W", kDim},
        {kLabL, row_top(5) + kTextY, "Wh", kDim},
        {kLabL, row_top(6) + kTextY, "Time", kDim},
        {kLabL, row_top(7) + kTextY, "FastChg", kDim},
        {kLabR, row_top(0) + kTextY, "PDO", kDim},       // the negotiated contract (label short: cell width)
        {kLabR, row_top(1) + kTextY, "Cable", kDim},
        {kLabR, row_top(2) + kTextY, "MaxW", kDim},
        {kLabR, row_top(4) + kTextY, "Alt", kDim},
        {kLabR, row_top(5) + kTextY, "Ah", kDim},
        {kLabR, row_top(6) + kTextY, "Temp", kDim},
        {kLabR, row_top(7) + kTextY, "R", kDim},
    };
    for (const Lab &l : labs) gfx_text(c, kLabelFont, l.x, l.y, l.t, l.col, kBg);
    gfx_text(c, kLabelFont, kLabL, row_top(4) + kTextY, "USB", kDim, kBg);
    // LINES row: the tags keep their line colours (blue D-, orange D+), the values one field each.
    gfx_text(c, kLabelFont, kLabL, row_top(3) + kLine1Y, "CC1", kDim, kBg);
    gfx_text(c, kLabelFont, kLabL, row_top(3) + kLine2Y, "CC2", kDim, kBg);
    gfx_text(c, kLabelFont, kLabR, row_top(3) + kLine1Y, "D+", kDPlus, kBg);
    gfx_text(c, kLabelFont, kLabR, row_top(3) + kLine2Y, "D-", kDMinus, kBg);
    gfx_text(c, kLabelFont, kLabR + 14, row_top(3) + kLine1Y, "/", kDim, kBg);
    gfx_text(c, kLabelFont, kLabR + 14, row_top(3) + kLine2Y, "/", kDim, kBg);
    for (int i = 0; i < SCount; i++)
        f_[i].init(c, kSmall, kPos[i].x, kPos[i].y, kPos[i].chars, Align::Right, kText, kBg);
    invalidate();
}

void DetailPage::invalidate() {
    bar_valid_ = false;
    for (int i = 0; i < SCount; i++) f_[i].invalidate();
}

void DetailPage::update(uint32_t now_ms) {
    // Detail is a live instrument: it never shows Home's frozen LAST snapshot (ctx_.last), so an unplugged source reads
    // 0.00 V and the lines keep moving. Only Home holds the last state.
    title_badge(false);
    const bool online = ctx_.pm_online;
    const power::Metrics &pm = ctx_.pm;
    const std::optional<adc::Reading> &an = ctx_.analog;
    const bool vbus = online && pm.vbus_mv >= 1000;
    const bool live = vbus;
    auto col = [&](uint16_t on) { return live ? on : kFaint; };
    auto put = [&](int slot, std::string_view s, uint16_t color) {
        f_[slot].set_colors(color, kBg);
        f_[slot].draw(s);
    };

    if (online) {
        StrBuf<12> v; v.put_fixed(pm.vbus_mv, 2).put(" V"); put(SV, v.view(), col(kVolt));
        StrBuf<12> a; a.put_abs_fixed(pm.current_ma, 2).put(" A"); put(SA, a.view(), col(kAmp));
        // The cell is 7 cells wide: "99.99 W" is the last two-decimal reading, from 100 W on one decimal ("180.0 W") fits.
        StrBuf<12> w; w.put_abs_fixed(pm.power_mw, pm.power_mw >= 100000 ? 1 : 2).put(" W"); put(SW, w.view(), col(kWatt));
    } else {
        put(SV, "-- V", kFaint); put(SA, "-- A", kFaint); put(SW, "-- W", kFaint);
    }

    // The negotiated contract ("20V 5A", PPS "5.4V 2.2A") in the volt row's right cell.
    const pd::Contract &ctr = fusb302::contract();
    {
        StrBuf<13> ct;
        home_contract_short(ctr, ctx_.fc, vbus ? pm.vbus_mv : 0, ct);
        if (ct.view().empty()) ct.put("--");
        put(SContract, ct.view(), col(kEnergy));
    }

    // MaxW: the largest offer of the attached source ("100W").
    {
        const pd::SourceCaps &caps = pd::active_caps();
        uint32_t mw = 0;
        for (int i = 0; i < caps.num_pdos && caps.valid; i++) mw = caps.pdos[i].max_mw > mw ? caps.pdos[i].max_mw : mw;
        if (caps.valid && mw >= 1000) {
            StrBuf<8> m; m.put_u32((mw + 500) / 1000).put("W");
            put(SMaxW, m.view(), col(kWatt));
        } else {
            put(SMaxW, "-- W", kFaint);
        }
    }

    // Cable: the e-marker's rating ("5A 240W"); EPR/active tags stay on the e-marker page.
    {
        const pd::Emarker &em = fusb302::emarker();
        StrBuf<10> cb;
        if (em.valid && em.max_current_a) {
            cb.put_u32(em.max_current_a).put_char('A');
            if (em.max_power_w) { cb.put_char(' ').put_u32(em.max_power_w).put_char('W'); }
        } else {
            cb.put("-- A");
        }
        put(SCable, cb.view(), em.valid ? col(kAmp) : kFaint);
    }

    {   // LINES row: "1.62/0.02" pairs; the connected CC line reads in the volt colour.
        const int cc = fusb302::active_cc();
        StrBuf<12> ccv, dmv;
        if (an) {
            ccv.put_fixed(an->cc1_mv, 2).put_char('/').put_fixed(an->cc2_mv, 2);
            dmv.put_fixed(an->dp_mv, 2).put_char('/').put_fixed(an->dm_mv, 2);
        } else {
            ccv.put("-.-/-.-"); dmv.put("-.-/-.-");
        }
        // The line levels are measured whether or not VBUS is up (a waiting charger shows its Rp with VBUS dark).
        put(SCC, ccv.view(), !an ? kFaint : live && cc ? kVolt : kText);
        put(SDm, dmv.view(), !an ? kFaint : kText);
    }

    {   // USB data link as the sniffed PD traffic describes it, and the alt mode beside it.
        const usblink::Link ul = fusb302::usb_link(an ? an->dp_mv : -1, an ? an->dm_mv : -1);
        std::array<char, usblink::kUiMaxChars + 1> ub{}, ab{};
        usblink::format_speed(ul, ub);
        const uint16_t cc2 = ul.conf == usblink::Confidence::Negotiated ? kOk
                           : ul.conf == usblink::Confidence::Advertised ? kCaution : kFaint;
        put(SUsb, ub.data(), cc2);
        const usblink::AltState as = usblink::format_alt(ul, ab, false);
        put(SAlt, ab.data(), as == usblink::AltState::Entered ? kOk
                             : as == usblink::AltState::Advertised ? kCaution : kFaint);
    }

    {   // Energy row
        char whb[16], ahb[16];
        fmt_energy_pair(whb, ahb, ctx_.meter.mwh(), ctx_.meter.mah(), 6);
        auto trim = [](const char *p) { while (*p == ' ') p++; return std::string_view(p); };
        put(SWh, trim(whb), kEnergy);
        put(SAh, trim(ahb), kCharge);
    }

    {
        StrBuf<8> el;
        el.put_hms(ctx_.meter.elapsed_s(now_ms));
        put(STime, el.view(), kText);
        StrBuf<8> tmp;
        temp_short_text(an, tmp);
        std::string_view ts = tmp.view();
        if (ts.substr(0, 3) == "MCU") ts.remove_prefix(3);
        if (ts.empty()) ts = "--";
        put(STemp, ts, kText);
    }

    {   // FastChg | R: the passive D+/D- verdict and the load resistance ("--" without a load).
        StrBuf<6> fp;
        if (ctx_.fc != fastcharge::Protocol::NoInput && ctx_.fc != fastcharge::Protocol::NoQuick &&
            ctx_.fc != fastcharge::Protocol::Pending)
            fp.put(fastcharge::short_name(ctx_.fc));
        if (fp.view().empty()) fp.put("--");
        put(SFc, fp.view(), live ? kAmp : kFaint);
        if (live && pm.current_ma) {
            char r[16];
            fmt_ohms(r, pm.vbus_mv, pm.current_ma);
            StrBuf<10> rs; rs.put(r);
            put(SOhm, rs.view(), kOhm);
        } else {
            put(SOhm, "-- R", kFaint);
        }
    }
}

}  // namespace k2
