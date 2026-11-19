// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD spec section of a Deep charger test: what the Runner keeps of one pdspec run (k2/pdspec/*), as plain data. Pure: no
// engine, no driver. The Runner starts the engine through Env::spec_*; the session fills a Result from the engine's checks when the
// run ends; the journal, the results grid and the remote read it.
//
// Politeness (the Deep run stays a no-load conformance run): only the families that never provoke the charger run here, Transitions
// (fixed level pairs, up and down) and PPS / AVS (sweeps and slams inside the advertised APDO range), plus the Source reset observer
// (it only watches). PPS and EPR keep-alive silence (the source Hard Resets by design), the K2's own Hard Reset and the detach gap
// (Rd off) are left to the standalone `pd_spec` tooling and are not part of the Deep run. Every Request is inside the advertised
// capabilities (the engine takes its levels from the live list).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/core/build_config.h"
#include "k2/pdspec/engine.hpp"
#include "k2/pdspec/result.hpp"

namespace k2::specsec {

namespace sc = pdspec;

// False in the default image: the engine glue is not linked, a Deep run has no spec section and its Review is unchanged.
inline constexpr bool kBuilt = K2_PDSPEC != 0;

// The checks of the Deep run in the grid's order: transitions, PPS, AVS, the observer. The Result keeps one Cell per entry.
using C = sc::Check;
inline constexpr std::array kOrder{
    C::AcceptTime,   C::PsRdyTime,    C::SrcStart,     C::PsRdyEarly,   C::PsRdyLow,      C::RiseTime,     C::FallTime,
    C::Overshoot,    C::Accuracy,     C::PpsMonotonic, C::PpsAccuracy,  C::PpsSettle,     C::PpsBand,      C::PpsPsRdy,
    C::PpsOvershoot, C::PpsTransient, C::PpsStatus,    C::PpsStatusTime, C::PpsTrack,     C::AvsMonotonic, C::AvsAccuracy,
    C::AvsBand,      C::AvsPsRdy,     C::AvsOvershoot, C::AvsTransient, C::AvsTrack,      C::SourceReset,
};
inline constexpr std::size_t kCells = kOrder.size();
static_assert(kCells == 27, "the Deep spec section judges 27 checks");
// The engine families a Deep run selects (a bit per pdspec::Family: Transitions = 0, Pps = 1; the glue asserts the mapping).
inline constexpr std::uint8_t kFamilyMask = 0x03;

// The grid's section starts (a 2 px rule above the row): the PPS block, the AVS block and the observer.
constexpr bool section_start(std::size_t i) {
    return i < kCells && (kOrder[i] == C::PpsMonotonic || kOrder[i] == C::AvsMonotonic || kOrder[i] == C::SourceReset);
}

// How the stage ended. NotRun = it never began (no PD unit, a stop or an earlier fault): the grid shows no section.
enum class End : std::uint8_t { NotRun, Done, Cancelled, Fault, Refused };
inline constexpr std::uint8_t kEndMax = static_cast<std::uint8_t>(End::Refused);

// The worst measurement as the cell keeps it: the check's unit at the precision the grid shows (us in 0.1 ms, mV in 10 mV, ms and
// counts whole), rounded half away from zero, in 16 bits. kUnseen = the event never came (pdspec::kNever and above).
inline constexpr std::int16_t kUnseen = 32767;
constexpr std::int32_t unit_step(sc::Unit u) { return u == sc::Unit::Us ? 100 : u == sc::Unit::Mv ? 10 : 1; }
constexpr std::int16_t quantize(sc::Unit u, std::int32_t v) {
    if (v >= sc::kNever) return kUnseen;
    const std::int32_t q = unit_step(u), h = q / 2;
    std::int32_t r = v >= 0 ? (v + h) / q : -((-v + h) / q);
    if (r > 32766) r = 32766;
    if (r < -32766) r = -32766;
    return static_cast<std::int16_t>(r);
}
// The value back in the unit (pdspec::kNever for kUnseen), as the grid and the detail page show it.
constexpr std::int32_t dequantize(sc::Unit u, std::int16_t s) { return s == kUnseen ? sc::kNever : static_cast<std::int32_t>(s) * unit_step(u); }

// A limit the cell keeps (the bound the worst measurement was judged against): the same quantisation as the value, kOpenLimit and
// beyond (an overshoot's open side) as kUnseen. dequantize_limit takes the side back: an open low side reads -kOpenLimit.
constexpr std::int16_t quantize_limit(sc::Unit u, std::int32_t v) {
    return (v >= sc::kOpenLimit || v <= -sc::kOpenLimit) ? kUnseen : quantize(u, v);
}
constexpr std::int32_t dequantize_limit(sc::Unit u, std::int16_t s, bool high) {
    return s == kUnseen ? (high ? sc::kOpenLimit : -sc::kOpenLimit) : static_cast<std::int32_t>(s) * unit_step(u);
}

// One check's outcome. code: bits 0..2 = pdspec::Verdict (0 = Pending: nothing judged), bits 3..6 = pdspec::Skip; fails saturates.
// The detail the page shows (limit, count, place) rides in the cell, so a resumed run reads what the live one did. n = 0 on a
// measured cell = not kept (a result restored from a journal written before the detail record): the page reads dim "-".
struct Cell {
    std::uint8_t code = 0, fails = 0;
    std::int16_t value = 0;   // quantize(unit, worst); 0 when nothing was measured
    std::int16_t lo = 0, hi = 0;   // quantize_limit of the bounds the worst measurement was judged against
    std::uint16_t n = 0;           // the measurements the check judged (the engine's count, as the aggregate kept it)
    std::uint8_t a = 0, b = 0;     // where the worst one happened (pdspec::Agg a / b, spec_text.hpp put_where)
    constexpr bool detailed() const { return n != 0; }
    constexpr sc::Verdict verdict() const { return static_cast<sc::Verdict>(code & 7u); }
    constexpr sc::Skip skip() const { return static_cast<sc::Skip>((code >> 3) & 15u); }
    constexpr bool measured() const { return verdict() != sc::Verdict::Skip && verdict() != sc::Verdict::Pending; }
    constexpr bool operator==(const Cell&) const = default;
};
constexpr Cell make_cell(sc::Verdict v, sc::Skip s, unsigned fails, std::int16_t value = 0) {
    return {static_cast<std::uint8_t>(static_cast<unsigned>(v) | (static_cast<unsigned>(s) << 3)),
            static_cast<std::uint8_t>(fails > 255 ? 255 : fails), value};
}

struct Result {
    End end = End::NotRun;
    std::uint8_t why = 0;        // pdspec::Stop (Fault, Cancelled) or pdspec::StartResult (Refused); 0 = none
    bool unconfirmed = false;    // the end-of-run return to 5 V was not confirmed by the engine
    std::array<Cell, kCells> cells{};

