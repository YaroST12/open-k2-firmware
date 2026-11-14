// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check, the result: the checks with their spec limits and weights, the judging rule, the per-check aggregate, the per-item
// records and the byte blob the remote reads (pure, host-tested in tests/test_pdspec_result.cpp and through the engine tests).
//
// A check is judged per measurement and aggregated: n measured, fails, the worst value with the limit it was judged against and the
// item it came from. Verdict: Skip when nothing was measured (with the reason), Fail when any measurement failed, else Pass; a check
// whose limit is inferred (no figure in the specification) is Unverified once measured: reported, never judged, no penalty.
// Score = sum of weight x fails over the judged checks: a penalty, 0 = ideal. The weights are provisional (docs/PD_SPEC_CHECK.md "Score").
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/pd_spec_timing.hpp"

namespace k2::pdspec {

enum class Check : std::uint8_t {
    AcceptTime,     // Request -> Accept, tSenderResponse (+ the wire)
    PsRdyTime,      // Accept -> PS_RDY, tSrcTransReq
    SrcStart,       // Accept -> VBUS starts to move, not before tSrcTransition (min)
    PsRdyEarly,     // a down-step's PS_RDY with VBUS still above the new level's vSrcNew
    PsRdyLow,       // an up-step's PS_RDY with VBUS still below the new level's vSrcNew
    RiseTime,       // up-step: Accept -> VBUS inside vSrcNew, tSrcTransition + tSrcSettle
    FallTime,       // down-step: the same limit (an unloaded source must discharge by itself)
    Overshoot,      // VBUS beyond vSrcNew + vSrcValid during or after the step
    Accuracy,       // the settled fixed level inside vSrcNew
    PpsMonotonic,   // each 20 mV PPS step moves VBUS the right way (or not at all)
    PpsAccuracy,    // each PPS level inside vPpsNew
    PpsSettle,      // a 20 mV PPS step settled within tPpsSrcTransSmall
    PpsKeepAlive,   // silence in a PPS contract: the source Hard Resets after tPPSTimeout
    EprKeepAlive,   // silence in EPR mode: the source Hard Resets after tSourceEPRKeepAlive
    HrSafe0V,       // K2 Hard Reset: VBUS under vSafe0V within tPSHardReset + tSafe0V
    HrRecover,      // VBUS kept off for tSrcRecover
    HrTurnOn,       // back at vSafe5V within tSrcTurnOn
    HrCaps,         // the first Source_Capabilities within tFirstSourceCap of vSafe5V
    DetachSafe0V,   // Rd removed: VBUS under vSafe0V within tVBUSOFF (+ the source's debounce)
    Reattach,       // Rd back: a 5 V contract again
    SourceReset,    // a Hard Reset from the source where none was due
    Count
};
inline constexpr std::size_t kChecks = static_cast<std::size_t>(Check::Count);

enum class Kind : std::uint8_t { Max, Min, Range };
// A Range limit with one open side (an overshoot is bounded only on the side the step moves to) carries this as the open bound.
inline constexpr std::int32_t kOpenLimit = 1000000;
enum class Unit : std::uint8_t { Us, Ms, Mv, Count };

struct CheckDef {
    const char* key;        // report / JSON name
    const char* label;      // on the K2 (<= 13 characters, the result grid's cell)
    Kind kind;
    Unit unit;
    std::uint8_t weight;    // penalty points per failed measurement (provisional)
    bool inferred = false;  // the limit is a K2 figure, not the specification's: the check is reported as Unverified, no penalty
};

inline constexpr std::array<CheckDef, kChecks> kDefs{{
    {"accept_time", "Accept", Kind::Max, Unit::Us, 1},
    {"ps_rdy_time", "PS_RDY time", Kind::Max, Unit::Us, 2},
    {"src_start", "VBUS start", Kind::Min, Unit::Us, 1},
    {"ps_rdy_early", "PS_RDY early", Kind::Max, Unit::Mv, 3},
    {"ps_rdy_low", "PS_RDY low", Kind::Min, Unit::Mv, 3},
    {"rise_time", "Rise time", Kind::Max, Unit::Us, 2},
    {"fall_time", "Fall time", Kind::Max, Unit::Us, 2},
    {"overshoot", "Overshoot", Kind::Range, Unit::Mv, 3},
    {"accuracy", "Accuracy", Kind::Range, Unit::Mv, 3},
    {"pps_monotonic", "PPS monotonic", Kind::Min, Unit::Mv, 3},
    {"pps_accuracy", "PPS accuracy", Kind::Range, Unit::Mv, 2},
    {"pps_settle", "PPS settle", Kind::Max, Unit::Us, 1, true},          // tPpsSrcTransSmall from the Accept: inferred
    {"pps_keepalive", "PPS timeout", Kind::Range, Unit::Ms, 5},
    {"epr_keepalive", "EPR timeout", Kind::Range, Unit::Ms, 5},
    {"hr_safe0v", "HR to 0 V", Kind::Max, Unit::Ms, 5},
    {"hr_recover", "HR off time", Kind::Range, Unit::Ms, 3},
    {"hr_turn_on", "HR turn-on", Kind::Max, Unit::Ms, 3},
    {"hr_caps", "HR caps", Kind::Max, Unit::Ms, 2},
    {"detach_safe0v", "Detach to 0 V", Kind::Max, Unit::Ms, 5, true},   // tVBUSOFF + tPDDebounce, both inferred
    {"reattach", "Re-attach", Kind::Max, Unit::Ms, 10, true},           // 5 s, a K2 figure
    {"source_reset", "Source reset", Kind::Max, Unit::Count, 20},
}};

constexpr const CheckDef& def(Check c) { return kDefs[static_cast<std::size_t>(c)]; }

// Why a check measured nothing.
enum class Skip : std::uint8_t {
    None, NotOffered, OneLevel, NoPps, NoEpr, NoEprProof, NoCapture, Refused, Stopped, NotReached, SelfPowered, NoData, NotSelected
};
constexpr const char* skip_name(Skip s) {
    switch (s) {
        case Skip::None: return "-";
        case Skip::NotOffered: return "not offered";
        case Skip::OneLevel: return "one fixed level";
        case Skip::NoPps: return "no PPS";
        case Skip::NoEpr: return "no EPR";
        case Skip::NoEprProof: return "no EPR cable proof";
        case Skip::NoCapture: return "capture busy";
        case Skip::Refused: return "level refused";
        case Skip::Stopped: return "run stopped";
        case Skip::NotReached: return "not reached";
        case Skip::SelfPowered: return "K2 on the charger";
        case Skip::NoData: return "no data";
        case Skip::NotSelected: return "not selected";
    }
    return "?";
}

enum class Verdict : std::uint8_t { Pending, Pass, Fail, Skip, Unverified };
constexpr const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Pending: return "pending";
        case Verdict::Pass: return "pass";
        case Verdict::Fail: return "fail";
        case Verdict::Skip: return "skip";
        case Verdict::Unverified: return "unverified";
    }
    return "?";
}

