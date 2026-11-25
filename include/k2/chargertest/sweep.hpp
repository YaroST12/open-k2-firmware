// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger-test deep sweep (ROADMAP "PPS deep sweep", "AVS deep sweep"): a pure point plan over one advertised
// programmable range plus the summary of the recorded points. No state, no heap: the plan is a range and a count, the
// record is a caller-owned array of one int16 error per point. Host-tested in tests/test_sweep.cpp.
//
//   Points: every kStepMv across the APDO, from ceil(min_mv) to floor(min(max_mv, cap_mv)) - never above the monitor
//   ceiling (cap_mv), at 1 A never above the APDO's current at that level nor the cable's limit.
//   Execution: one result row per APDO (a ct::Step marked kDetailSweep); each point is a Request through the sink's
//   single gate, judged on Accept + PS_RDY + measured VBUS. A steady out-of-band point is RECORDED, not a stop - the
//   sweep stops only on a lost source (the other PPS rows' pattern handling: needs-load, capped, floored, withdrawn).
//   Summary: min / max / worst error (measured - setpoint) and where, the peak delivered level and the setpoint it came
//   at, and the first fold (pd::track_step: VBUS falling by more than 50 mV while the request rises; the rule's track break
//   needs steps over 100 mV, so these 100 mV sweeps never show one) - the FH22 shape: tracks 60-150 mV low to 19.9 V, then
//   every request >= 20.0 V folds back to 19.42 V. A per-point verdict stays as it was; the finding is an extra, like Capped.
#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include "k2/core/bss_zero.hpp"
#include <span>
#include "k2/pd/pd.hpp"
#include "k2/pd/prog_track.hpp"
#include "k2/core/strpack.hpp"