    constexpr bool shown() const { return kBuilt && end != End::NotRun; }
    constexpr bool same(const Result& o) const {
        if (end != o.end || why != o.why || unconfirmed != o.unconfirmed) return false;
        for (std::size_t i = 0; i < kCells; ++i)
            if (!(cells[i] == o.cells[i])) return false;
        return true;
    }
    // pass / fail / skip counts as the grid's tally reads them (an unverified or pending cell is in none of the three).
    struct Tally { unsigned pass = 0, fail = 0, skip = 0; };
    constexpr Tally tally() const {
        Tally t;
        for (const Cell& c : cells) {
            switch (c.verdict()) {
                case sc::Verdict::Pass: ++t.pass; break;
                case sc::Verdict::Fail: ++t.fail; break;
                case sc::Verdict::Skip: ++t.skip; break;
                case sc::Verdict::Pending:
                case sc::Verdict::Unverified: break;
            }
        }
        return t;
    }
};
static_assert(sizeof(Result) == 4 + 12 * kCells, "RAM: three header bytes padded to four, then twelve bytes per cell");

// One check's cell from its aggregate (the verdict the pdspec result shows: an inferred limit is Unverified).
constexpr Cell cell_of(sc::Check c, const sc::Agg& a) {
    const auto k = static_cast<std::size_t>(c);
    const sc::Unit u = sc::kDefs[k].unit;
    Cell cell = make_cell(sc::verdict(c, a), a.skip, a.fails, a.n ? quantize(u, a.worst) : std::int16_t{0});
    if (a.n) {
        cell.lo = quantize_limit(u, a.lo); cell.hi = quantize_limit(u, a.hi);
        cell.n = a.n; cell.a = a.a; cell.b = a.b;
    }
    return cell;
}

// What the engine's aggregates say, one cell per kOrder entry. fill_from_aggs writes into a Result in place (the Runner's own, no
// stack copy of its 328 bytes); from_aggs is the by-value form for tests and tools.
constexpr void fill_from_aggs(Result& r, const std::array<sc::Agg, sc::kChecks>& agg, End e, std::uint8_t why, bool unconfirmed) {
    r.end = e; r.why = why; r.unconfirmed = unconfirmed;
    for (std::size_t i = 0; i < kCells; ++i) r.cells[i] = cell_of(kOrder[i], agg[static_cast<std::size_t>(kOrder[i])]);
}
constexpr Result from_aggs(const std::array<sc::Agg, sc::kChecks>& agg, End e, std::uint8_t why, bool unconfirmed) {
    Result r;
    fill_from_aggs(r, agg, e, why, unconfirmed);
    return r;
}

// A result for a stage that did not judge anything: every cell reads skipped with `why`.
constexpr Result all_skipped(End e, std::uint8_t why, sc::Skip reason) {
    Result r;
    r.end = e; r.why = why;
    for (Cell& c : r.cells) c = make_cell(sc::Verdict::Skip, reason, 0);
    return r;
}

// The wire form (the journal record /jr/sp.rec and the remote read 0x0B [6, off]): end, why, flags, then code, fails and the value
// (little endian) per cell. The detail (limits, count, place) is a second form below: both carry a one-byte total and offset.
inline constexpr std::size_t kWireBytes = 3 + 4 * kCells;
constexpr void to_wire(const Result& r, std::array<std::uint8_t, kWireBytes>& w) {
    w[0] = static_cast<std::uint8_t>(r.end); w[1] = r.why; w[2] = r.unconfirmed ? 1 : 0;
    for (std::size_t i = 0; i < kCells; ++i) {
        const auto v = static_cast<std::uint16_t>(r.cells[i].value);
        w[3 + 4 * i] = r.cells[i].code; w[4 + 4 * i] = r.cells[i].fails;
        w[5 + 4 * i] = static_cast<std::uint8_t>(v & 0xFF); w[6 + 4 * i] = static_cast<std::uint8_t>(v >> 8);
    }
}
// False = not a record of this version (a short or damaged one): the result stays NotRun.
constexpr bool from_wire(const std::uint8_t* w, std::size_t n, Result& r) {
    if (n != kWireBytes || w[0] > kEndMax || (w[2] & ~1u)) return false;
    Result o;
    o.end = static_cast<End>(w[0]); o.why = w[1]; o.unconfirmed = (w[2] & 1u) != 0;
    for (std::size_t i = 0; i < kCells; ++i) {
        o.cells[i] = {w[3 + 4 * i], w[4 + 4 * i], static_cast<std::int16_t>(static_cast<std::uint16_t>(w[5 + 4 * i] | (w[6 + 4 * i] << 8)))};
        if (static_cast<unsigned>(o.cells[i].verdict()) > static_cast<unsigned>(sc::Verdict::Unverified) ||
            static_cast<unsigned>(o.cells[i].skip()) > static_cast<unsigned>(sc::Skip::NoAvs))
            return false;
    }
    r = o;
    return true;
}

// The detail wire form (the journal record /jr/spd.rec and the remote read 0x0B [7, off]): per cell lo, hi, n (little endian), a, b.
// Apart from the first form so that a record written before it still loads (its cells read n = 0) and each form stays inside the
// one-byte total, offset and record length.
inline constexpr std::size_t kDetailBytes = 8 * kCells;
constexpr void to_detail_wire(const Result& r, std::array<std::uint8_t, kDetailBytes>& w) {
    for (std::size_t i = 0; i < kCells; ++i) {
        const Cell& c = r.cells[i];
        const auto lo = static_cast<std::uint16_t>(c.lo), hi = static_cast<std::uint16_t>(c.hi);
        std::uint8_t* p = w.data() + 8 * i;
        p[0] = static_cast<std::uint8_t>(lo & 0xFF); p[1] = static_cast<std::uint8_t>(lo >> 8);
        p[2] = static_cast<std::uint8_t>(hi & 0xFF); p[3] = static_cast<std::uint8_t>(hi >> 8);
        p[4] = static_cast<std::uint8_t>(c.n & 0xFF); p[5] = static_cast<std::uint8_t>(c.n >> 8);
        p[6] = c.a; p[7] = c.b;
    }
}
// Folds the detail into a result that already holds its cells (from_wire first). False = not a record of this form: nothing changes.
constexpr bool from_detail_wire(const std::uint8_t* w, std::size_t n, Result& r) {
    if (n != kDetailBytes) return false;
    for (std::size_t i = 0; i < kCells; ++i) {
        const std::uint8_t* p = w + 8 * i;
        Cell& c = r.cells[i];
        c.lo = static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8)));
        c.hi = static_cast<std::int16_t>(static_cast<std::uint16_t>(p[2] | (p[3] << 8)));
        c.n = static_cast<std::uint16_t>(p[4] | (p[5] << 8));
        c.a = p[6]; c.b = p[7];
    }
    return true;
}

