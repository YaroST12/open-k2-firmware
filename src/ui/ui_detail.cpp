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
// The three values: font 24, five cells each, a 12 x 16 key chip after the digits, a 3 px underline under value and chip.
constexpr int16_t kValY = 27, kValPitch = 75, kValChars = 5;
constexpr int16_t kChipX = kValChars * 12 + 1, kChipW = 12, kChipH = 16, kChipY = kValY + 4;
constexpr int16_t kUnderY = 53, kUnderH = 3, kUnderW = kChipX + kChipW;   // 73 px
static_assert(2 * kValPitch + kUnderW <= 224, "the W column ends inside the content");

// The receipt: 8 rows of 22 px from y 60, two columns. Keys font 12 at x 4 / x 106 on the value's baseline; values font 16
// right-aligned to x 100 / x 222; a hairline under every row but the last (x 4..100 / 106..222).
constexpr int16_t kGridY0 = 60, kRowPitch = 22, kRows = 8;
constexpr int16_t kKeyL = 4, kKeyR = 106, kValRtL = 100, kValRtR = 222;
constexpr int16_t kTextDy = 3;                                    // value line-box top inside a row
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
    {"Energy", 7}, {"PDO", 9}, {"Charge", 7}, {"Cable", 8}, {"Time", 8}, {"Max", 5}, {"Temp", 4}, {"D+", 5},
    {"R", 6}, {"D-", 5}, {"CC1", 5}, {"Fast chg", 8}, {"CC2", 5}, {"USB", 10}, {"Flow", 8}, {"Alt", 10},
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
void lay_ground(const Surface &c) {
    {   // the values band, from under the masthead to the underline
        kit::Box h[6];
        int n = 0;
        for (int q = 0; q < 3; q++) {
            const Pos p = pos_of(q);
            h[n++] = {p.x, p.y, static_cast<int16_t>(p.chars * kMid.adv), kMid.height};
            h[n++] = {static_cast<int16_t>(p.x + kChipX), kChipY, kChipW, kChipH};
        }
        kit::fill_except(c, {0, kit::kBelowBar, c.w, static_cast<int16_t>(kUnderY - kit::kBelowBar)}, h, n, kBg);
        kit::Box u[3];
        for (int q = 0; q < 3; q++) u[q] = {static_cast<int16_t>(q * kValPitch), kUnderY, kUnderW, kUnderH};
        kit::fill_except(c, {0, kUnderY, c.w, static_cast<int16_t>(kGridY0 - kUnderY)}, u, 3, kBg);
    }
    for (int r = 0; r < kRows; r++) {
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
    const int16_t below = row_top(kRows);
    if (below < c.h) gfx_fill(c, 0, below, c.w, static_cast<int16_t>(c.h - below), kBg);
}

}  // namespace

void DetailPage::draw_static() {
    const Surface &c = ctx_.content;
    title_bar();
    if (!ctx_.blank) lay_ground(c);   // a blank screen is kBg already
    for (int q = 0; q < 3; q++) {
        const int16_t x = static_cast<int16_t>(q * kValPitch);
        gfx_fill(c, static_cast<int16_t>(x + kChipX), kChipY, kChipW, kChipH, chip_fill(q));
        gfx_text(c, kLabelFont, static_cast<int16_t>(x + kChipX + 3), static_cast<int16_t>(kChipY + 2), kChipLetter[q], chip_ink(q),
                 chip_fill(q));
        gfx_fill(c, x, kUnderY, kUnderW, kUnderH, chip_fill(q));
    }
    for (int r = 0; r < kRows - 1; r++) {
        gfx_fill(c, kKeyL, static_cast<int16_t>(row_top(r) + kRuleDy), kValRtL - kKeyL, 1, kRule);
        gfx_fill(c, kKeyR, static_cast<int16_t>(row_top(r) + kRuleDy), kValRtR - kKeyR, 1, kRule);
    }
    for (int s = kFirstCell; s < SCount; s++)
        gfx_text(c, kLabelFont, key_x(s), static_cast<int16_t>(row_top((s - kFirstCell) / 2) + kKeyDy), kCells[s - kFirstCell].key,
                 kDim, kBg);
    for (int i = 0; i < SCount; i++) {
        const Pos p = pos_of(i);
        if (i == SFlow) continue;   // painted whole by paint_flow (its arrow is a multi-byte glyph)
        f_[i].init(c, i < kFirstCell ? kMid : kSmall, p.x, p.y, p.chars, Align::Right, kText, kBg);
    }
    invalidate();
}

