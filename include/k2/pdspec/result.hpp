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
    PpsSettle,      // a 20 mV PPS step settled on its final level within tPpsSrcTransSmall (the meter's floor: unverified)
    PpsKeepAlive,   // silence in a PPS contract: the source Hard Resets after tPPSTimeout
    EprKeepAlive,   // silence in EPR mode: the source Hard Resets after tSourceEPRKeepAlive
    HrSafe0V,       // K2 Hard Reset: VBUS under vSafe0V within tPSHardReset + tSafe0V
    HrRecover,      // VBUS kept off for tSrcRecover
    HrTurnOn,       // back at vSafe5V within tSrcTurnOn
    HrCaps,         // the first Source_Capabilities within tFirstSourceCap of vSafe5V
    DetachSafe0V,   // Rd removed: VBUS under vSafe0V within tVBUSOFF (+ the source's debounce)
    Reattach,       // Rd back: a 5 V contract again
    SourceReset,    // a Hard Reset from the source where none was due
    // Appended in PSC2 (the blob keeps the order above for PSC1 readers).
    PpsBand,        // a 100 / 500 mV step or a slam: VBUS inside vPpsNew within tPpsSrcTransSmall / Large of the Accept
    PpsPsRdy,       // ... and PS_RDY within the same window
    PpsOvershoot,   // VBUS beyond vPpsNew + vPpsValid after the Accept
    PpsTransient,   // after the first entry, VBUS outside vPpsNew no longer than tPpsTransient
    PpsStatus,      // Get_PPS_Status answered with a valid PPS_Status (fault bits kPs* below; 0 = clean)
    PpsStatusTime,  // Get_PPS_Status -> PPS_Status within tReceiverResponse (+ the wire)
    AvsMonotonic,   // the AVS counterparts (SPR AVS APDO)
    AvsAccuracy,
    AvsBand,
    AvsPsRdy,
    AvsOvershoot,
    AvsTransient,
    ReattachCaps,   // Rd back: the first Source_Capabilities within tFirstSourceCap of vSafe5V
    PpsTrack,       // an upward step where VBUS falls behind the request (a fold, or a break): pd::track_behind <= track_limit
    AvsTrack,       // ... and the AVS counterpart
    Count
};
inline constexpr std::size_t kChecks = static_cast<std::size_t>(Check::Count);

enum class Kind : std::uint8_t { Max, Min, Range };
// A Range limit with one open side (an overshoot is bounded only on the side the step moves to) carries this as the open bound.
inline constexpr std::int32_t kOpenLimit = 1000000;
enum class Unit : std::uint8_t { Us, Ms, Mv, Count };
// The PSC1 blob's checks (the first 21 of the table below).
inline constexpr std::size_t kChecksPsc1 = 21;

// The key names a check in the host tools (pdspec_blob.py reads the keys from this table, host builds print them). The firmware shows
// and logs the label only, so on the K2 the key is dropped at compile time (34 strings and a pointer per check of flash).
#if defined(__arm__)
struct CheckKey {
    consteval CheckKey(const char*) {}
};
#else
using CheckKey = const char*;
#endif

struct CheckDef {
    [[no_unique_address]] CheckKey key;   // report / JSON name (host builds only)
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
    {"pps_settle", "PPS settle", Kind::Max, Unit::Us, 1, true},          // 20 mV steps: under the meter's resolution, unverified
    {"pps_keepalive", "PPS timeout", Kind::Range, Unit::Ms, 5},
    {"epr_keepalive", "EPR timeout", Kind::Range, Unit::Ms, 5},
    {"hr_safe0v", "HR to 0 V", Kind::Max, Unit::Ms, 5},
    {"hr_recover", "HR off time", Kind::Range, Unit::Ms, 3},
    {"hr_turn_on", "HR turn-on", Kind::Max, Unit::Ms, 3},
    {"hr_caps", "HR caps", Kind::Max, Unit::Ms, 2},
    {"detach_safe0v", "Detach to 0 V", Kind::Max, Unit::Ms, 5},         // tSRCDisconnect + tVBUSOFF
    {"reattach", "Re-attach", Kind::Max, Unit::Ms, 10},                 // Rd back -> vSafe5V: tCCDebounce + tBulkDischarge + tVBUSON
    {"source_reset", "Source reset", Kind::Max, Unit::Count, 20},
    {"pps_band", "PPS in band", Kind::Max, Unit::Us, 2},
    {"pps_ps_rdy", "PPS PS_RDY", Kind::Max, Unit::Us, 2},
    {"pps_overshoot", "PPS overshoot", Kind::Range, Unit::Mv, 3},
    {"pps_transient", "PPS transient", Kind::Max, Unit::Us, 2},
    {"pps_status", "PPS_Status", Kind::Max, Unit::Count, 3},
    {"pps_status_time", "PPS_Status t", Kind::Max, Unit::Us, 1},
    {"avs_monotonic", "AVS monotonic", Kind::Min, Unit::Mv, 3},
    {"avs_accuracy", "AVS accuracy", Kind::Range, Unit::Mv, 2},
    {"avs_band", "AVS in band", Kind::Max, Unit::Us, 2},
    {"avs_ps_rdy", "AVS PS_RDY", Kind::Max, Unit::Us, 2},
    {"avs_overshoot", "AVS overshoot", Kind::Range, Unit::Mv, 3},
    {"avs_transient", "AVS transient", Kind::Max, Unit::Us, 2},
    {"reattach_caps", "Reattach caps", Kind::Max, Unit::Ms, 2},
    {"pps_track", "PPS tracking", Kind::Max, Unit::Mv, 2},   // value = how far the offset grew over the step, limit min(step + 50, 150) mV
    {"avs_track", "AVS tracking", Kind::Max, Unit::Mv, 2},
}};

