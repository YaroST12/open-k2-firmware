// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD spec section's words and numbers, as pure text over a specsec::Cell: the ResultGrid label of each check, the value in
// its unit, the grid cell, and the rows of the spec check's detail page. No driver, no UI state: the charger-test page draws what
// these return, the host tests read it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

#include "k2/chargertest/spec_rows.hpp"   // empty without K2_SPECROWS
#include "k2/chargertest/spec_section.hpp"
#include "k2/pdspec/plan.hpp"
#include "k2/pdspec/result.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2::spectext {

namespace sc = pdspec;

// The ResultGrid's label strip (font 12, kResultLabelW - 8 = 46 px): at most 7 characters, by sc::Check.
inline constexpr char kCheckShort[][8] = {
    "Accept",  "PS_RDY",  "VBUS go", "Rdy hi",  "Rdy low", "Rise",    "Fall",    "Oversh",  "Level",   "PPS mon", "PPS lvl",
    "PPS set", "PPS t/o", "EPR t/o", "HR 0V",   "HR off",  "HR on",   "HR caps", "Detach",  "Re-att",  "Src rst", "PPS bnd",
    "PPS rdy", "PPS ovs", "PPS tr",  "PPS st",  "Stat t",  "AVS mon", "AVS lvl", "AVS bnd", "AVS rdy", "AVS ovs", "AVS tr",
    "Re-caps", "PPS trk", "AVS trk",
};
static_assert(std::size(kCheckShort) == sc::kChecks, "one grid label per check");
static_assert(all_fit(kCheckShort, kLabelAdv, kit::kResultLabelW - 8), "a check label is wider than the label strip");

inline constexpr const char* label(sc::Check c) { return kCheckShort[static_cast<std::size_t>(c)]; }

// The detail page's keys (a KvTable at x72, 18 characters of value).
inline constexpr const char* kDetailKeys[] = {"Verdict", "Worst", "Limit", "Count", "Fails", "At", "Penalty"};
inline constexpr int kDetailRows = static_cast<int>(std::size(kDetailKeys));
static_assert(all_fit(kDetailKeys, kSmallAdvPx, kit::kKvValueX - 6 - 4), "a Detail key runs into the value column");
// A skip reason as a Detail value (18 characters) and as the lone grid cell (font 16, 150 px).
static_assert(fits_px(kSmallAdvPx, sc::skip_name(sc::Skip::NoEprProof), 150), "a skip reason is wider than its cell");

inline void put_int(StrBufBase& s, std::int32_t v) {
    if (v < 0) s.put_char('-');
    s.put_u32(v < 0 ? 0u - static_cast<std::uint32_t>(v) : static_cast<std::uint32_t>(v));   // INT32_MIN included
}

// One measured value in its unit: us as ms with one decimal, ms whole, mV as volts with two decimals, a count bare.
inline void put_value(StrBufBase& s, sc::Unit u, std::int32_t v) {
    if (v >= static_cast<std::int32_t>(sc::kNever)) { s.put("not seen"); return; }
    switch (u) {
        case sc::Unit::Us: s.put_fixed(v, 1).put("ms"); return;
        case sc::Unit::Ms: put_int(s, v); s.put("ms"); return;
        case sc::Unit::Mv: s.put_fixed(v, 2).put_char('V'); return;
        case sc::Unit::Count: put_int(s, v); return;
    }
}

// The limit a check was judged against, from its bounds in the check's unit.
inline void put_limit(StrBufBase& s, const sc::CheckDef& d, std::int32_t lo, std::int32_t hi) {
    switch (d.kind) {
        case sc::Kind::Max: s.put("max "); put_value(s, d.unit, hi); return;
        case sc::Kind::Min: s.put("min "); put_value(s, d.unit, lo); return;
        case sc::Kind::Range:
            // One open side (an overshoot): only the bound that applies.
            if (lo <= -sc::kOpenLimit) { s.put("max "); put_value(s, d.unit, hi); return; }
            if (hi >= sc::kOpenLimit) { s.put("min "); put_value(s, d.unit, lo); return; }
            put_value(s, d.unit, lo);
            s.put_char('-');
            put_value(s, d.unit, hi);
            return;
    }
}
// The limit the cell kept (live and restored alike).
inline void put_limit(StrBufBase& s, const sc::CheckDef& d, const specsec::Cell& c) {
    put_limit(s, d, specsec::dequantize_limit(d.unit, c.lo, false), specsec::dequantize_limit(d.unit, c.hi, true));
}

