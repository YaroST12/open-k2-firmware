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
// The value back in the unit (pdspec::kNever for kUnseen), as the old Spec page's put_value took it.
constexpr std::int32_t dequantize(sc::Unit u, std::int16_t s) { return s == kUnseen ? sc::kNever : static_cast<std::int32_t>(s) * unit_step(u); }

// One check's outcome. code: bits 0..2 = pdspec::Verdict (0 = Pending: nothing judged), bits 3..6 = pdspec::Skip; fails saturates.
struct Cell {
    std::uint8_t code = 0, fails = 0;
    std::int16_t value = 0;   // quantize(unit, worst); 0 when nothing was measured
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
static_assert(sizeof(Result) == 4 + 4 * kCells, "RAM: three header bytes padded to four, then four bytes per cell");

// One check's cell from its aggregate (the verdict the pdspec result shows: an inferred limit is Unverified).
constexpr Cell cell_of(sc::Check c, const sc::Agg& a) {
    const auto k = static_cast<std::size_t>(c);
    return make_cell(sc::verdict(c, a), a.skip, a.fails, a.n ? quantize(sc::kDefs[k].unit, a.worst) : std::int16_t{0});
}

// What the engine's aggregates say, one cell per kOrder entry.
constexpr Result from_aggs(const std::array<sc::Agg, sc::kChecks>& agg, End e, std::uint8_t why, bool unconfirmed) {
    Result r;
    r.end = e; r.why = why; r.unconfirmed = unconfirmed;
    for (std::size_t i = 0; i < kCells; ++i) r.cells[i] = cell_of(kOrder[i], agg[static_cast<std::size_t>(kOrder[i])]);
    return r;
}

// A result for a stage that did not judge anything: every cell reads skipped with `why`.
constexpr Result all_skipped(End e, std::uint8_t why, sc::Skip reason) {
    Result r;
    r.end = e; r.why = why;
    for (Cell& c : r.cells) c = make_cell(sc::Verdict::Skip, reason, 0);
    return r;
}

// The wire form (the journal record): end, why, flags, then code, fails and the value (little endian) per cell.
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

// The time the stage adds to a run's estimate: the engine takes about two minutes for the transitions and the PPS sweeps on a charger
// that has both (the engine runs at most pdspec::kMaxRunMs).
inline constexpr std::uint32_t kEstimateMs = 120000;
// The Runner's own bound on the stage: the engine's cap plus its orderly end. A stage past it is stopped, then abandoned.
inline constexpr std::uint32_t kMaxMs = 1200000 + 30000;
inline constexpr std::uint32_t kStopGraceMs = 15000;

}  // namespace k2::specsec