void DetailPage::invalidate() {
    bar_valid_ = false;
    flow_sig_ = 0xFF;
    for (int i = 0; i < SCount; i++) f_[i].invalidate();
}

// The Flow cell: the physical arrow and where the current goes ("→ to top", "← bottom"), or a dim dash in the deadband or
// with no reading. The arrow turns with the screen (it points at the port, never a minus sign). One gfx_text right-aligned to
// x 100 over the part of the box it leaves, painted only when the direction or the rotation changes.
void DetailPage::paint_flow(bool live, power::Flow flow) {
    const uint8_t sig = !live || flow == power::Flow::Idle ? 0x80
                      : static_cast<uint8_t>((flow == power::Flow::Forward ? 0x40 : 0) | (ctx_.rotation & 3u));
    if (sig == flow_sig_) return;
    flow_sig_ = sig;
    static constexpr const char *kArrow[4] = {"\xE2\x86\x91", "\xE2\x86\x90", "\xE2\x86\x93", "\xE2\x86\x92"};   // up left down right
    char buf[16];
    std::string_view s = "--";
    if (sig != 0x80) {
        const bool fwd = flow == power::Flow::Forward;
        StrBuf<16> t;
        t.put(kArrow[home::arrow_dir(fwd, ctx_.rotation)]).put(fwd ? " to top" : " bottom");
        const std::string_view v = t.view();
        for (size_t i = 0; i < v.size() && i < sizeof buf; i++) buf[i] = v[i];
        s = {buf, v.size() < sizeof buf ? v.size() : sizeof buf};
    }
    const Pos p = pos_of(SFlow);
    const int16_t w = static_cast<int16_t>(text_width(kSmall, s));
    const int16_t box = static_cast<int16_t>(p.chars * kSmall.adv);
    if (box > w) gfx_fill(ctx_.content, p.x, p.y, static_cast<int16_t>(box - w), kSmall.height, kBg);
    gfx_text(ctx_.content, kSmall, static_cast<int16_t>(p.x + box - w), p.y, s, sig == 0x80 ? kFaint : kText, kBg);
}

