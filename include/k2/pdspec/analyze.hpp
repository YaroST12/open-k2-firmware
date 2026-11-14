// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check, the timing analysis: pure functions over a transition capture (any Recorder-shaped trace: the live
// k2::transcap::Recorder on the K2, a decoded transcap::View on the host) and a small tracker for the Hard Reset / detach path, fed live.
// They measure; judging against the spec limits is result.hpp's. Host-tested in tests/test_pdspec_analyze.cpp.
//
// Every time is relative to a K2 edge (the Request's TX start, the Accept's or PS_RDY's RX edge) and carries the spacing of the two
// samples that bracket it (Timed::gap_us): the event happened somewhere in (us - gap_us, us]. A limit counts as missed only when the
// whole bracket is past it (result.hpp), so a long screen paint between two samples never fails a charger.
#pragma once

#include <cstdint>
#include <cstdlib>

#include "k2/pd/pd_spec_timing.hpp"
#include "k2/pd/transcap_calc.hpp"

namespace k2::pdspec {

// The PD edges of one transition (absolute us on the capture's clock). From the capture's own markers (find_edges) or from the
// sink's edge stamps, whichever the caller trusts.
struct Edges {
    std::uint32_t req_us = 0, accept_us = 0, psrdy_us = 0;
    bool accept = false, psrdy = false, rejected = false, wait = false;
};

// An event time relative to a reference, and the spacing of the samples that bracket it.
struct Timed {
    std::uint32_t us = 0;
    std::uint32_t gap_us = 0;
    bool seen = false;
};

struct TransitionIn {
    Edges e;
    std::int32_t from_mv = 0, to_mv = 0;   // the nominal levels (fixed PDO voltages, or the PPS set points)
};

struct TransitionOut {
    Timed start;                           // Accept -> VBUS has left the old level by a tenth of the step (at least kDepartMinMv)
    Timed band;                            // Accept -> the first sample from which VBUS stays inside vSrcNew of the new level
    bool psrdy_known = false;
    std::int32_t vbus_at_psrdy_mv = 0;     // the last sample at or before the PS_RDY edge
    std::int32_t peak_mv = 0;              // after the Accept: the highest VBUS of an up-step, the lowest of a down-step
    std::int32_t final_mv = 0;             // mean of the window's last kTailUs, only when that tail is inside the band
    bool final_valid = false;
    std::uint32_t last_us = 0;             // the window's last sample, relative to the Accept
    std::int32_t last_mv = 0;
    std::uint32_t max_gap_us = 0;          // the largest sample spacing after the Accept
};

struct StepOut {
    bool valid = false;
    std::int32_t final_mv = 0;             // mean of the window's last kTailUs
    Timed settle;                          // Accept -> the first sample from which VBUS stays within kSettleTolMv of final_mv
    bool settled = false;
};

inline constexpr std::int32_t kDepartMinMv = 100;
inline constexpr std::uint32_t kPreUs = 20000;       // the old level: mean of the 20 ms before the Accept
inline constexpr std::uint32_t kTailUs = 50000;      // the final level: mean of the window's last 50 ms
inline constexpr std::int32_t kSettleTolMv = 10;     // a PPS step has settled within half a 20 mV step of its final level

constexpr std::int32_t band_lo(std::int32_t mv) { return mv - mv * std::int32_t(pd::spec::kVSrcNewPermille) / 1000; }
constexpr std::int32_t band_hi(std::int32_t mv) { return mv + mv * std::int32_t(pd::spec::kVSrcNewPermille) / 1000; }
constexpr bool in_band(std::int32_t v, std::int32_t mv) { return v >= band_lo(mv) && v <= band_hi(mv); }

namespace detail {
constexpr bool after(std::uint32_t t, std::uint32_t ref) { return std::int32_t(t - ref) >= 0; }
}  // namespace detail

// The first Request at or after `from_us`, then the first Accept / Reject / Wait after it and the first PS_RDY after that Accept.
template <class Rec>
Edges find_edges(const Rec& r, std::uint32_t from_us) {
    Edges e;
    bool req = false;
    for (std::size_t i = 0; i < r.marker_count(); ++i) {
        const transcap::Marker m = r.marker(i);
        const auto code = transcap::Mark(m.code);
        if (!req) {
            if (code == transcap::Mark::TxRequest && detail::after(m.t_us, from_us)) { req = true; e.req_us = m.t_us; }
            continue;
        }
        if (!e.accept && !e.rejected && !e.wait) {
            if (code == transcap::Mark::Accept) { e.accept = true; e.accept_us = m.t_us; }
            else if (code == transcap::Mark::Reject) e.rejected = true;
            else if (code == transcap::Mark::Wait) e.wait = true;
            continue;
        }
        if (e.accept && !e.psrdy && code == transcap::Mark::PsRdy) { e.psrdy = true; e.psrdy_us = m.t_us; }
    }
    return e;
}

template <class Rec>
TransitionOut analyze_transition(const Rec& r, const TransitionIn& in) {
    TransitionOut o;
    const std::size_t n = r.sample_count();
    if (n == 0 || !in.e.accept) return o;
    const std::uint32_t acc = in.e.accept_us;
    const bool up = in.to_mv >= in.from_mv;
    // The old level as measured just before the Accept (the nominal one when the window starts later).
    std::uint32_t pre_sum = 0;   // 32 bits: the ring holds transcap::kSamples (1920) samples of at most 65535 mV
    std::uint32_t pre_n = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const transcap::Sample s = r.sample(i);
        if (!detail::after(s.t_us, acc - kPreUs)) continue;
        if (detail::after(s.t_us, acc)) break;
        pre_sum += s.vbus_mv;
        ++pre_n;
    }
    const std::int32_t pre = pre_n ? std::int32_t(pre_sum / pre_n) : in.from_mv;
    const std::int32_t step = std::abs(in.to_mv - in.from_mv);
    const std::int32_t depart = step / 10 > kDepartMinMv ? step / 10 : kDepartMinMv;
    // Band entry: the sample after the last out-of-band one (scanning forwards keeps it one pass, no second buffer).
    std::size_t first_after = n;      // index of the first sample after the Accept
    std::size_t entry = n;            // candidate band entry (n = none yet)
    std::uint32_t prev_t = 0;
    bool have_prev = false;
    std::int32_t peak = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const transcap::Sample s = r.sample(i);
        const std::uint32_t gap = have_prev ? s.t_us - prev_t : 0;
        prev_t = s.t_us;
        have_prev = true;
        if (in.e.psrdy && !detail::after(s.t_us, in.e.psrdy_us + 1)) { o.vbus_at_psrdy_mv = s.vbus_mv; o.psrdy_known = true; }
        if (!detail::after(s.t_us, acc)) continue;
        if (first_after == n) { first_after = i; peak = s.vbus_mv; }
        if (gap > o.max_gap_us) o.max_gap_us = gap;
        const std::int32_t v = s.vbus_mv;
        if (up ? v > peak : v < peak) peak = v;
        if (!o.start.seen && std::abs(v - pre) > depart) {
            o.start = {s.t_us - acc, gap, true};
        }
        if (in_band(v, in.to_mv)) {
            if (entry == n) { entry = i; o.band = {s.t_us - acc, gap, true}; }
        } else {
            entry = n;
            o.band = {};
        }
        o.last_us = s.t_us - acc;
        o.last_mv = v;
    }
    o.peak_mv = peak;
    // The final level: the mean of the tail, only when the whole tail is in the band (a level still moving has no final value).
    if (first_after < n && o.band.seen) {
        const std::uint32_t t_end = r.sample(n - 1).t_us;
        std::uint32_t sum = 0;   // as pre_sum: 1920 x 65535 mV fits
        std::uint32_t cnt = 0;
        bool ok = true;
        for (std::size_t i = first_after; i < n; ++i) {
            const transcap::Sample s = r.sample(i);
            if (!detail::after(s.t_us, t_end - kTailUs)) continue;
            if (!in_band(s.vbus_mv, in.to_mv)) ok = false;
            sum += s.vbus_mv;
            ++cnt;
        }
        if (ok && cnt) { o.final_mv = std::int32_t(sum / cnt); o.final_valid = true; }
    }
    return o;
}