// Where the worst measurement happened (the aggregate's a / b, result.hpp "where"). false = no object ("the source").
inline bool put_where(int k, std::uint8_t a, std::uint8_t b, StrBufBase& s) {
    const bool fixed_walk = k < static_cast<int>(sc::Check::PpsMonotonic);
    if (fixed_walk && b < 16) {
        s.put("PDO ").put_u32(a).put(" to ").put_u32(b);
    } else if (fixed_walk || k < static_cast<int>(sc::Check::PpsKeepAlive) ||
               (k >= static_cast<int>(sc::Check::PpsBand) && k <= static_cast<int>(sc::Check::AvsTransient)) ||
               k >= static_cast<int>(sc::Check::PpsTrack)) {
        // A programmable move: APDO, step class, direction and the level asked for ("APDO 5 c2 dn 10.9V").
        s.put("APDO ").put_u32(a & 0x0Fu).put(" c").put_u32((a >> 4) & 7u).put(a & 0x80u ? " dn " : " up ");
        s.put_u32(b / 10u).put(".").put_u32(b % 10u).put("V");
    } else if (a) {
        s.put("PDO ").put_u32(a);
    } else {
        s.put("the source");
        return false;
    }
    return true;
}

// The grid cell of one check (one wide VerdictCell). `text` holds the number; `shown` is what the cell prints.
//   skip: the reason, struck out. pending: empty. unverified (an inferred limit): the number, no pass/fail look.
//   pass / fail: the number, then ", N fail" when it failed.
inline kit::Verdict cell_view(int check, const specsec::Cell& c, StrBufBase& text, std::string_view& shown) {
    const sc::Unit u = sc::kDefs[static_cast<std::size_t>(check)].unit;
    text.clear();
    shown = {};
    switch (c.verdict()) {
        case sc::Verdict::Skip: shown = sc::skip_name(c.skip()); return kit::Verdict::Skip;
        case sc::Verdict::Pending: return kit::Verdict::Pending;
        case sc::Verdict::Unverified:
            put_value(text, u, specsec::dequantize(u, c.value));
            shown = text.view();
            return kit::Verdict::None;
        case sc::Verdict::Pass:
        case sc::Verdict::Fail:
            put_value(text, u, specsec::dequantize(u, c.value));
            if (c.fails) text.put(", ").put_u32(c.fails).put(" fail");
            shown = text.view();
            return c.fails ? kit::Verdict::Fail : kit::Verdict::Pass;
    }
    return kit::Verdict::None;
}

// One Detail row. The limit, the count and the place come from the cell (live run and journal resume alike); a cell restored from a
// journal written before the detail record has none (n = 0) and reads a dim "-".
inline void detail_value(int row, int check, const specsec::Cell& c, StrBufBase& s, std::uint16_t& color) {
    color = kText;
    const sc::CheckDef& d = sc::kDefs[static_cast<std::size_t>(check)];
    const sc::Verdict v = c.verdict();
    const bool measured = c.measured();
    const bool judged = v == sc::Verdict::Pass || v == sc::Verdict::Fail;
    switch (row) {
        case 0:
            switch (v) {
                case sc::Verdict::Pass: s.put("Pass"); color = kOk; return;
                case sc::Verdict::Fail: s.put("Fail"); color = kError; return;
                case sc::Verdict::Unverified: s.put("Unverified"); return;
                case sc::Verdict::Skip: s.put("Skip"); color = kDim; return;
                case sc::Verdict::Pending: s.put("Pending"); color = kDim; return;
            }
            return;
        case 1:
            if (measured) put_value(s, d.unit, specsec::dequantize(d.unit, c.value));
            else { s.put(sc::skip_name(c.skip())); color = kDim; }
            return;
        case 2:
            if (measured && c.detailed()) put_limit(s, d, c);
            else { s.put(measured ? "-" : "not judged"); color = kDim; }
            return;
        case 3:
            if (c.detailed()) s.put_u32(c.n);
            else { s.put(measured ? "-" : "0"); if (measured) color = kDim; }
            return;
        case 4: s.put_u32(c.fails); color = c.fails && judged ? kCaution : kText; return;
        case 5:
            if (!measured) { s.put("none"); color = kDim; return; }
            if (!c.detailed()) { s.put("-"); color = kDim; return; }
            if (!put_where(check, c.a, c.b, s)) color = kDim;
            return;
        case 6: {
            const std::uint32_t pts = judged ? static_cast<std::uint32_t>(d.weight) * c.fails : 0;
            s.put_u32(pts);
            color = pts ? kCaution : kText;
            return;
        }
        default: return;
    }
}

