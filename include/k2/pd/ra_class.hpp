// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The verdict on the silent-cable-chip probe (k2/pd/ra_probe.hpp): does a cable that answered no e-marker query carry a
// chip's pull-down (Ra) on its supply pin, and on which of the two CC lines? Pure code over the probe's raw samples.
//
// Two signs, both required (a single sample never decides):
//  1. Idle: with nothing driven, the K2's CC2 sits ~1.69 V (the scan's own bias, tied to CC2 only; CC1 has none, so its
//     idle level is ~0 mV either way). A pull-down on the CC2 pin collapses that bias to ~0 mV [measured, the Baseus cable
//     in one orientation: cc2 0 mV against 1662-1694 mV for an empty port or a chipless cable].
//  2. VCONN: the K2 drives its VCONN source (~2.87 V through ~85 ohm, inferred) onto one line at a time. A bare pin keeps
//     ~2.87 V; a pin with a ~1 kohm pull-down sags to ~2.63-2.65 V [measured]. The droop of the driven pin against the
//     other driven pin is the second sign.
// A pull-down on CC2 shows both (collapsed idle + droop under VCONN on CC2); a pull-down on CC1 shows the droop on CC1
// only (CC1 has no idle bias to collapse) with the CC2 bias intact. Anything else, a collapse without the droop (a held
// PLUS key clamps CC2 to ~2 mV and loads the source by only ~47 mV, inferred), a droop without the collapse, a drift
// between the 10 ms and the 100 ms sample, a missing sample, an aborted run, is "unclear" and never a verdict.
//
// Provenance: the idle and the loaded levels are measured (docs/K2_PORTS_SPEC.md, "The CC front end"). The levels of VCONN
// on an empty port or a chipless cable, and of a pull-down on CC1, are modelled, not measured: the thresholds below leave
// 100 mV or more on each side of every measured level and are to be re-fitted from the first hardware run.
#pragma once
#include <cstdint>
#include "k2/pd/ra_probe.hpp"