// PpsStatus fault bits (the check's value; 0 = a clean answer). No-load expectations of the PD CTS (SRC3.7#2).
inline constexpr std::int32_t kPsNotSupported = 1;   // Not_Supported instead of PPS_Status (Get_PPS_Status is required of a PPS source)
inline constexpr std::int32_t kPsNoAnswer = 2;       // nothing within the sink's answer window
inline constexpr std::int32_t kPsVoltage = 4;        // Output Voltage neither 0xFFFF nor within +-3 % (rounded to 20 mV) of the request
inline constexpr std::int32_t kPsCurrent = 8;        // Output Current neither 0x00..0x03 nor 0xFF at no load
inline constexpr std::int32_t kPsOmf = 16;           // OMF set: current limit mode at no load
inline constexpr std::int32_t kPsFlags = 32;         // reserved flag bits (bit 0, bits 4..7) set, or PTF = 2 / 3

// The caps note (advisory, never a verdict): PPS without AVS is allowed before PD R3.3.
enum class CapsNote : std::uint8_t { None, PpsNoAvs, PpsAvs, AvsOnly };
constexpr const char* caps_note_name(CapsNote n) {
    switch (n) {
        case CapsNote::None: return "no PPS, no AVS";
        case CapsNote::PpsNoAvs: return "PPS without AVS: allowed before PD R3.3";
        case CapsNote::PpsAvs: return "PPS + AVS present";
        case CapsNote::AvsOnly: return "AVS without PPS";
    }
    return "?";
}

// The programmable checks that belong to the AVS APDO (the rest of the Pps family is PPS).
constexpr bool avs_check(Check c) { return (c >= Check::AvsMonotonic && c <= Check::AvsTransient) || c == Check::AvsTrack; }

constexpr const CheckDef& def(Check c) { return kDefs[static_cast<std::size_t>(c)]; }

// Why a check measured nothing.
enum class Skip : std::uint8_t {
    None, NotOffered, OneLevel, NoPps, NoEpr, NoEprProof, NoCapture, Refused, Stopped, NotReached, SelfPowered, NoData, NotSelected,
    NoAvs
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
        case Skip::NoAvs: return "no AVS";
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
    std::uint8_t a = 0, b = 0;
    Skip skip = Skip::None;
    bool worst_failed = false;       // the kept measurement failed (its rank is redone from worst/lo/hi: 20 bytes, RAM)