// A small programmable step (PPS): the level the source settled on and how long it took after the Accept.
template <class Rec>
StepOut analyze_step(const Rec& r, std::uint32_t accept_us) {
    StepOut o;
    const std::size_t n = r.sample_count();
    if (n == 0) return o;
    const std::uint32_t t_end = r.sample(n - 1).t_us;
    if (!detail::after(t_end, accept_us + kTailUs)) return o;   // no tail after the Accept to judge a level by
    std::uint32_t sum = 0;   // 1920 samples x 65535 mV fits in 32 bits
    std::uint32_t cnt = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const transcap::Sample s = r.sample(i);
        if (detail::after(s.t_us, t_end - kTailUs) && detail::after(s.t_us, accept_us)) { sum += s.vbus_mv; ++cnt; }
    }
    if (cnt == 0) return o;
    o.valid = true;
    o.final_mv = std::int32_t(sum / cnt);
    bool have = false, have_prev = false;
    std::uint32_t prev_t = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const transcap::Sample s = r.sample(i);
        const std::uint32_t gap = have_prev ? s.t_us - prev_t : 0;
        prev_t = s.t_us;
        have_prev = true;
        if (!detail::after(s.t_us, accept_us)) continue;
        if (std::abs(std::int32_t(s.vbus_mv) - o.final_mv) <= kSettleTolMv) {
            if (!have) { o.settle = {s.t_us - accept_us, gap, true}; have = true; }
        } else {
            have = false;
            o.settle = {};
        }
    }
    o.settled = have;
    return o;
}