namespace k2::raclass {

enum class PullDown : std::uint8_t {
    NotProbed = 0,   // zero on the wire: the probe did not run (a cable that was read, a refusal, an older firmware)
    None = 1,        // no pull-down on either line: a chipless cable, or nothing connected
    Cc1 = 2,         // a chip's pull-down on CC1
    Cc2 = 3,         // a chip's pull-down on CC2
    Unclear = 4,     // the samples do not support a verdict
};

constexpr const char *name(PullDown p) {
    switch (p) {
    case PullDown::None:    return "none";
    case PullDown::Cc1:     return "cc1";
    case PullDown::Cc2:     return "cc2";
    case PullDown::Unclear: return "unclear";
    default:                return "not-probed";
    }
}

// Idle CC2 below this is a collapsed bias (measured collapsed 0 mV; margin 394 mV to the CC1 idle floor of 6 mV, 1262 mV
// to the lowest measured bias 1662 mV).
constexpr std::int32_t kCollapseMv = 400;
// The pin under VCONN must sit at least this far under the other driven pin (measured droop 225-243 mV; the modelled PLUS
// key 47 mV: 73 mV above the model's worst case, 105-123 mV under the measured droop).
constexpr std::int32_t kDroopMinMv = 120;
// The undroopped pin under VCONN must read at least this (measured bare pin 2867-2872 mV; loaded pin tops at 2646 mV).
constexpr std::int32_t kUnloadedMinMv = 2750;
// The 10 ms and the 100 ms sample of a VCONN phase may differ by at most this (measured settling 2627 -> 2646 mV = 19 mV).
constexpr std::int32_t kStableMv = 60;
// The idle CC2 bias band is raprobe::kBiasMinMv .. kBiasMaxMv (1400-2000 mV); idle CC1 below raprobe::kSourceLowMaxMv.

// The judged levels, in mV. The 2 ms sample of a VCONN phase is a transient (the sweep behind it may predate the switch
// change) and is never read; the other line during a VCONN phase is not judged.
struct Levels {
    std::int32_t idle_cc1_max = 0;                  // highest of the three idle CC1 samples
    std::int32_t idle_cc2_min = 0, idle_cc2_max = 0;   // lowest / highest of the three idle CC2 samples
    std::int32_t v1_early = 0, v1_settled = 0;     // CC1 with VCONN on CC1, at 10 ms and 100 ms
    std::int32_t v2_early = 0, v2_settled = 0;     // CC2 with VCONN on CC2, at 10 ms and 100 ms
};

namespace detail {
constexpr std::int32_t abs32(std::int32_t v) { return v < 0 ? -v : v; }
}  // namespace detail

constexpr PullDown classify_levels(const Levels &l) {
    using namespace raprobe;
    // CC1 carries no idle bias: a level there means a source (Rp) or something driven, so no verdict.
    if (l.idle_cc1_max >= kSourceLowMaxMv) return PullDown::Unclear;
    const bool collapsed = l.idle_cc2_max < kCollapseMv;
    const bool biased = l.idle_cc2_min >= kBiasMinMv && l.idle_cc2_max <= kBiasMaxMv;
    if (!collapsed && !biased) return PullDown::Unclear;
    // Each driven pin must have settled.
    if (detail::abs32(l.v1_early - l.v1_settled) > kStableMv) return PullDown::Unclear;
    if (detail::abs32(l.v2_early - l.v2_settled) > kStableMv) return PullDown::Unclear;
    // Judge with the lower of the two samples of a pin, so a drift cannot lift a droop out of the margin.
    const std::int32_t v1 = l.v1_early < l.v1_settled ? l.v1_early : l.v1_settled;
    const std::int32_t v2 = l.v2_early < l.v2_settled ? l.v2_early : l.v2_settled;
    const bool v1_bare = v1 >= kUnloadedMinMv;
    const bool v2_bare = v2 >= kUnloadedMinMv;
    const bool cc2_drooped = v1_bare && (v1 - v2 >= kDroopMinMv);
    const bool cc1_drooped = v2_bare && (v2 - v1 >= kDroopMinMv);
    if (cc2_drooped && collapsed) return PullDown::Cc2;
    if (cc1_drooped && biased) return PullDown::Cc1;
    if (biased && v1_bare && v2_bare) return PullDown::None;
    return PullDown::Unclear;
}

// The verdict on a whole probe result. A refused probe is "not probed"; a run that did not complete, or lacks a judged
// sample, is "unclear".
constexpr PullDown classify(const raprobe::Result &r) {
    using namespace raprobe;
    if (r.refusal != std::uint8_t(Refusal::None)) return PullDown::NotProbed;
    if (r.n != kMaxSamples || (r.flags & kFlagAborted) != 0) return PullDown::Unclear;
    Levels l;
    std::int32_t i1_max = 0, i2_min = 0, i2_max = 0;
    for (std::uint8_t i = 0; i < kSamplesPerPhase; i++) {
        const Sample &s = r.s[sample_index(kPhaseIdle, i)];
        const std::int32_t a = raw_to_mv(s.cc1_raw), b = raw_to_mv(s.cc2_raw);
        if (a < 0 || b < 0) return PullDown::Unclear;
        if (i == 0 || a > i1_max) i1_max = a;
        if (i == 0 || b < i2_min) i2_min = b;
        if (i == 0 || b > i2_max) i2_max = b;
    }
    l.idle_cc1_max = i1_max;
    l.idle_cc2_min = i2_min;
    l.idle_cc2_max = i2_max;
    const std::int32_t e1 = raw_to_mv(r.s[sample_index(kPhaseVconn1, 1)].cc1_raw);
    const std::int32_t s1 = raw_to_mv(r.s[sample_index(kPhaseVconn1, 2)].cc1_raw);
    const std::int32_t e2 = raw_to_mv(r.s[sample_index(kPhaseVconn2, 1)].cc2_raw);
    const std::int32_t s2 = raw_to_mv(r.s[sample_index(kPhaseVconn2, 2)].cc2_raw);
    if (e1 < 0 || s1 < 0 || e2 < 0 || s2 < 0) return PullDown::Unclear;
    l.v1_early = e1; l.v1_settled = s1;
    l.v2_early = e2; l.v2_settled = s2;
    return classify_levels(l);
}

}  // namespace k2::raclass
