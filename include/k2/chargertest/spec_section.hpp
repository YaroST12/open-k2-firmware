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

// One check's outcome. code: bits 0..2 = pdspec::Verdict (0 = Pending: nothing judged), bits 3..6 = pdspec::Skip; fails saturates.
struct Cell {
    std::uint8_t code = 0, fails = 0;
    constexpr sc::Verdict verdict() const { return static_cast<sc::Verdict>(code & 7u); }
    constexpr sc::Skip skip() const { return static_cast<sc::Skip>((code >> 3) & 15u); }
};
constexpr Cell make_cell(sc::Verdict v, sc::Skip s, unsigned fails) {
    return {static_cast<std::uint8_t>(static_cast<unsigned>(v) | (static_cast<unsigned>(s) << 3)),
            static_cast<std::uint8_t>(fails > 255 ? 255 : fails)};
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
            if (cells[i].code != o.cells[i].code || cells[i].fails != o.cells[i].fails) return false;
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
static_assert(sizeof(Result) == 3 + 2 * kCells, "the journal record is 3 bytes of header and two per cell");

// What the engine's aggregates say, one cell per kOrder entry (the verdict the pdspec result shows: an inferred limit is Unverified).
constexpr Result from_aggs(const std::array<sc::Agg, sc::kChecks>& agg, End e, std::uint8_t why, bool unconfirmed) {
    Result r;
    r.end = e; r.why = why; r.unconfirmed = unconfirmed;
    for (std::size_t i = 0; i < kCells; ++i) {
        const auto k = static_cast<std::size_t>(kOrder[i]);
        r.cells[i] = make_cell(sc::verdict(kOrder[i], agg[k]), agg[k].skip, agg[k].fails);
    }
    return r;
}

// A result for a stage that did not judge anything: every cell reads skipped with `why`.
constexpr Result all_skipped(End e, std::uint8_t why, sc::Skip reason) {
    Result r;
    r.end = e; r.why = why;
    for (Cell& c : r.cells) c = make_cell(sc::Verdict::Skip, reason, 0);
    return r;
}

// The wire form (the journal record): end, why, flags, then code and fails per cell.
inline constexpr std::size_t kWireBytes = 3 + 2 * kCells;
constexpr void to_wire(const Result& r, std::array<std::uint8_t, kWireBytes>& w) {
    w[0] = static_cast<std::uint8_t>(r.end); w[1] = r.why; w[2] = r.unconfirmed ? 1 : 0;
    for (std::size_t i = 0; i < kCells; ++i) { w[3 + 2 * i] = r.cells[i].code; w[4 + 2 * i] = r.cells[i].fails; }
}
// False = not a record of this version (a short or damaged one): the result stays NotRun.
constexpr bool from_wire(const std::uint8_t* w, std::size_t n, Result& r) {
    if (n != kWireBytes || w[0] > kEndMax || (w[2] & ~1u)) return false;
    Result o;
    o.end = static_cast<End>(w[0]); o.why = w[1]; o.unconfirmed = (w[2] & 1u) != 0;
    for (std::size_t i = 0; i < kCells; ++i) {
        o.cells[i] = {w[3 + 2 * i], w[4 + 2 * i]};
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