// The Hard Reset / detach path at poll resolution (a few ms): VBUS under vSafe0V, back above it, at vSafe5V, the first capabilities.
struct ResetTimes {
    bool off_seen = false, up_seen = false, on_seen = false, caps_seen = false;
    std::uint32_t off_ms = 0;       // start -> VBUS under vSafe0V
    std::uint32_t dark_ms = 0;      // under vSafe0V -> back above it (the source's tSrcRecover)
    std::uint32_t turn_on_ms = 0;   // back above vSafe0V -> at vSafe5V (tSrcTurnOn)
    std::uint32_t caps_ms = 0;      // at vSafe5V -> the first Source_Capabilities (tFirstSourceCap)
    std::uint32_t gap_ms = 0;       // the largest spacing of the samples fed
    std::int32_t min_mv = 0;        // the lowest VBUS seen
};

class ResetTrack {
public:
    void start(std::uint32_t now_ms) { t0_ = last_ = now_ms; t_ = {}; t_.min_mv = 0x7FFFFFFF; up_at_ = on_at_ = off_at_ = 0; }
    void feed(std::uint32_t now_ms, std::int32_t vbus_mv, bool caps_in) {
        const std::uint32_t gap = now_ms - last_;
        last_ = now_ms;
        if (gap > t_.gap_ms) t_.gap_ms = gap;
        if (vbus_mv < t_.min_mv) t_.min_mv = vbus_mv;
        if (!t_.off_seen) {
            if (vbus_mv < pd::spec::kVSafe0VMaxMv) { t_.off_seen = true; t_.off_ms = now_ms - t0_; off_at_ = now_ms; }
            return;
        }
        if (!t_.up_seen) {
            if (vbus_mv > pd::spec::kVSafe0VMaxMv) { t_.up_seen = true; t_.dark_ms = now_ms - off_at_; up_at_ = now_ms; }
            else return;
        }
        if (!t_.on_seen) {
            if (vbus_mv >= pd::spec::kVSafe5VMinMv) { t_.on_seen = true; t_.turn_on_ms = now_ms - up_at_; on_at_ = now_ms; }
            else return;
        }
        if (!t_.caps_seen && caps_in) { t_.caps_seen = true; t_.caps_ms = now_ms - on_at_; }
    }
    const ResetTimes& times() const { return t_; }
    std::uint32_t since_start(std::uint32_t now_ms) const { return now_ms - t0_; }

private:
    ResetTimes t_{};
    std::uint32_t t0_ = 0, last_ = 0, off_at_ = 0, up_at_ = 0, on_at_ = 0;
};

}  // namespace k2::pdspec
