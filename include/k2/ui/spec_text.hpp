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

#include "k2/chargertest/spec_section.hpp"
#include "k2/pdspec/result.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2::spectext {

namespace sc = pdspec;

// The ResultGrid's label strip (font 12, right-aligned at x55, kResultLabelW - 8 = 52 px): at most 8 characters, by sc::Check.
inline constexpr char kCheckShort[][9] = {
    "Accept",   "PS_RDY",  "VBUS go",  "Rdy high", "Rdy low",  "Rise",     "Fall",     "Oversh.",  "Level",    "PPS mono", "PPS lvl",
    "PPS set",  "PPS t/o", "EPR t/o",  "HR 0V",    "HR off",   "HR on",    "HR caps",  "Detach",   "Re-att.",  "Src rst",  "PPS band",
    "PPS rdy",  "PPS ovs", "PPS tr.",  "PPS stat", "Stat t",   "AVS mono", "AVS lvl",  "AVS band", "AVS rdy",  "AVS ovs",  "AVS tr.",
    "Re-caps",  "PPS trk", "AVS trk",
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

template <std::size_t N>
void put_int(StrBuf<N>& s, std::int32_t v) {
    if (v < 0) s.put_char('-');
    s.put_u32(v < 0 ? 0u - static_cast<std::uint32_t>(v) : static_cast<std::uint32_t>(v));   // INT32_MIN included
}

// One measured value in its unit: us as ms with one decimal, ms whole, mV as volts with two decimals, a count bare.
template <std::size_t N>
void put_value(StrBuf<N>& s, sc::Unit u, std::int32_t v) {
    if (v >= static_cast<std::int32_t>(sc::kNever)) { s.put("not seen"); return; }
    switch (u) {
        case sc::Unit::Us: s.put_fixed(v, 1).put("ms"); return;
        case sc::Unit::Ms: put_int(s, v); s.put("ms"); return;
        case sc::Unit::Mv: s.put_fixed(v, 2).put_char('V'); return;
        case sc::Unit::Count: put_int(s, v); return;
    }
}

// The limit the engine judged against (the aggregate keeps the bounds).
template <std::size_t N>
void put_limit(StrBuf<N>& s, const sc::CheckDef& d, const sc::Agg& a) {
    switch (d.kind) {
        case sc::Kind::Max: s.put("max "); put_value(s, d.unit, a.hi); return;
        case sc::Kind::Min: s.put("min "); put_value(s, d.unit, a.lo); return;
        case sc::Kind::Range:
            // One open side (an overshoot): only the bound that applies.
            if (a.lo <= -sc::kOpenLimit) { s.put("max "); put_value(s, d.unit, a.hi); return; }
            if (a.hi >= sc::kOpenLimit) { s.put("min "); put_value(s, d.unit, a.lo); return; }
            put_value(s, d.unit, a.lo);
            s.put_char('-');
            put_value(s, d.unit, a.hi);
            return;
    }
}

// Where the worst measurement happened (the aggregate's a / b, result.hpp "where"). false = no object ("the source").
template <std::size_t N>
bool put_where(int k, std::uint8_t a, std::uint8_t b, StrBuf<N>& s) {
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
template <std::size_t N>
kit::Verdict cell_view(int check, const specsec::Cell& c, StrBuf<N>& text, std::string_view& shown) {
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

// One Detail row. `a` is the engine's aggregate of the same run when it is still the one this cell came from (the live run), else
// null: then the limit, the count and the place, which the cell does not keep, read a dim "-".
inline void detail_value(int row, int check, const specsec::Cell& c, const sc::Agg* a, StrBuf<20>& s, std::uint16_t& color) {
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
            if (measured && a && a->n) put_limit(s, d, *a);
            else { s.put(measured ? "-" : "not judged"); color = kDim; }
            return;
        case 3:
            if (a && measured) s.put_u32(a->n);
            else { s.put(measured ? "-" : "0"); if (measured) color = kDim; }
            return;
        case 4: s.put_u32(c.fails); color = c.fails && judged ? kCaution : kText; return;
        case 5:
            if (!measured) { s.put("none"); color = kDim; return; }
            if (!a) { s.put("-"); color = kDim; return; }
            if (!put_where(check, a->a, a->b, s)) color = kDim;
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

}  // namespace k2::spectext