    void add(Kind k, const Meas& m) {
        const std::int32_t ex = excess(k, m);
        if (ex > 0) ++fails;
        // Among passing measurements the worst is the one closest to its limit. The kept one is ranked again from its stored value
        // without its sample bracket (not kept): a kept failure can only rank higher by less than its bracket, a kept pass ranks exactly.
        if (n++ == 0 || rank(k, m, ex > 0) > rank(k, {worst, 0, lo, hi}, worst_failed)) {
            worst = m.value; lo = m.lo; hi = m.hi; a = m.a; b = m.b; worst_failed = ex > 0;
        }
    }
    static std::int32_t rank(Kind k, const Meas& m, bool failed) {
        if (!failed) return closeness(k, m);
        const std::int32_t ex = excess(k, m);
        return (ex > 0 ? ex : 1) + (1 << 24);
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
static_assert(sizeof(Agg) == 20, "36 of these live in RAM for the whole run");

// The verdict the result shows: an inferred limit never passes or fails a charger (the count over it stays in fails).
constexpr Verdict verdict(Check c, const Agg& a) {
    const Verdict v = a.verdict();
    return def(c).inferred && v != Verdict::Skip ? Verdict::Unverified : v;
}

// ---- per-item records (the report's detail) ------------------------------------------------------------------------------------

// One fixed transition of the walk. Times in units of 0.1 ms (Accept) or ms; 0xFFFF = not seen.
// In RAM 18 bytes (the record pool, engine.hpp); on the wire 20 (kTransBytes: flags, a zero byte, ..., gap_ms as a u16 last).
struct TransRec {
    std::uint8_t from = 0, to = 0;       // PDO positions
    std::uint8_t flags = 0;              // kTr* below
    std::uint8_t gap_ms = 0;             // the largest sample gap of the capture (saturates at 255)
    std::uint16_t accept_100us = 0xFFFF; // Request -> Accept
    std::uint16_t psrdy_ms = 0xFFFF;     // Accept -> PS_RDY
    std::uint16_t start_ms = 0xFFFF;     // Accept -> VBUS starts to move
    std::uint16_t band_ms = 0xFFFF;      // Accept -> VBUS inside vSrcNew
    std::uint16_t psrdy_mv = 0;          // VBUS at PS_RDY (0 = unknown)
    std::uint16_t peak_mv = 0;           // highest (up) / lowest (down) VBUS after the Accept
    std::uint16_t final_mv = 0;          // settled level (0 = not settled in the capture)
};
static_assert(sizeof(TransRec) == 18);
inline constexpr std::size_t kTransBytes = 20;   // a TransRec on the wire
inline constexpr std::uint8_t kTrEpr = 1, kTrLiveBand = 2, kTrRefused = 4, kTrNoCapture = 8, kTrSuspectStamp = 16;

// One pass of the programmable sweep: one window of an APDO (plan.hpp Prog) at one step class (20 / 100 / 500 mV for PPS, 100 / 500 /
// 1000 mV for AVS, or the two slams), up and back down. Times in units of 0.1 ms, saturating at 0xFFFE; 0xFFFF = never.
struct PassRec {
    std::uint8_t pos = 0;                // APDO position
    std::uint8_t cls = 0;                // step class 0..3 (3 = slam), bit 7 = AVS
    std::uint16_t steps = 0;             // moves measured (a full 20 mV sweep makes hundreds)
    std::uint16_t lo_mv = 0, hi_mv = 0;  // the window swept
    std::uint16_t band_100us = 0;        // the slowest Accept -> inside vPpsNew / vAvsNew (classes 1..3), or the slowest settle on the
                                         // final level (class 0, 20 mV)
    std::uint16_t psrdy_100us = 0;       // the slowest Accept -> PS_RDY
    std::int16_t worst_err_mv = 0;       // the largest |final - requested|, signed
    std::int16_t min_delta_mv = 0;       // the smallest step-to-step change, signed in the step's direction (monotonic)
    std::int16_t over_mv = 0;            // the largest excursion beyond vPpsNew / vAvsNew after the Accept (<= 0: none)
    std::uint16_t out_100us = 0;         // the longest stretch outside the band after the first entry
    std::uint8_t band_fails = 0, psrdy_fails = 0, mono_fails = 0, acc_fails = 0, over_fails = 0, out_fails = 0;   // saturating
    std::uint8_t status = 0;             // the PpsStatus fault bits of the pass's Get_PPS_Status asks, or-ed (PPS only)
    std::uint8_t track = 0;              // 1 = a fold / track break was named in this pass (pd/prog_track.hpp); only the first is judged
};
static_assert(sizeof(PassRec) == 28);
inline constexpr std::size_t kPassBytes = sizeof(PassRec);   // the same on the wire
constexpr void bump_sat(std::uint8_t& n) { if (n < 0xFF) ++n; }
inline constexpr std::uint8_t kPassAvs = 0x80;

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
// Little endian. Header (16): magic "PSC2" | state u8 | stop u8 | n_trans u8 | n_pass u8 | score u16 | elapsed_s u16 | flags u8 |
// n_checks u8 | caps_note u8 | opts u8. Then n_checks x 20 (verdict u8, skip u8, a u8, b u8, n u16, fails u16, worst i32, lo i32, hi i32),
// Timers (32), n_trans x TransRec (20), n_pass x PassRec (28). (PSC1, before the programmable sweep: 21 checks, bytes 13..15 zero and
// 14-byte PPS point records; tools/pdspec_blob.py reads both.)
// The "where" of a programmable check's worst value (Agg a / b): a = APDO position | class << 4 | down << 7, b = the target / 100 mV.
inline constexpr std::size_t kBlobHeader = 16, kAggBytes = 20, kTimerBytes = 32;
inline constexpr std::size_t kMaxTrans = 60;   // the walk of 8 nodes (56) plus room
inline constexpr std::size_t kMaxPass = 16;    // kMaxProg windows x kClasses
inline constexpr std::size_t kMaxBlob = kBlobHeader + kChecks * kAggBytes + kTimerBytes + kMaxTrans * kTransBytes + kMaxPass * kPassBytes;
inline constexpr std::uint8_t kOptFull20 = 1;  // opts: the 20 mV class swept the whole window (else three 10-step windows)

struct BlobHead {
    std::uint8_t state = 0, stop = 0, n_trans = 0, n_pass = 0;
    std::uint16_t score = 0, elapsed_s = 0;
    std::uint8_t flags = 0;
    CapsNote caps_note = CapsNote::None;
    std::uint8_t opts = 0;
};

constexpr std::size_t blob_len(std::size_t n_trans, std::size_t n_pass) {
    return kBlobHeader + kChecks * kAggBytes + kTimerBytes + n_trans * kTransBytes + n_pass * kPassBytes;
}

// Writes the byte at blob offset `off` (a streaming reader: the K2 never holds the whole blob, the remote reads it in chunks).
class BlobReader {
public:
    BlobReader(const BlobHead& h, std::span<const Agg> agg, const Timers& t, std::span<const TransRec> tr, std::span<const PassRec> pp)
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
                case 0: return 'P'; case 1: return 'S'; case 2: return 'C'; case 3: return '2';
                case 4: return h_.state; case 5: return h_.stop; case 6: return h_.n_trans; case 7: return h_.n_pass;
                case 8: case 9: return le(h_.score, o - 8);
                case 10: case 11: return le(h_.elapsed_s, o - 10);
                case 12: return h_.flags;
                case 13: return std::uint8_t(kChecks);
                case 14: return std::uint8_t(h_.caps_note);
                default: return h_.opts;
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
        if (o < tr_.size() * kTransBytes) return bytes(tr_[o / kTransBytes], o % kTransBytes);
        o -= tr_.size() * kTransBytes;
        return bytes(pp_[o / kPassBytes], o % kPassBytes);
    }
    static std::uint8_t bytes(const TransRec& r, std::size_t i) {
        const std::uint16_t w[10] = {std::uint16_t(r.from | r.to << 8), r.flags, r.accept_100us, r.psrdy_ms, r.start_ms,
                                     r.band_ms, r.psrdy_mv, r.peak_mv, r.final_mv, r.gap_ms};
        return le(w[i / 2], i % 2);
    }
    static std::uint8_t bytes(const PassRec& r, std::size_t i) {
        const std::uint16_t w[14] = {std::uint16_t(r.pos | r.cls << 8), r.steps, r.lo_mv, r.hi_mv, r.band_100us, r.psrdy_100us,
                                     std::uint16_t(r.worst_err_mv), std::uint16_t(r.min_delta_mv), std::uint16_t(r.over_mv), r.out_100us,
                                     std::uint16_t(r.band_fails | r.psrdy_fails << 8), std::uint16_t(r.mono_fails | r.acc_fails << 8),
                                     std::uint16_t(r.over_fails | r.out_fails << 8), std::uint16_t(r.status | r.track << 8)};
        return le(w[i / 2], i % 2);
    }
    const BlobHead& h_;
    std::span<const Agg> agg_;
    const Timers& t_;
    std::span<const TransRec> tr_;
    std::span<const PassRec> pp_;
};

// Score: weight x fails over the judged checks (an inferred limit adds nothing), summed (saturates at 0xFFFF).
inline std::uint16_t score(std::span<const Agg> agg) {
    std::uint32_t s = 0;
    for (std::size_t k = 0; k < agg.size() && k < kChecks; ++k)
        if (!kDefs[k].inferred) s += std::uint32_t(kDefs[k].weight) * agg[k].fails;
    return std::uint16_t(s > 0xFFFF ? 0xFFFF : s);
}

}  // namespace k2::pdspec
