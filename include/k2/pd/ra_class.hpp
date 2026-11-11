// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The verdict on the silent-cable-chip probe (k2/pd/ra_probe.hpp): does a cable that answered no e-marker query carry a
// chip's pull-down (Ra) on its supply pin, and on which of the two CC lines? Pure code over the probe's raw samples.
//
// Two signs, both required (a single sample never decides):
//  1. Idle: with nothing driven, the K2's CC2 sits ~1.64-1.71 V (the scan's own bias, tied to CC2 only; CC1 has none, so
//     its idle level is 0-11 mV either way). A pull-down on the CC2 pin collapses that bias to 0 mV.
//  2. VCONN: the K2 drives its VCONN source onto one line at a time and each line is judged against its OWN bare level,
//     never against the other line: a bare CC1 reads 2868-2872 mV, a bare CC2 reads 3268-3283 mV (a different level, so a
//     cross-line difference is not a droop: it read an empty port as a CC1 chip), and a pin with a cable's ~1 kohm pull-down
//     reads 2640-2643 mV on either line, up to 2683 mV on CC1 while a cable's chip is still settling.
// A pull-down on CC2 shows both (collapsed idle + CC2 loaded under VCONN on CC2, CC1 bare); a pull-down on CC1 shows CC1
// loaded under VCONN on CC1 with CC2 bare and the bias intact; no pull-down is the bias intact with both lines bare. Anything
// else (a collapse with both lines bare, such as a held PLUS key; a load without the collapse on CC2; both lines loaded; a
// pin whose 10 ms and 100 ms samples sit on different sides of its threshold; a missing sample; an aborted run) is
// "unclear" and never a verdict. The line that is NOT driven is never judged: with a CUKTECH 6 A cable on CC1 the
// undriven CC2 read 1255-1264 mV instead of ~3278 mV, with a Baseus cable it read 3278 (CC1 chip) or 0 (CC2 chip).
//
// Provenance: measured, 2026-11-11, build 1.0.0-509, each cable alone in the bottom port (docs/K2_PORTS_SPEC.md, "The CC front
// end"): an empty port, a chipless C-to-C cable, a Baseus 5 A cable in both orientations (chip on CC1, chip on CC2) and a
// CUKTECH 6 A cable (chip on CC1). tests/test_ra_class.cpp holds the raw counts of all five runs.
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

// Idle CC2 below this is a collapsed bias; the same 400 mV as raprobe::kSourceLowMaxMv, which also bounds idle CC1.
// Margins: collapsed idle CC2 measured 0 mV (400 mV under it), idle CC1 measured 0-11 mV (389 mV under it), the lowest
// measured bias 1635 mV (1235 mV over it).
constexpr std::int32_t kCollapseMv = 400;
// A driven line is "bare" (no cable pull-down) at or above its threshold and "loaded" below it. Each line has its own
// threshold, set midway between its loaded and its bare levels:
//   CC1: loaded 2640-2644 mV, 2683 mV at the end of a chip's settling (CUKTECH, 100 ms); bare 2868-2872 mV.
//        2775 mV: 92 mV above the highest loaded reading, 93 mV below the lowest bare one.
//   CC2: loaded 2641-2643 mV; bare 3268-3283 mV.
//        2960 mV: 317 mV above the highest loaded reading, 308 mV below the lowest bare one.
// A pin counts only when its 10 ms and its 100 ms sample sit on the same side (a pin still moving across its threshold, or
// a drift that big, is "unclear"): that replaces a fixed settling tolerance, which a chip's 39 mV creep came within 21 mV of.
constexpr std::int32_t kCc1BareMinMv = 2775;
constexpr std::int32_t kCc2BareMinMv = 2960;
// The idle CC2 bias band is raprobe::kBiasMinMv .. kBiasMaxMv (1400-2000 mV; measured 1635-1712 mV, 235 mV and 288 mV of
// margin); idle CC1 below raprobe::kSourceLowMaxMv.

// The judged levels, in mV. The 2 ms sample of a VCONN phase is a transient (the sweep behind it may predate the switch
// change) and is never read; the other line during a VCONN phase is not judged.
struct Levels {
    std::int32_t idle_cc1_max = 0;                  // highest of the three idle CC1 samples
    std::int32_t idle_cc2_min = 0, idle_cc2_max = 0;   // lowest / highest of the three idle CC2 samples
    std::int32_t v1_early = 0, v1_settled = 0;     // CC1 with VCONN on CC1, at 10 ms and 100 ms
    std::int32_t v2_early = 0, v2_settled = 0;     // CC2 with VCONN on CC2, at 10 ms and 100 ms
};

enum class LineState : std::uint8_t { Loaded, Bare, Mixed };

constexpr LineState line_state(std::int32_t early, std::int32_t settled, std::int32_t bare_min) {
    const bool e = early >= bare_min, s = settled >= bare_min;
    return e != s ? LineState::Mixed : e ? LineState::Bare : LineState::Loaded;
}

constexpr PullDown classify_levels(const Levels &l) {
    using namespace raprobe;
    // CC1 carries no idle bias: a level there means a source (Rp) or something driven, so no verdict.
    if (l.idle_cc1_max >= kSourceLowMaxMv) return PullDown::Unclear;
    const bool collapsed = l.idle_cc2_max < kCollapseMv;
    const bool biased = l.idle_cc2_min >= kBiasMinMv && l.idle_cc2_max <= kBiasMaxMv;
    if (!collapsed && !biased) return PullDown::Unclear;
    const LineState c1 = line_state(l.v1_early, l.v1_settled, kCc1BareMinMv);
    const LineState c2 = line_state(l.v2_early, l.v2_settled, kCc2BareMinMv);
    if (c1 == LineState::Mixed || c2 == LineState::Mixed) return PullDown::Unclear;
    if (collapsed && c2 == LineState::Loaded && c1 == LineState::Bare) return PullDown::Cc2;
    if (biased && c1 == LineState::Loaded && c2 == LineState::Bare) return PullDown::Cc1;
    if (biased && c1 == LineState::Bare && c2 == LineState::Bare) return PullDown::None;
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