// One measurement: value with its uncertainty (a time measured between two samples: the event lies in (value - gap, value]).
// lo / hi bound the passing range (Max uses hi, Min uses lo, Range both). Missed only when the whole bracket is outside.
struct Meas {
    std::int32_t value = 0;
    std::int32_t gap = 0;
    std::int32_t lo = 0, hi = 0;
    std::uint8_t a = 0, b = 0;   // the item: from / to position (transitions), APDO position / point index (PPS)
};

// How far outside the passing range (0 = inside, counting the bracket in the measurement's favour).
constexpr std::int32_t excess(Kind k, const Meas& m) {
    std::int32_t over = 0;
    if (k != Kind::Min && m.value - m.gap > m.hi) over = m.value - m.gap - m.hi;
    if (k != Kind::Max && m.value < m.lo) { const std::int32_t under = m.lo - m.value; if (under > over) over = under; }
    return over;
}
constexpr bool passes(Kind k, const Meas& m) { return excess(k, m) == 0; }

struct Agg {
    std::uint16_t n = 0, fails = 0;
    std::int32_t worst = 0;          // the value of the worst measurement (largest excess; the largest margin-use while all pass)
    std::int32_t lo = 0, hi = 0;     // the limits it was judged against
    std::int32_t worst_excess = -1;  // -1 = nothing measured
    std::uint8_t a = 0, b = 0;
    Skip skip = Skip::None;

    void add(Kind k, const Meas& m) {
        ++n;
        const std::int32_t ex = excess(k, m);
        if (ex > 0) ++fails;
        // Among passing measurements the worst is the one closest to its limit.
        const std::int32_t rank = ex > 0 ? ex + (1 << 24) : closeness(k, m);
        if (rank > worst_excess) { worst_excess = rank; worst = m.value; lo = m.lo; hi = m.hi; a = m.a; b = m.b; }
    }
    static constexpr std::int32_t closeness(Kind k, const Meas& m) {
        // Larger = closer to a limit; kept below 2^24 so a failure always ranks higher.
        std::int32_t c = 0;
        if (k != Kind::Min) c = m.value - m.hi + (1 << 23);
        if (k != Kind::Max) { const std::int32_t d = m.lo - m.value + (1 << 23); if (d > c) c = d; }
        return c < 0 ? 0 : c >= (1 << 24) ? (1 << 24) - 1 : c;
    }
    Verdict verdict() const { return n == 0 ? Verdict::Skip : fails ? Verdict::Fail : Verdict::Pass; }   // judged as if sourced
};

