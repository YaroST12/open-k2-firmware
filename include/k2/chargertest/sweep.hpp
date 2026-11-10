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
//   at, and a fold-back (VBUS falling while the request rises) - the FH22 shape: tracks -150 mV to 19.75 V, then every
//   request >= 20.0 V folds back to 19.42 V.
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include "k2/pd/pd.hpp"

namespace k2::ctsweep {

// A point plan per range kind; the AVS sweep (100 mV is the AVS Request granularity) reuses the PPS one.
enum class Kind : uint8_t { Pps, Avs };
inline constexpr unsigned kStepMv = 100;      // every 100 mV across the range (the PPS quantum is 20 mV)
inline constexpr unsigned kSweepMa = 1000;    // 1 A per point, clamped by point_ma() to the offer and the cable
// All APDOs of one plan together: the record is one int16 error per point, kMaxPoints * 2 B, no heap. A plan that
// would exceed it sweeps fewer APDOs (plan_total counts the same budget), so the Tests-list count and the plan agree.
inline constexpr unsigned kMaxPoints = 640;
inline constexpr unsigned kMaxRows = 8;       // one result row per APDO; the card keeps at most 4 ranges

// The point range of one advertised APDO range [apdo_min_mv, apdo_max_mv], capped at cap_mv, on kStepMv boundaries.
// lo > hi (empty) when nothing fits. lo/hi are setpoints in mV.
struct PointRange { unsigned lo = 0, hi = 0; bool empty() const { return hi < lo; } };
constexpr PointRange point_range(unsigned apdo_min_mv, unsigned apdo_max_mv, unsigned cap_mv) {
    const unsigned lo = (apdo_min_mv + kStepMv - 1) / kStepMv * kStepMv;
    const unsigned hi = (std::min(apdo_max_mv, cap_mv) / kStepMv) * kStepMv;
    return {lo, hi};
}
constexpr unsigned point_count(PointRange r) { return r.empty() ? 0u : (r.hi - r.lo) / kStepMv + 1u; }
// The setpoint of point i of a row that starts at r.lo.
constexpr unsigned point_mv(PointRange r, unsigned i) { return r.lo + i * kStepMv; }
// The current a point asks: 1 A, never above the APDO's current at that level nor the cable's limit.
constexpr unsigned point_ma(const pd::Pdo& p, unsigned mv, unsigned cable_ma) {
    return std::min({kSweepMa, unsigned(pd::current_limit_ma(p, uint16_t(mv))), cable_ma});
}
// The total point count of a PPS sweep over a caps list (what the Tests list line shows), within the kMaxPoints
// budget, APDOs in list order (the planner emits them in the same order). cap_mv = the effective ceiling already
// clamped by the caller to the monitor ceiling and the cable's voltage limit (cable_limit_mv).
unsigned plan_total(const pd::SourceCaps& caps, unsigned cap_mv);

// The summary of one APDO's recorded points. err[i] = measured - setpoint of point i (setpoint = lo_mv + i*kStepMv),
// so the measured level of point i is err[i] + lo_mv + i*kStepMv.
struct Summary {
    int16_t min_err = 0, max_err = 0, worst_err = 0;   // mV, over the recorded points
    uint16_t worst_mv = 0;    // the setpoint of the worst |err| point (0 = none recorded)
    uint16_t points = 0;      // points recorded
    uint16_t peak_mv = 0;     // the highest delivered VBUS (0 = none)
    uint16_t peak_at_mv = 0;  // the setpoint it was delivered at
    uint16_t fold_mv = 0;     // 0 = VBUS never fell while the request rose; else the level it folded back to
    uint16_t fold_from_mv = 0;   // the setpoint of the first falling point ("folds back ... from")
    bool out_of_band = false;    // some recorded point sat outside the row's band (steady, still recorded)
};
Summary summarize(std::span<const int16_t> err, unsigned lo_mv);

// One finished (or in-flight) sweep row: which APDO, where its points sit in the record, and the summary so far.
struct RowView {
    uint8_t pos = 0;        // PDO slot of the APDO
    uint8_t alg = 0;        // ct::Algorithm (Pps today, Avs when the AVS sweep lands)
    uint16_t lo_mv = 0;     // setpoint of point 0
    uint16_t first = 0;     // index of point 0 in the point array
    uint16_t n = 0;         // points recorded
};
// The view the results tab and the card read: the finished rows plus the whole point array (errors, mV).
struct View {
    std::span<const RowView> rows{};
    std::span<const int16_t> points{};
    const RowView* find(uint8_t alg, uint8_t pos) const {
        for (const auto& r : rows) if (r.alg == alg && r.pos == pos) return &r;
        return nullptr;
    }
};
// "peak 19.75 V at 19.9 V, folds back to 19.42 V from 20.0 V" / "peak 21.0 V at 21.0 V" (no fold-back) /
// "no points" - mV printed at 10 mV resolution with one or two decimals (19.9, 20.0, 19.75). At most 63 chars.
// Returns the length written.
size_t format_summary(const Summary&, std::span<char> out);
// One mV value in that shape: 19748 -> "19.75", 19900 -> "19.9", 20000 -> "20.0".
size_t format_mv(unsigned mv, std::span<char> out);

}  // namespace k2::ctsweep