// ---- Layout B: a range's own checks -------------------------------------------------------------------------------------------
// A range line's footer when its word is not "spec": the check behind the word, its worst value and its fails ("PPS ovs: 9.90V, 2 fail";
// over an inferred limit "2 over"), shortened to the 27 footer cells by dropping the value, then the count.
inline void range_footer(StrBufBase& f, const specsec::Apdo& p, const specsec::RangeSpec& rs) {
    const sc::Check c = specsec::apdo_check(p.avs, rs.slot);
    if (c == sc::Check::Count) return;
    const specsec::Cell& cell = p.cells[rs.slot];
    const sc::Unit u = sc::def(c).unit;
    StrBuf<48> g;
    for (int part = 2; part >= 0; --part) {
        g.clear();
        g.put(label(c)).put(": ");
        if (part != 1) put_value(g, u, specsec::dequantize(u, cell.value));
        if (part == 2) g.put(", ");
        if (part >= 1) g.put_u32(cell.fails).put(sc::def(c).inferred ? " over" : " fail");
        if (text_cells(g.view()) <= 27) break;
    }
    f.put(g.view());
}
// A range's spec row on its Steps page: the worst value as the tail (none when nothing was measured) ...
inline void slot_tail(int check, const specsec::Cell& c, StrBufBase& s) {
    const sc::Unit u = sc::kDefs[static_cast<std::size_t>(check)].unit;
    if (c.measured()) put_value(s, u, specsec::dequantize(u, c.value));
}
// ... and the limit and count as its note (31 cells): "max 9.65V, 14 measured", the skip reason, or "not judged".
inline void slot_note(int check, const specsec::Cell& c, StrBufBase& s) {
    if (c.verdict() == sc::Verdict::Skip) { s.put(sc::skip_name(c.skip())); return; }
    if (!c.measured()) { s.put("not judged"); return; }
    if (!c.detailed()) { s.put("-"); return; }
    StrBuf<48> g;
    for (int part = 2; part >= 0; --part) {
        g.clear();
        put_limit(g, sc::kDefs[static_cast<std::size_t>(check)], c);
        if (part) g.put(", ").put_u32(c.n);
        if (part == 2) g.put(" measured");
        if (text_cells(g.view()) <= static_cast<int>(kit::kNoteMaxPx / kLabelAdv)) break;
    }
    s.put(g.view());
}

#if K2_SPECROWS
// ---- the per-transition rows (specsec::Row): name, value, tail ----------------------------------------------------------------------
// A row of the spec detail list reads  5->9V      9.21V  -0.29V : the pair (or the programmable window and step) at x28, the value in the
// check's own unit at x104, the margin right-aligned to x214. Every text keeps its column for any row (a row read back from a journal
// can hold anything): the name 9 cells (fixed) / 16 (programmable), the value and the tail 6.

namespace rowtext {

inline constexpr int kNameFixedCells = 9, kNameProgCells = 16, kNumCells = 6;

// The number part of a fixed-point text (a '+' before a positive one when asked), as precise as the room allows: max_dec decimals,
// then fewer until it fits `width` characters.
inline void fit_fixed(StrBufBase& out, std::int32_t milli, int max_dec, int width, bool plus) {
    for (int d = max_dec;; --d) {
        out.clear();
        if (plus && milli > 0) out.put_char('+');
        out.put_fixed(milli, d);
        if (static_cast<int>(out.view().size()) <= width || d == 0) return;
    }
}

// A time given in tenths of a millisecond: 4.5ms, 12ms, 169ms, 1.5s. `plus` prefixes '+' to a positive one (a margin). 6 cells at most.
inline void put_time(StrBufBase& s, std::int32_t tenths, bool plus) {
    const bool neg = tenths < 0;
    const std::uint32_t a = neg ? 0u - static_cast<std::uint32_t>(tenths) : static_cast<std::uint32_t>(tenths);
    if (neg) s.put_char('-');
    else if (plus && a) s.put_char('+');
    if (a < 100u) s.put_fixed(static_cast<std::int32_t>(a * 100u), 1).put("ms");
    else if (a < 9950u) s.put_u32((a + 5u) / 10u).put("ms");
    else s.put_fixed(static_cast<std::int32_t>((a + 500u) / 1000u * 100u), 1).put_char('s');
}

// Millivolts as volts, 'V' included, 6 cells at most.
inline void put_volts(StrBufBase& s, std::int32_t mv, bool plus) {
    StrBuf<16> t;
    fit_fixed(t, mv, 2, kNumCells - 1, plus);
    s.put(t.view()).put_char('V');
}

// One level of a pair, with the decimals it has (at most `dec`): 9, 9.5, 9.25.
inline void put_level(StrBufBase& s, std::uint16_t mv, int dec) {
    const int nat = mv % 100u ? 2 : mv % 1000u ? 1 : 0;
    s.put_fixed(mv, nat < dec ? nat : dec);
}

}  // namespace rowtext