// ---- per APDO (the range rows of a Deep run's PD block) -------------------------------------------------------------------------
// Each PPS / AVS APDO keeps its own cells, folded from the engine's measurements of it (Env::on_apdo) the way an Agg folds them, so
// the range's row carries its own spec word and its Detail lists its checks. A slot is one check; the PPS and AVS counterparts share
// it (Count = none on that kind). Slots 0..3 judge the level, 4..8 the timing, 9..10 the shape of a step.
inline constexpr std::size_t kApdoSlots = 11;
inline constexpr std::array<C, kApdoSlots> kApdoPps{C::PpsAccuracy, C::PpsMonotonic, C::PpsTrack, C::PpsStatus, C::PpsBand, C::PpsSettle,
                                                   C::PpsPsRdy, C::AcceptTime, C::PpsStatusTime, C::PpsOvershoot, C::PpsTransient};
inline constexpr std::array<C, kApdoSlots> kApdoAvs{C::AvsAccuracy, C::AvsMonotonic, C::AvsTrack, C::Count, C::AvsBand, C::Count,
                                                   C::AvsPsRdy, C::AcceptTime, C::Count, C::AvsOvershoot, C::AvsTransient};
constexpr std::size_t apdo_slot(C c) {
    for (std::size_t i = 0; i < kApdoSlots; ++i)
        if (kApdoPps[i] == c || kApdoAvs[i] == c) return i;
    return kApdoSlots;
}
constexpr C apdo_check(bool avs, std::size_t slot) { return slot < kApdoSlots ? (avs ? kApdoAvs : kApdoPps)[slot] : C::Count; }
enum class Group : std::uint8_t { Level, Timing, Shape };
constexpr Group slot_group(std::size_t slot) { return slot < 4 ? Group::Level : slot < 9 ? Group::Timing : Group::Shape; }
constexpr const char* group_word(Group g) { return g == Group::Level ? "level" : g == Group::Timing ? "timing" : "shape"; }

