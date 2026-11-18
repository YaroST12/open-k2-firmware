// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Deep-level check of one PPS / AVS transition, over the 1 kHz transition capture of the row's own request: how far VBUS overshot the
// new level, when it entered the band, whether it left the band again, where the final level sat against the request, and how much it
// crept over the hold. Pure over values (a Recorder-shaped trace): the page freezes the capture and calls measure(); the runner turns the
// flags into an amber caution on the range chip, never a failure (a cap is the source's design, the levels still pass).
// Limits are the PD spec's (pd_spec_timing.hpp, docs/PD_PPS_AVS_SPEC_REFERENCE.md): the same ones the PD Spec section judges with.
// NOT judged, on purpose: the slew rate (the 30 mV/us limit is 30 V per sample at 1 kHz: unprovable, reported only) and the creep (the
// docs name no limit for it: reported only).
#pragma once

#include <cstdint>
#include <cstdlib>

#include "k2/pd/pd_spec_timing.hpp"
#include "k2/pdspec/analyze.hpp"

namespace k2::charger_test::progcheck {

enum Flag : std::uint8_t {
    kOver = 1,        // past vPpsNew + vPpsValid (vAvsNew + vAvsValid) on the side the step moves to
    kLate = 2,        // the new band entered later than tPpsSrcTransSmall / Large (tAvsSrcTransSmall / Large)
    kTransient = 4,   // left the band again for longer than tPpsTransient / tAvsTransient
    kOff = 8,         // the final level more than vPpsNew / vAvsNew (5 %) from the request
};

// The capture is frozen as the runner's hold begins (the band has been stable for kStableMs): kPostMs later it ends, so the window
// holds the ramp, the 500 ms of stability and kPostMs of the hold. The creep is read over kSpanMs of it (50 ms after the band was
// stable, to the end). The runner's hold (1000 ms from the band's entry) is 50 ms longer than kStableMs + kPostMs.
inline constexpr std::uint32_t kStableMs = 500;
inline constexpr std::uint32_t kPostMs = 450;
inline constexpr std::uint32_t kSpanMs = 900;

struct In {
    bool avs = false;
    std::int32_t from_mv = 0, to_mv = 0;   // the previous and the requested set point (from 0 = read it from the capture: the 20 ms before the Accept)
    std::uint32_t hold_us = kSpanMs * 1000;   // the span the creep is read over, back from the capture's end
};

struct Out {
    bool measured = false;                  // false: no Accept in the window or no tail after it, nothing judged
    std::uint8_t flags = 0;
    std::int32_t over_mv = 0;               // past the band edge on the side the step moves to (0 = inside it)
    std::int32_t band_us = -1;              // Accept -> VBUS in the band for good (-1 = never)
    std::int32_t lag_us = 0;                // band entry minus PS_RDY (negative: VBUS was there before PS_RDY)
    bool lag_known = false;
    std::int32_t out_us = 0;                // the longest stretch outside the band after entering it
    std::int32_t err_mv = 0;                // final level minus the request
    std::int32_t creep_mv = 0;              // the last 50 ms against the 50 ms one hold earlier (signed)
    bool creep_known = false;
    std::int32_t slew_mv_ms = 0;            // the steepest sample-to-sample slope after the Accept
};

constexpr const char* flag_text(std::uint8_t f) {
    return f & kOver ? "overshoot" : f & kLate ? "late" : f & kTransient ? "transient" : f & kOff ? "level off" : "";
}

// The judgment over a measured step (analyze_step) and its PD edges.
inline Out judge(const pdspec::StepOut& s, const pdspec::Edges& e, const In& in) {
    Out o;
    if (!s.valid || !e.accept) return o;
    o.measured = true;
    const bool up = in.to_mv >= in.from_mv;
    const std::int32_t step = std::abs(in.to_mv - in.from_mv);
    const bool large = step > std::int32_t(in.avs ? pd::spec::kAvsSmallStepMv : pd::spec::kPpsSmallStepMv);
    const std::int32_t lim_ms = std::int32_t(in.avs ? (large ? pd::spec::kAvsLargeStepMs : pd::spec::kAvsSmallStepMs)
                                                    : (large ? pd::spec::kPpsLargeStepMs : pd::spec::kPpsSmallStepMs));
    const std::int32_t extra = in.avs ? pd::spec::kAvsValidExtraMv : pd::spec::kPpsValidExtraMv;
    const std::int32_t over = up ? s.peak_mv - pdspec::band_hi(in.to_mv) : pdspec::band_lo(in.to_mv) - s.peak_mv;
    o.over_mv = over > 0 ? over : 0;
    if (over > extra) o.flags |= kOver;
    if (s.band.seen) {
        o.band_us = std::int32_t(s.band.us);
        // The same bracket rule as the PD Spec section: late only when the whole sample gap is past the limit.
        if (std::int32_t(s.band.us) - std::int32_t(s.band.gap_us) > lim_ms * 1000) o.flags |= kLate;
        if (e.psrdy) { o.lag_us = o.band_us - std::int32_t(e.psrdy_us - e.accept_us); o.lag_known = true; }
        const std::int32_t tr_ms = std::int32_t(in.avs ? pd::spec::kAvsTransientMs : pd::spec::kPpsTransientMs);
        o.out_us = std::int32_t(s.out_us);
        if (s.out_us && std::int32_t(s.out_us) - std::int32_t(s.band.gap_us) > tr_ms * 1000) o.flags |= kTransient;
    }
    const std::int32_t tol = in.to_mv * std::int32_t(in.avs ? pd::spec::kAvsNewPermille : pd::spec::kPpsNewPermille) / 1000;
    o.err_mv = s.final_mv - in.to_mv;
    if (std::abs(o.err_mv) > tol) o.flags |= kOff;
    return o;
}

// The capture's own edges and samples: find the Request and its Accept / PS_RDY, analyze the step, add the creep and the slew.
template <class Rec>
Out measure(const Rec& r, const In& in) {
    const std::size_t n = r.sample_count();
    if (n < 2) return {};
    const pdspec::Edges e = pdspec::find_edges(r, r.sample(0).t_us);
    if (!e.accept) return {};
    In x = in;
    if (!x.from_mv) {   // the old level as it stood just before the Accept (the new one when the window holds none: an up-step)
        std::uint32_t sum = 0, cnt = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const transcap::Sample p = r.sample(i);
            if (pdspec::detail::after(p.t_us, e.accept_us - pdspec::kPreUs) && !pdspec::detail::after(p.t_us, e.accept_us)) { sum += p.vbus_mv; ++cnt; }
        }
        x.from_mv = cnt ? std::int32_t(sum / cnt) : in.to_mv;
    }
    const pdspec::StepOut s = pdspec::analyze_step(r, {e.accept_us, x.from_mv, x.to_mv});
    Out o = judge(s, e, x);
    if (!o.measured) return o;
    const std::uint32_t t_end = r.sample(n - 1).t_us;
    const std::uint32_t head_from = t_end - in.hold_us;
    std::int32_t prev = 0;
    std::uint32_t prev_t = 0, head_sum = 0, head_n = 0;
    bool have = false;
    for (std::size_t i = 0; i < n; ++i) {
        const transcap::Sample x = r.sample(i);
        if (have && pdspec::detail::after(x.t_us, e.accept_us) && x.t_us != prev_t) {
            const std::int32_t slope = std::int32_t(std::abs(std::int32_t(x.vbus_mv) - prev) * 1000 / std::int32_t(x.t_us - prev_t));
            if (slope > o.slew_mv_ms) o.slew_mv_ms = slope;
        }
        prev = x.vbus_mv;
        prev_t = x.t_us;
        have = true;
        if (pdspec::detail::after(x.t_us, head_from) && !pdspec::detail::after(x.t_us, head_from + pdspec::kTailUs - 1)) { head_sum += x.vbus_mv; ++head_n; }
    }
    // The head window has to lie after the band was entered: before that it measures the ramp, not the hold.
    if (head_n && pdspec::detail::after(head_from, e.accept_us + std::uint32_t(o.band_us > 0 ? o.band_us : 0))) {
        o.creep_mv = s.final_mv - std::int32_t(head_sum / head_n);
        o.creep_known = true;
    }
    return o;
}

}  // namespace k2::charger_test::progcheck