constexpr bool row_known(const specsec::Row& r) { return r.check < sc::kChecks; }

// 5->9V, 12->15V, 5->9.5V. A programmable pass: its window and step class, 3.3-11V 100mV, 15-28V slam.
inline void put_row_name(const specsec::Row& r, StrBufBase& s) {
    if (r.flags & specsec::kRowProg) {
        rowtext::put_level(s, r.a, 1);
        s.put_char('-');
        rowtext::put_level(s, r.b, 1);
        s.put_char('V').put_char(' ');
        const std::uint8_t cls = specsec::row_class(r);
        if (cls >= sc::kSlam) {
            s.put("slam");
        } else {
            const bool avs = row_known(r) && sc::avs_check(static_cast<sc::Check>(r.check));
            const std::int32_t mv = sc::step_mv(avs ? sc::ProgKind::Avs : sc::ProgKind::Pps, cls);
            if (mv >= 1000) s.put_u32(static_cast<std::uint32_t>(mv / 1000)).put_char('V');
            else s.put_u32(static_cast<std::uint32_t>(mv)).put("mV");
        }
        return;
    }
    // The pair keeps its decimals while it fits the 9 cells, then loses them.
    for (int dec = 2;; --dec) {
        StrBuf<24> t;
        rowtext::put_level(t, r.a, dec);
        t.put("\xE2\x86\x92");
        rowtext::put_level(t, r.b, dec);
        t.put_char('V');
        if (text_cells(t.view()) <= rowtext::kNameFixedCells || dec == 0) { s.put(t.view()); return; }
    }
}

// The value in the check's unit: 9.21V, 4.5ms, 169ms, 1.5s; signed millivolts for a programmable pass's error; `never` for no event.
inline void put_row_value(const specsec::Row& r, StrBufBase& s) {
    if (!row_known(r)) return;
    if (r.value == specsec::kUnseen) { s.put("never"); return; }
    const std::int32_t v = r.value;
    switch (sc::kDefs[r.check].unit) {
        case sc::Unit::Us: rowtext::put_time(s, v, false); return;
        case sc::Unit::Ms: rowtext::put_time(s, v * 10, false); return;
        case sc::Unit::Mv:
            if (!(r.flags & specsec::kRowProg)) { rowtext::put_volts(s, v * 10, false); return; }
            if (v > -100 && v < 100) {   // under a volt: whole millivolts with their sign
                if (v > 0) s.put_char('+');
                put_int(s, v * 10);
                s.put("mV");
            } else {
                rowtext::put_volts(s, v * 10, true);
            }
            return;
        case sc::Unit::Count: put_int(s, v); return;
    }
}

// The margin past the nearest limit, signed (positive = outside), in the value's unit; a programmable pass: how many of its steps failed.
inline void put_row_tail(const specsec::Row& r, StrBufBase& s) {
    if (!row_known(r)) return;
    if (r.flags & specsec::kRowProg) {
        const std::int32_t n = r.margin < 0 ? 0 : r.margin;
        if (n > 99) s.put(">99");
        else if (n) s.put_u32(static_cast<std::uint32_t>(n)).put(" bad");
        else s.put((r.flags & specsec::kRowFail) ? "bad" : "ok");
        return;
    }
    if (r.value == specsec::kUnseen || r.margin == specsec::kUnseen) return;
    const std::int32_t m = r.margin;
    switch (sc::kDefs[r.check].unit) {
        case sc::Unit::Us: rowtext::put_time(s, m, true); return;
        case sc::Unit::Ms: rowtext::put_time(s, m * 10, true); return;
        case sc::Unit::Mv: rowtext::put_volts(s, m * 10, true); return;
        case sc::Unit::Count: if (m > 0) s.put_char('+'); put_int(s, m); return;
    }
}
#endif  // K2_SPECROWS

}  // namespace k2::spectext