// Bit 7 of an APDO cell's code: the kept worst failed (its rank is redone from the quantised value, as Agg does from worst / lo / hi).
inline constexpr std::uint8_t kWorstFailed = 0x80;
constexpr void cell_add(C c, Cell& cell, const sc::Meas& m) {
    const sc::CheckDef& d = sc::def(c);
    const std::int32_t ex = sc::excess(d.kind, m);
    if (ex > 0 && cell.fails < 255) ++cell.fails;
    const sc::Meas kept{dequantize(d.unit, cell.value), 0, dequantize_limit(d.unit, cell.lo, false), dequantize_limit(d.unit, cell.hi, true)};
    bool worst_failed = (cell.code & kWorstFailed) != 0;
    if (cell.n == 0 || sc::Agg::rank(d.kind, m, ex > 0) > sc::Agg::rank(d.kind, kept, worst_failed)) {
        cell.value = quantize(d.unit, m.value);
        cell.lo = quantize_limit(d.unit, m.lo); cell.hi = quantize_limit(d.unit, m.hi);
        cell.a = m.a; cell.b = m.b; worst_failed = ex > 0;
    }
    if (cell.n < 0xFFFF) ++cell.n;
    const sc::Verdict v = d.inferred ? sc::Verdict::Unverified : cell.fails ? sc::Verdict::Fail : sc::Verdict::Pass;
    cell.code = static_cast<std::uint8_t>(static_cast<unsigned>(v) | (worst_failed ? kWorstFailed : 0u));
}