// The verdict the result shows: an inferred limit never passes or fails a charger (the count over it stays in fails).
constexpr Verdict verdict(Check c, const Agg& a) {
    const Verdict v = a.verdict();
    return def(c).inferred && v != Verdict::Skip ? Verdict::Unverified : v;
}

// ---- per-item records (the report's detail) ------------------------------------------------------------------------------------

// One fixed transition of the walk. Times in units of 0.1 ms (Accept) or ms; 0xFFFF = not seen.
struct TransRec {
    std::uint8_t from = 0, to = 0;       // PDO positions
    std::uint8_t flags = 0;              // kTr* below
    std::uint8_t pad = 0;
    std::uint16_t accept_100us = 0xFFFF; // Request -> Accept
    std::uint16_t psrdy_ms = 0xFFFF;     // Accept -> PS_RDY
    std::uint16_t start_ms = 0xFFFF;     // Accept -> VBUS starts to move
    std::uint16_t band_ms = 0xFFFF;      // Accept -> VBUS inside vSrcNew
    std::uint16_t psrdy_mv = 0;          // VBUS at PS_RDY (0 = unknown)
    std::uint16_t peak_mv = 0;           // highest (up) / lowest (down) VBUS after the Accept
    std::uint16_t final_mv = 0;          // settled level (0 = not settled in the capture)
    std::uint16_t gap_ms = 0;            // the largest sample gap of the capture
};
static_assert(sizeof(TransRec) == 20);
inline constexpr std::uint8_t kTrEpr = 1, kTrLiveBand = 2, kTrRefused = 4, kTrNoCapture = 8, kTrSuspectStamp = 16;

// One PPS set point: kPpsSteps steps up and as many down.
struct PpsRec {
    std::uint8_t pos = 0;                // APDO position
    std::uint8_t steps = 0;              // steps measured
    std::uint8_t mono_fails = 0, acc_fails = 0;
    std::uint16_t set_mv = 0;            // the set point
    std::int16_t worst_err_mv = 0;       // the largest |measured - requested|, signed
    std::int16_t min_delta_mv = 0, max_delta_mv = 0;   // the smallest / largest step-to-step change (signed in the step's direction)
    std::uint16_t max_settle_100us = 0;  // the slowest step's settle time
};
static_assert(sizeof(PpsRec) == 14);

// The timer checks' raw numbers (ms; 0xFFFFFFFF = not seen).
struct Timers {
    std::uint32_t pps_ka_ms = 0xFFFFFFFF;    // the PPS Request -> the source's Hard Reset
    std::uint32_t epr_ka_ms = 0xFFFFFFFF;    // keep-alives silenced -> the source's Hard Reset
    std::uint16_t hr_from_mv = 0;            // the level the K2 Hard Reset was sent from
    std::uint16_t hr_off_ms = 0xFFFF, hr_dark_ms = 0xFFFF, hr_on_ms = 0xFFFF, hr_caps_ms = 0xFFFF;
    std::uint16_t det_from_mv = 0;
    std::uint16_t det_off_ms = 0xFFFF, det_back_ms = 0xFFFF;
    std::uint16_t poll_gap_ms = 0;           // the largest poll spacing of the live timer measurements
};

// ---- the blob ----------------------------------------------------------------------------------------------------------------------
// Little endian. Header (16): magic "PSC1" | state u8 | stop u8 | n_trans u8 | n_pps u8 | score u16 | elapsed_s u16 | flags u8 | pad 3.
// Then kChecks x 20 (verdict u8, skip u8, a u8, b u8, n u16, fails u16, worst i32, lo i32, hi i32), Timers (32), n_trans x TransRec (20),
// n_pps x PpsRec (14).
inline constexpr std::size_t kBlobHeader = 16, kAggBytes = 20, kTimerBytes = 32;
inline constexpr std::size_t kMaxTrans = 60;   // the walk of 8 nodes (56) plus room
inline constexpr std::size_t kMaxPps = 12;     // kMaxApdos x kPointsPerApdo
inline constexpr std::size_t kMaxBlob = kBlobHeader + kChecks * kAggBytes + kTimerBytes + kMaxTrans * sizeof(TransRec) + kMaxPps * sizeof(PpsRec);

struct BlobHead {
    std::uint8_t state = 0, stop = 0, n_trans = 0, n_pps = 0;
    std::uint16_t score = 0, elapsed_s = 0;
    std::uint8_t flags = 0;
};

constexpr std::size_t blob_len(std::size_t n_trans, std::size_t n_pps) {
    return kBlobHeader + kChecks * kAggBytes + kTimerBytes + n_trans * sizeof(TransRec) + n_pps * sizeof(PpsRec);
}