namespace k2::ctsweep {

// A point plan per range kind; the AVS sweep (100 mV is the AVS Request granularity) reuses the PPS one.
enum class Kind : uint8_t { Pps, Avs };
inline constexpr unsigned kStepMv = 100;      // every 100 mV across the range (the PPS quantum is 20 mV)
inline constexpr unsigned kSweepMa = 1000;    // 1 A per point, clamped by point_ma() to the offer and the cable
// All APDOs of one plan together: the record is one int16 error per point, kMaxPoints * 2 B, no heap. A plan that
// would exceed it sweeps fewer APDOs (plan_total counts the same budget), so the Tests-list count and the plan agree.
inline constexpr unsigned kMaxPoints = 640;
inline constexpr unsigned kMaxRows = 12;      // one result row per APDO part (a PPS sweep is two, split at 5 V); the card keeps at most 4 ranges

// Sweep depth (Settings "Sweep depth"): how finely the deep sweeps step. The record stays one error per 100 mV cell whatever the
// depth (kMaxPoints counts cells); a cell the sweep did not measure holds kHole. Step100 = 0 so a journal / card written before
// the depth existed reads as the sweep it ran.
//   Step100  every 100 mV.                          Step200  every 200 mV (a subset of the 100 mV points).
//   Phone    100 mV, and 20 mV (PPS only) inside the phone windows kWin, each window swept once, on the APDO with the most current.
//   Step20   every PPS quantum (20 mV); AVS keeps 100 mV, its Request granularity.
// Every depth ends on the APDO top; a finding between two points is located by Refiner at finer steps (100 mV, then 20 mV).
enum class Depth : uint8_t { Step100 = 0, Step200 = 1, Phone = 2, Step20 = 3 };
inline constexpr unsigned kDepthCount = 4;
// The Settings cycle (and Review's) runs 200 mV, 100 mV, Phone, 20 mV: coarse to fine, the default first.
constexpr Depth next_depth(Depth d) {
    switch (d) {
    case Depth::Step200: return Depth::Step100;
    case Depth::Step100: return Depth::Phone;
    case Depth::Phone: return Depth::Step20;
    default: return Depth::Step200;
    }
}
// The depth's short name for the lists (fits a 12 px tail and a Review tail: no longer than "200 mV").
constexpr const char* depth_name(Depth d) {
    switch (d) {
    case Depth::Step200: return K2_P("200 mV");
    case Depth::Step100: return K2_P("100 mV");
    case Depth::Phone: return K2_P("Phone");
    default: return K2_P("20 mV");
    }
}
inline constexpr int16_t kHole = INT16_MIN;     // an unmeasured cell of the point record
inline constexpr unsigned kFineMv = 20;         // the PPS Request quantum
inline constexpr unsigned kWinCount = 2;
inline constexpr unsigned kWinMv[kWinCount][2] = {{6800, 11000}, {14000, 19000}};   // the phone windows, [lo, hi] setpoints
// A depth's step at a setpoint (wins = bit per kWin window this APDO sweeps finely).
constexpr unsigned depth_step(Depth d, Kind k, unsigned wins, unsigned mv) {
    if (d == Depth::Step200) return 200;
    if (k != Kind::Pps) return kStepMv;
    if (d == Depth::Step20) return kFineMv;
    if (d == Depth::Phone)
        for (unsigned w = 0; w < kWinCount; ++w)
            if ((wins >> w & 1u) && mv >= kWinMv[w][0] && mv < kWinMv[w][1]) return kFineMv;
    return kStepMv;
}

// The point range of one advertised APDO range [apdo_min_mv, apdo_max_mv], capped at cap_mv, on kStepMv boundaries.
// lo > hi (empty) when nothing fits. lo/hi are setpoints in mV.
struct PointRange { unsigned lo = 0, hi = 0; bool empty() const { return hi < lo; } };
constexpr PointRange point_range(unsigned apdo_min_mv, unsigned apdo_max_mv, unsigned cap_mv) {
    const unsigned lo = (apdo_min_mv + kStepMv - 1) / kStepMv * kStepMv;
    const unsigned hi = (std::min(apdo_max_mv, cap_mv) / kStepMv) * kStepMv;
    return {lo, hi};
}
constexpr unsigned point_count(PointRange r) { return r.empty() ? 0u : (r.hi - r.lo) / kStepMv + 1u; }
// The sweep of one APDO is split at kSplitMv (user decision 2026-11-14): the part at and above it is a PD row right after the
// APDO's own checks (it runs self-powered too); the part below it is a low programmable row of the PdLow unit (external power,
// last of the run: the low edge is where latch-off sources cut). Either part may be empty.
inline constexpr unsigned kSplitMv = 5000;
constexpr PointRange part_range(PointRange r, bool low) {
    if (low) return {r.lo, std::min(r.hi, kSplitMv - kStepMv)};
    return {std::max(r.lo, kSplitMv), r.hi};
}
// The setpoint of point i of a row that starts at r.lo.
constexpr unsigned point_mv(PointRange r, unsigned i) { return r.lo + i * kStepMv; }
// The current a point asks: 1 A, never above the APDO's current at that level nor the cable's limit.
constexpr unsigned point_ma(const pd::Pdo& p, unsigned mv, unsigned cable_ma) {
    return std::min({kSweepMa, unsigned(pd::current_limit_ma(p, uint16_t(mv))), cable_ma});
}
// One row's point sequence (the record is the 100 mV cell grid of its PointRange, whatever the depth): the first point is lo, each
// next one a depth_step() up, the last always the row's top - the APDO's exact limit when it is on the 20 mV grid and lies in
// the last cell, else the range's hi. A point is on the grid (a record cell) when it is lo + n * kStepMv, else it is only judged.
struct Plan {
    Depth depth = Depth::Step100;
    Kind kind = Kind::Pps;
    uint8_t wins = 0;          // bit per kWinMv window this APDO sweeps at 20 mV (phone_wins)
    uint16_t lo = 0, hi = 0;   // the row's cell range (a PointRange)
    uint16_t top = 0;          // the last point
    unsigned first() const { return lo; }
    unsigned next(unsigned mv) const {   // 0 = the row is done
        if (mv >= top) return 0;
        const unsigned n = mv + depth_step(depth, kind, wins, mv);
        return n < top ? n : unsigned(top);
    }
};
// limit_mv = the APDO's own limit (min of its max and the caller's ceiling): its exact value is the top when it falls inside the
// last cell on the 20 mV grid (a PPS APDO of 11.02 V ends at 11.02, not 11.0).
constexpr Plan make_plan(Depth d, Kind k, PointRange part, unsigned limit_mv, unsigned wins) {
    Plan p;
    p.depth = d; p.kind = k; p.wins = uint8_t(wins); p.lo = uint16_t(part.lo); p.hi = uint16_t(part.hi); p.top = uint16_t(part.hi);
    if (k == Kind::Pps && limit_mv > part.hi && limit_mv - part.hi < kStepMv && limit_mv % kFineMv == 0) p.top = uint16_t(limit_mv);
    return p;
}
// The record cell of a setpoint, -1 for a point off the 100 mV grid.
constexpr int plan_cell(const Plan& p, unsigned mv) {
    return mv >= p.lo && (mv - p.lo) % kStepMv == 0 ? int((mv - p.lo) / kStepMv) : -1;
}
// How many points the row measures (the Running view and the estimate; the record's size is point_count).
constexpr unsigned plan_points(const Plan& p) {
    unsigned n = 0;
    for (unsigned mv = p.lo; mv; mv = p.next(mv)) ++n;
    return n;
}
// Which phone windows (bits of kWinMv) the APDO in slot `slot` sweeps at 20 mV: each window goes to ONE PPS APDO - the one with the
// most current at the window, then the widest overlap, then the first - clamped to the APDO and cap_mv.
unsigned phone_wins(const pd::SourceCaps& caps, unsigned cap_mv, unsigned slot);

// The total point count of a sweep over a caps list (what the Tests list line shows), within the kMaxPoints budget,
// APDOs in list order (the planner emits them in the same order). Kind::Pps sweeps the SPR PPS APDOs (slots 1..7);
// Kind::Avs the AVS APDOs (SPR AVS in slots 1..7, EPR AVS in slots 8+ when the EPR list is in hand). cap_mv = the
// effective ceiling already clamped by the caller to the monitor ceiling and the cable's voltage limit.
// It counts the points MEASURED at `depth` (a 200 mV sweep is half of the 100 mV cells); the budget stays in cells.
unsigned plan_total(const pd::SourceCaps& caps, unsigned cap_mv, Kind kind = Kind::Pps, Depth depth = Depth::Step100);
// The count with the caller's monitor ceiling and cable voltage limit applied (a 20 V cable class permits the 21 V
// PPS ceiling, as cable_limit_mv does for Pps): what the Tests-list line and the planner both use.
inline unsigned plan_total_capped(const pd::SourceCaps& caps, unsigned monitor_cap_mv, unsigned cable_mv,
                                  Kind kind = Kind::Pps, Depth depth = Depth::Step100) {
    const unsigned cl = kind == Kind::Pps && cable_mv == 20000 ? 21000 : cable_mv;   // SPR PPS permits a 21 V request on a 20 V cable
    return plan_total(caps, std::min(monitor_cap_mv, cl), kind, depth);
}

// The summary of one APDO's recorded points. err[i] = measured - setpoint of point i (setpoint = lo_mv + i*kStepMv),
// so the measured level of point i is err[i] + lo_mv + i*kStepMv. A kHole cell was not measured and is skipped.
struct Summary {
    int16_t min_err = 0, max_err = 0, worst_err = 0;   // mV, over the recorded points
    uint16_t worst_mv = 0;    // the setpoint of the worst |err| point (0 = none recorded)
    uint16_t points = 0;      // points recorded
    uint16_t peak_mv = 0;     // the highest delivered VBUS (0 = none)
    uint16_t peak_at_mv = 0;  // the setpoint it was delivered at
    uint16_t fold_mv = 0;     // 0 = no fold (pd::track_step); else the lowest level from that point on
    uint16_t fold_from_mv = 0;   // the setpoint of the first point the rule names ("folds back ... from")
    uint16_t fold_ok_mv = 0;     // the last setpoint that still tracked (0 = the one a cell before fold_from_mv); a located fold's is exact
    bool fold_located = false;   // fold_ok_mv / fold_from_mv were located by Refiner (View::apdo keeps them through the merge)
    // The worst |err| over the points BEFORE the first fold's onset (all points when there is no fold): how well the output
    // tracked until it folded. The points after the onset are the fold, not tracking, so the Results chip grades a caution
    // by this and a fold of any depth stays amber (user decision 2026-11-16). worst_err above stays the whole record's.
    uint16_t track_err_mv = 0;
    bool out_of_band = false;    // some recorded point sat outside the row's band (steady, still recorded)
};
Summary summarize(std::span<const int16_t> err, unsigned lo_mv);
// Two segments in setpoint order, each from its own first setpoint: the low part of a split sweep, then the part from 5 V up.
Summary summarize(std::span<const int16_t> a, unsigned a_lo_mv, std::span<const int16_t> b, unsigned b_lo_mv);

// A recorded point sat outside the PPS band: VBUS below the setpoint by more than max(100 mV, 5 %). The band is
// one-sided (above the setpoint is the ceiling's business, not the point's); the summary and the live line share it.
bool out_of_band(unsigned set_mv, int measured_mv);

// How the Running view grades the live error (reading - setpoint) of a sweep row: green up to kErrOkMv, amber up to kErrCautionMv, red
// above. kErrOkMv is the Results chip's pass window (result_layout::kSweepPassMv; a static_assert in the view keeps them one number).
inline constexpr unsigned kErrOkMv = 100;
inline constexpr unsigned kErrCautionMv = 300;

// The sweep row in flight, as the Running view reads it (Runner::sweep_live): the planned point range, the setpoint of the
// Request that is out, and the last recorded point. Not a record: the view keeps nothing of its own.
struct Live {
    bool active = false;        // a sweep row is the step under test
    uint16_t lo_mv = 0, hi_mv = 0;   // the row's point range (setpoints, the planner's clamp applied)
    uint16_t set_mv = 0;        // the setpoint whose Request is out
    uint16_t n = 0;             // points measured so far
    uint16_t last_mv = 0;       // the setpoint of the last measured point
    int16_t last_err = 0;       // measured - setpoint of that point (valid when n > 0)
    unsigned last_set_mv() const { return n ? last_mv : 0u; }
    int last_measured_mv() const { return n ? int(last_set_mv()) + last_err : 0; }
};

// The per-point line of the Running view: "req 12.30V  got 12.15V  err-150mV" (volts to 10 mV, the error in whole mV with
// its sign - an error, not a flow direction; clamped to +-9999). At most kPointTextMax - 1 chars. Returns the length.
inline constexpr size_t kPointTextMax = 40;
size_t format_point(unsigned set_mv, int measured_mv, std::span<char> out);

// A finding between two points (a fold or a break at a coarse step) located to the PPS quantum: re-request the last point that
// tracked, then step up at 100 mV and again at 20 mV (PPS; AVS stops at its 100 mV granularity) until the finding sits between
// two neighbours. Pure and stepwise - the caller makes each Request next() names and feeds the settled level back; the Runner
// does that once per APDO range (bounded: ~10 Requests). Done with found = false when the re-requested point no longer tracks
// (the output latched: nothing to locate) or the first finding was not a fall.
class Refiner {
public:
    struct Located {
        bool found K2_BSS_INIT(false);
        pd::Track kind K2_BSS_INIT(pd::Track::None);
        uint16_t ok_mv K2_BSS_INIT(0), ok_level_mv K2_BSS_INIT(0);     // the last request that tracked and what it settled to
        uint16_t bad_mv K2_BSS_INIT(0), bad_level_mv K2_BSS_INIT(0);   // the first request the rule names and what it settled to
    };
    // good = the last measured point before the finding, bad = the point the rule named (request, settled level).
    void begin(Kind k, pd::Track kind, int good_req, int good_level, int bad_req, int bad_level) {
        *this = Refiner{};
        floor_ = k == Kind::Pps ? kFineMv : kStepMv;
        res_.kind = kind;
        a_ = good_req; a_lv_ = good_level; b_ = bad_req; b_lv_ = bad_level;
        if (unsigned(b_ - a_) <= floor_) { finish(); return; }
        f_ = floor_ < kStepMv && unsigned(b_ - a_) > kStepMv ? kStepMv : floor_;
        phase_ = Phase::Recheck; cur_ = a_;
    }
    bool done() const { return phase_ == Phase::Done; }
    unsigned next() const { return phase_ == Phase::Done ? 0u : unsigned(cur_); }   // the Request to make now (0 = none)
    void feed(int level_mv) {
        if (phase_ == Phase::Recheck) {
            if (level_mv - a_lv_ > pd::kTrackFoldMv || a_lv_ - level_mv > pd::kTrackFoldMv) { phase_ = Phase::Done; return; }   // latched
            scan_.clear(); scan_.feed(cur_, level_mv);
            prev_ = cur_; prev_lv_ = level_mv; phase_ = Phase::Sub;
            cur_ += int(f_); step_on();
        } else if (phase_ == Phase::Sub) {
            scan_.feed(cur_, level_mv);
            if (scan_.found()) { hit(cur_, level_mv); return; }
            prev_ = cur_; prev_lv_ = level_mv;
            cur_ += int(f_); step_on();
        }
    }
    const Located& result() const { return res_; }
private:
    enum class Phase : uint8_t { Idle, Recheck, Sub, Done };
    // cur_ is the next interior point; at or past b_ the bad request's known level closes the pass.
    void step_on() {
        if (cur_ < b_) return;
        scan_.feed(b_, b_lv_);
        if (scan_.found()) hit(b_, b_lv_);
        else { a_ = prev_; a_lv_ = prev_lv_; finish(); }
    }
    void hit(int bad, int bad_lv) {   // this pass named `bad`: its predecessor in the pass tracked
        a_ = prev_; a_lv_ = prev_lv_; b_ = bad; b_lv_ = bad_lv; res_.kind = scan_.kind();
        if (f_ > floor_) { f_ = floor_; phase_ = Phase::Recheck; cur_ = a_; return; }   // again at the finer step, from the good point
        finish();
    }
    void finish() {
        phase_ = Phase::Done;
        res_.found = true;
        res_.ok_mv = uint16_t(a_); res_.ok_level_mv = uint16_t(a_lv_); res_.bad_mv = uint16_t(b_); res_.bad_level_mv = uint16_t(b_lv_);
    }
    pd::TrackScan scan_;
    Located res_;
    Phase phase_ K2_BSS_INIT(Phase::Idle);
    unsigned floor_ = kFineMv, f_ = kFineMv;
    int a_ K2_BSS_INIT(0), a_lv_ K2_BSS_INIT(0), b_ K2_BSS_INIT(0), b_lv_ K2_BSS_INIT(0), cur_ K2_BSS_INIT(0), prev_ K2_BSS_INIT(0), prev_lv_ K2_BSS_INIT(0);
};

// One finished (or in-flight) sweep row: which APDO, where its points sit in the record, and the summary so far.
struct RowView {
    uint8_t pos = 0;        // PDO slot of the APDO
    uint8_t alg = 0;        // ct::Algorithm (Pps today, Avs when the AVS sweep lands)
    uint16_t lo_mv = 0;     // setpoint of point 0
    uint16_t hi_mv = 0;     // last setpoint of the row's planned range (the strip spans lo..hi)
    uint16_t first = 0;     // index of point 0 in the point array
    uint16_t n = 0;         // points recorded
    Summary sum{};          // over the n recorded points (filled when the row closes; live while it runs)
};
// The view the results tab and the card read: the finished rows plus the whole point array (errors, mV).
struct View {
    std::span<const RowView> rows{};
    std::span<const int16_t> points{};
    // One part of an APDO's sweep (low = the part below kSplitMv).
    const RowView* find(uint8_t alg, uint8_t pos, bool low) const {
        for (const auto& r : rows) if (r.alg == alg && r.pos == pos && (r.lo_mv < kSplitMv) == low) return &r;
        return nullptr;
    }
    // The APDO's whole sweep, both parts in one row: lo..hi over both, n = both parts' points, the summary over both in
    // setpoint order (what the card keeps per range and the results strip shows per APDO). `first` is the high part's (the
    // parts are recorded apart, so the points are not one slice). nullopt when no part has closed.
    std::optional<RowView> apdo(uint8_t alg, uint8_t pos) const;
};
// "peak 19.75 V at 19.9 V, folds back to 19.42 V from 20.0 V" / "peak 21.0 V at 21.0 V" (no fold) / "no points" - mV printed at
// 10 mV resolution with one or two decimals (19.9, 20.0, 19.75). At most 63 chars.
// Returns the length written.
size_t format_summary(const Summary&, std::span<char> out);
// One mV value in that shape: 19748 -> "19.75", 19900 -> "19.9", 20000 -> "20.0".
size_t format_mv(unsigned mv, std::span<char> out);
// The fold note of a caution (user decision 2026-11-16, the same words on the K2, the card, the summary line and the web): "folds to
// 19.4V above 19.9V" - the level the output folds to, and the last setpoint that still tracked (the setpoint before the first the
// rule names), both rounded to 0.1 V, the unit attached (K2 copy rules). Empty (0) when the summary has no fold. Lower-case, at most
// kFoldTextMax - 1 = 31 chars: one row note, never truncated.
inline constexpr size_t kFoldTextMax = 32;
size_t format_fold(const Summary&, std::span<char> out);

}  // namespace k2::ctsweep