struct Apdo {
    std::uint8_t pos = 0;   // the APDO's position in the list (1-based, as Meas::a & 15 carries it)
    bool avs = false;
    std::array<Cell, kApdoSlots> cells{};
};
struct Apdos {
    std::array<Apdo, sc::kMaxProg> r{};
    std::uint8_t n = 0;
    constexpr const Apdo* find(std::uint8_t pos) const {
        for (std::size_t i = 0; i < n; ++i)
            if (r[i].pos == pos) return &r[i];
        return nullptr;
    }
    constexpr Apdo* find(std::uint8_t pos) { return const_cast<Apdo*>(static_cast<const Apdos*>(this)->find(pos)); }
    // A check with no slot, or a fifth APDO, is dropped (the merged cells still count it).
    constexpr void add(C c, const sc::Meas& m) {
        const std::size_t slot = apdo_slot(c);
        const auto pos = static_cast<std::uint8_t>(m.a & 15u);
        if (slot >= kApdoSlots || pos == 0) return;
        Apdo* p = find(pos);
        if (!p) {
            if (n >= r.size()) return;
            p = &r[n++];
            *p = Apdo{};
            p->pos = pos;
        }
        p->avs = p->avs || sc::avs_check(c);
        cell_add(c, p->cells[slot], m);
    }
};
static_assert(sizeof(Apdos) <= 540, "RAM: one copy, in the Runner's evidence");

// The range's word: Fail with the family of the judged failures (the larger weight x fails), Caution when only an inferred limit
// (PPS settle) was exceeded, Pass ("spec") when something was judged and all passed, None when the range measured nothing.
enum class RangeTone : std::uint8_t { None, Pass, Caution, Fail };
struct RangeSpec { RangeTone tone = RangeTone::None; Group group = Group::Level; };
constexpr RangeSpec range_spec(const Apdo& p) {
    std::array<std::uint32_t, 3> pen{};
    RangeSpec rs;
    for (std::size_t i = 0; i < kApdoSlots; ++i) {
        const Cell& c = p.cells[i];
        const C k = apdo_check(p.avs, i);
        if (c.n == 0 || k == C::Count) continue;
        const auto g = static_cast<std::size_t>(slot_group(i));
        if (sc::def(k).inferred) {
            if (c.fails && rs.tone != RangeTone::Fail && rs.tone != RangeTone::Caution) rs = {RangeTone::Caution, slot_group(i)};
        } else {
            pen[g] += std::uint32_t(sc::def(k).weight) * c.fails;
        }
        if (rs.tone == RangeTone::None) rs.tone = RangeTone::Pass;
    }
    std::size_t worst = 0;
    for (std::size_t g = 1; g < pen.size(); ++g)
        if (pen[g] > pen[worst]) worst = g;
    if (pen[worst]) rs = {RangeTone::Fail, static_cast<Group>(worst)};
    return rs;
}
// One range on the wire (the journal record /jr/sa<n>.rec, the remote read 0x0B [9, off]): pos, avs, then per slot the cell's code,
// fails, value, lo, hi, n (little endian), a, b. The remote reads a stream: the count, then each range's form (none: empty).
inline constexpr std::size_t kApdoCellBytes = 12, kApdoWireBytes = 2 + kApdoCellBytes * kApdoSlots;
static_assert(kApdoWireBytes % 2 == 0 && kApdoWireBytes <= 254, "one even journal record");
constexpr std::uint8_t apdo_wire_byte(const Apdo& p, std::size_t k) {
    if (k == 0) return p.pos;
    if (k == 1) return p.avs ? 1 : 0;
    const Cell& c = p.cells[(k - 2) / kApdoCellBytes];
    const auto u16 = [](std::int32_t v, bool hi) { return static_cast<std::uint8_t>(hi ? static_cast<std::uint16_t>(v) >> 8 : v & 0xFF); };
    switch ((k - 2) % kApdoCellBytes) {
        case 0: return c.code;
        case 1: return c.fails;
        case 2: case 3: return u16(c.value, (k - 2) % 2);
        case 4: case 5: return u16(c.lo, (k - 2) % 2);
        case 6: case 7: return u16(c.hi, (k - 2) % 2);
        case 8: case 9: return u16(c.n, (k - 2) % 2);
        case 10: return c.a;
        default: return c.b;
    }
}
constexpr void to_apdo_wire(const Apdo& p, std::array<std::uint8_t, kApdoWireBytes>& w) {
    for (std::size_t k = 0; k < kApdoWireBytes; ++k) w[k] = apdo_wire_byte(p, k);
}
// Adds the range (or replaces the one at its position). False = not a form of this version: nothing changes.
constexpr bool from_apdo_wire(const std::uint8_t* w, std::size_t n, Apdos& a) {
    if (n != kApdoWireBytes || w[0] == 0 || w[0] > 15 || w[1] > 1) return false;
    Apdo p;
    p.pos = w[0]; p.avs = w[1] != 0;
    const auto s16 = [](const std::uint8_t* q) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(q[0] | (q[1] << 8))); };
    for (std::size_t i = 0; i < kApdoSlots; ++i) {
        const std::uint8_t* q = w + 2 + kApdoCellBytes * i;
        Cell& c = p.cells[i];
        c = {q[0], q[1], s16(q + 2), s16(q + 4), s16(q + 6), static_cast<std::uint16_t>(q[8] | (q[9] << 8)), q[10], q[11]};
        if (static_cast<unsigned>(c.verdict()) > static_cast<unsigned>(sc::Verdict::Unverified)) return false;
    }
    Apdo* slot = a.find(p.pos);
    if (!slot) {
        if (a.n >= a.r.size()) return false;
        slot = &a.r[a.n++];
    }
    *slot = p;
    return true;
}
constexpr std::size_t apdo_stream_size(const Apdos& a) { return a.n ? 1 + a.n * kApdoWireBytes : 0; }
constexpr std::uint8_t apdo_stream_byte(const Apdos& a, std::size_t i) {
    return i == 0 ? a.n : apdo_wire_byte(a.r[(i - 1) / kApdoWireBytes], (i - 1) % kApdoWireBytes);
}