// Writes the byte at blob offset `off` (a streaming reader: the K2 never holds the whole blob, the remote reads it in chunks).
class BlobReader {
public:
    BlobReader(const BlobHead& h, std::span<const Agg> agg, const Timers& t, std::span<const TransRec> tr, std::span<const PpsRec> pp)
        : h_(h), agg_(agg), t_(t), tr_(tr), pp_(pp) {}
    std::size_t size() const { return blob_len(tr_.size(), pp_.size()); }
    std::size_t read(std::size_t off, std::span<std::uint8_t> out) const {
        std::size_t n = 0;
        for (; n < out.size() && off + n < size(); ++n) out[n] = at(off + n);
        return n;
    }

private:
    static std::uint8_t le(std::uint32_t v, std::size_t i) { return std::uint8_t(v >> (8 * i)); }
    std::uint8_t at(std::size_t o) const {
        if (o < kBlobHeader) {
            switch (o) {
                case 0: return 'P'; case 1: return 'S'; case 2: return 'C'; case 3: return '1';
                case 4: return h_.state; case 5: return h_.stop; case 6: return h_.n_trans; case 7: return h_.n_pps;
                case 8: case 9: return le(h_.score, o - 8);
                case 10: case 11: return le(h_.elapsed_s, o - 10);
                case 12: return h_.flags;
                default: return 0;
            }
        }
        o -= kBlobHeader;
        if (o < kChecks * kAggBytes) {
            const std::size_t k = o / kAggBytes, i = o % kAggBytes;
            const Agg& a = agg_[k];
            switch (i) {
                case 0: return std::uint8_t(verdict(Check(k), a));
                case 1: return std::uint8_t(a.skip);
                case 2: return a.a;
                case 3: return a.b;
                case 4: case 5: return le(a.n, i - 4);
                case 6: case 7: return le(a.fails, i - 6);
                default: break;
            }
            if (i < 12) return le(std::uint32_t(a.worst), i - 8);
            if (i < 16) return le(std::uint32_t(a.lo), i - 12);
            return le(std::uint32_t(a.hi), i - 16);
        }
        o -= kChecks * kAggBytes;
        if (o < kTimerBytes) {
            const std::uint32_t w[8] = {t_.pps_ka_ms, t_.epr_ka_ms, std::uint32_t(t_.hr_from_mv) | std::uint32_t(t_.hr_off_ms) << 16,
                                        std::uint32_t(t_.hr_dark_ms) | std::uint32_t(t_.hr_on_ms) << 16,
                                        std::uint32_t(t_.hr_caps_ms) | std::uint32_t(t_.det_from_mv) << 16,
                                        std::uint32_t(t_.det_off_ms) | std::uint32_t(t_.det_back_ms) << 16, t_.poll_gap_ms, 0};
            return le(w[o / 4], o % 4);
        }
        o -= kTimerBytes;
        if (o < tr_.size() * sizeof(TransRec)) return bytes(tr_[o / sizeof(TransRec)], o % sizeof(TransRec));
        o -= tr_.size() * sizeof(TransRec);
        return bytes(pp_[o / sizeof(PpsRec)], o % sizeof(PpsRec));
    }
    static std::uint8_t bytes(const TransRec& r, std::size_t i) {
        const std::uint16_t w[10] = {std::uint16_t(r.from | r.to << 8), std::uint16_t(r.flags | r.pad << 8), r.accept_100us, r.psrdy_ms, r.start_ms,
                                     r.band_ms, r.psrdy_mv, r.peak_mv, r.final_mv, r.gap_ms};
        return le(w[i / 2], i % 2);
    }
    static std::uint8_t bytes(const PpsRec& r, std::size_t i) {
        const std::uint16_t w[7] = {std::uint16_t(r.pos | r.steps << 8), std::uint16_t(r.mono_fails | r.acc_fails << 8), r.set_mv,
                                    std::uint16_t(r.worst_err_mv), std::uint16_t(r.min_delta_mv), std::uint16_t(r.max_delta_mv), r.max_settle_100us};
        return le(w[i / 2], i % 2);
    }
    const BlobHead& h_;
    std::span<const Agg> agg_;
    const Timers& t_;
    std::span<const TransRec> tr_;
    std::span<const PpsRec> pp_;
};

// Score: weight x fails over the judged checks (an inferred limit adds nothing), summed (saturates at 0xFFFF).
inline std::uint16_t score(std::span<const Agg> agg) {
    std::uint32_t s = 0;
    for (std::size_t k = 0; k < agg.size() && k < kChecks; ++k)
        if (!kDefs[k].inferred) s += std::uint32_t(kDefs[k].weight) * agg[k].fails;
    return std::uint16_t(s > 0xFFFF ? 0xFFFF : s);
}

}  // namespace k2::pdspec