void DetailPage::update(uint32_t now_ms) {
    // Numbers is a live instrument: it never shows Flow's frozen LAST snapshot (ctx_.last), so an unplugged source reads
    // 0.00 V and the lines keep moving. Only Flow holds the last state.
    title_pager(1);
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
        StrBuf<12> v; v.put_fixed(pm.vbus_mv, 2); put(SV, v.view(), col(kText));
        StrBuf<12> a; a.put_abs_fixed(pm.current_ma, 3); put(SA, a.view(), col(kText));
        // Five cells: "99.99" is the last two-decimal reading, from 100 W on one decimal ("180.0").
        StrBuf<12> w; w.put_abs_fixed(pm.power_mw, pm.power_mw >= 99995 ? 1 : 2); put(SW, w.view(), col(kText));
    } else {
        put(SV, "--.--", kFaint); put(SA, "-.---", kFaint); put(SW, "--.--", kFaint);
    }

    // PDO: the negotiated contract ("20V 5A", PPS "5.4V 2.2A").
    const pd::Contract &ctr = fusb302::contract();
    {
        StrBuf<13> ct;
        home_contract_short(ctr, ctx_.fc, vbus ? pm.vbus_mv : 0, ct);
        const bool none = ct.view().empty();
        put(SContract, none ? "--" : ct.view(), none ? kFaint : col(kText));
    }

    // Max: the largest offer of the attached source ("100W").
    {
        const pd::SourceCaps &caps = pd::active_caps();
        uint32_t mw = 0;
        for (int i = 0; i < caps.num_pdos && caps.valid; i++) mw = caps.pdos[i].max_mw > mw ? caps.pdos[i].max_mw : mw;
        if (caps.valid && mw >= 1000) {
            StrBuf<8> m; m.put_u32((mw + 500) / 1000).put("W");
            put(SMaxW, m.view(), col(kText));
        } else {
            put(SMaxW, "--", kFaint);
        }
    }

    // Cable: the e-marker's rating ("5A 100W"); EPR / active tags stay with the Cable place.
    {
        const pd::Emarker &em = fusb302::emarker();
        StrBuf<10> cb;
        if (em.valid && em.max_current_a) {
            cb.put_u32(em.max_current_a).put_char('A');
            if (em.max_power_w) { cb.put_char(' ').put_u32(em.max_power_w).put_char('W'); }
        } else {
            cb.put("--");
        }
        put(SCable, cb.view(), em.valid ? col(kText) : kFaint);
    }

    {   // The lines, one cell each. They are measured whether or not VBUS is up (a waiting charger shows its Rp with VBUS dark).
        const int32_t mv[4] = {an ? an->cc1_mv : 0, an ? an->cc2_mv : 0, an ? an->dp_mv : 0, an ? an->dm_mv : 0};
        const int slot[4] = {SCc1, SCc2, SDp, SDm};
        for (int i = 0; i < 4; i++) {
            StrBuf<8> s;
            if (an) s.put_fixed(mv[i] < 0 ? 0 : mv[i], 2).put_char('V'); else s.put("--");
            put(slot[i], s.view(), an ? kText : kFaint);
        }
    }

    {   // USB data link as the sniffed PD traffic describes it, and the alt mode: ink when negotiated, dim when only advertised.
        const usblink::Link ul = fusb302::usb_link(an ? an->dp_mv : -1, an ? an->dm_mv : -1);
        std::array<char, usblink::kUiMaxChars + 1> ub{}, ab{};
        usblink::format_speed(ul, ub);
        put(SUsb, ub.data(), ul.conf == usblink::Confidence::Negotiated ? kText
                             : ul.conf == usblink::Confidence::Advertised ? kDim : kFaint);
        const usblink::AltState as = usblink::format_alt(ul, ab, false);
        put(SAlt, ab.data(), as == usblink::AltState::Entered ? kText : as == usblink::AltState::Advertised ? kDim : kFaint);
    }

    {   // Energy and charge, each with its unit.
        char whb[16], ahb[16];
        fmt_energy_pair(whb, ahb, ctx_.meter.mwh(), ctx_.meter.mah(), 5);
        auto trim = [](const char *p) { while (*p == ' ') p++; return std::string_view(p); };
        StrBuf<10> wh, ah;
        wh.put(trim(whb)).put("Wh");
        ah.put(trim(ahb)).put("Ah");
        put(SWh, wh.view(), kText);
        put(SAh, ah.view(), kText);
    }

    {
        StrBuf<10> el;
        el.put_hms(ctx_.meter.elapsed_s(now_ms));
        put(STime, el.view(), kText);
        StrBuf<8> tmp;
        temp_short_text(an, tmp);
        std::string_view ts = tmp.view();
        if (ts.substr(0, 3) == "MCU") ts.remove_prefix(3);
        const bool none = ts.empty() || ts == " --" || ts == "--";
        put(STemp, none ? "--" : ts, none ? kFaint : kText);
    }

    {   // Fast chg: the passive D+/D- verdict; R: the load resistance ("--" without a load).
        StrBuf<10> fp;
        if (ctx_.fc != fastcharge::Protocol::NoInput && ctx_.fc != fastcharge::Protocol::NoQuick &&
            ctx_.fc != fastcharge::Protocol::Pending)
            fp.put(fastcharge::short_name(ctx_.fc));
        const bool none = fp.view().empty();
        put(SFc, none ? "--" : fp.view(), none || !live ? kFaint : kText);
        if (live && pm.current_ma) {
            char r[16];
            const int n = fmt_ohms(r, pm.vbus_mv, pm.current_ma);
            StrBuf<10> rs;
            std::string_view rv{r, static_cast<size_t>(n)};
            while (!rv.empty() && rv.front() == ' ') rv.remove_prefix(1);
            rs.put(rv);
            if (rv != "--") rs.put_char('R');
            put(SOhm, rs.view(), rv == "--" ? kFaint : kText);
        } else {
            put(SOhm, "--", kFaint);
        }
    }

    paint_flow(live, pm.flow);
}

}  // namespace k2