constexpr const char* range_word(const RangeSpec& rs) { return rs.tone == RangeTone::Pass ? "spec" : rs.tone == RangeTone::None ? "-" : group_word(rs.group); }

// The time the stage adds to a run's estimate, from the list (hardware, Anker IQ4: 12 transitions in 32 s, 248 programmable moves in
// 80 s): the fixed walk, then per programmable window either its cover, slams and status asks (the run's own sweep covered it) or the
// engine's full passes (the engine runs at most pdspec::kMaxRunMs).
inline constexpr std::uint32_t kTransitionMs = 2700, kProgMoveMs = 320, kCoveredWindowMs = 4000;
// kCoveredWindowMs holds the two slams and the asks; the cover's other moves are kProgMoveMs each.
inline std::uint32_t covered_window_ms(const sc::Prog& p) { return kCoveredWindowMs + (sc::cover_moves(p) - 2) * kProgMoveMs; }
inline std::uint32_t estimate_ms(const sc::Levels& L, bool pps_swept, bool avs_swept) {
    std::uint32_t ms = std::uint32_t(L.n_fixed > 1 ? L.n_fixed * (L.n_fixed - 1) : 0) * kTransitionMs;
    for (std::size_t w = 0; w < L.n_prog; ++w) {
        const bool swept = L.prog[w].kind == sc::ProgKind::Avs ? avs_swept : pps_swept;
        std::uint32_t moves = 0;
        for (std::uint8_t c = 0; c < sc::kClasses && !swept; ++c) moves += sc::measured_moves(sc::make_sweep(L.prog[w], c, false));
        ms += swept ? covered_window_ms(L.prog[w]) : moves * kProgMoveMs;
    }
    return ms;
}
// The Runner's own bound on the stage: the engine's cap plus its orderly end. A stage past it is stopped, then abandoned.
inline constexpr std::uint32_t kMaxMs = 1200000 + 30000;
inline constexpr std::uint32_t kStopGraceMs = 15000;

}  // namespace k2::specsec
