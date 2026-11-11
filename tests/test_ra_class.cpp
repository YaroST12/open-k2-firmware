// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::raclass: the verdict on the silent-cable-chip probe's raw samples (none / pull-down on CC1 / pull-down on CC2 /
// unclear). The vectors marked "measured" are the 2026-11-11 hardware numbers (docs/K2_PORTS_SPEC.md, "The CC front end");
// the ones marked "not measured" are the model's prediction and must be replaced by the first hardware run's numbers.
#include "k2/pd/ra_class.hpp"
#include "test_util.h"

using namespace k2;
using raclass::PullDown;

static std::uint16_t raw(int mv) { return std::uint16_t((mv * 4096 + 1650) / 3300); }

struct Row { int idle1[3], idle2[3], v1[3], v2[3]; };   // mV; v1 = CC1 under VCONN on CC1 at 2/10/100 ms, v2 = CC2 under VCONN on CC2

// The CC1 reading while VCONN sits on CC2 and the CC2 reading while it sits on CC1 are measured only, never judged:
// fill them with a plausible level and prove the verdict ignores them.
static raprobe::Result make(const Row &r, int other = 6) {
    raprobe::Result res;
    res.flags = raprobe::kFlagRestored;
    res.n = 9;
    for (int i = 0; i < 3; i++) {
        res.s[i] = {raw(r.idle1[i]), raw(r.idle2[i])};
        res.s[3 + i] = {raw(r.v1[i]), raw(other)};
        res.s[6 + i] = {raw(other), raw(r.v2[i])};
    }
    return res;
}

static PullDown cls(const Row &r, int other = 6) { return raclass::classify(make(r, other)); }
static int val(PullDown p) { return int(p); }

// Idle rows
static const Row kEmpty    = {{6, 6, 7},    {1692, 1693, 1694}, {2868, 2870, 2870}, {2868, 2870, 2871}};   // idle measured; VCONN not measured (bare pin number used)
static const Row kChipless = {{6, 6, 6},    {1671, 1673, 1675}, {2868, 2870, 2872}, {2868, 2870, 2872}};   // idle measured; VCONN not measured
static const Row kBaseusB  = {{6, 6, 6},    {0, 0, 0},          {2867, 2870, 2872}, {2627, 2641, 2644}};   // idle measured (cc2 0); VCONN: bare 2.87 V and the loaded pin's 2627 -> 2644 measured
static const Row kBaseusA  = {{0, 0, 0},    {1662, 1662, 1663}, {2627, 2641, 2644}, {2868, 2870, 2871}};   // idle measured (cc1 0, cc2 1662); VCONN mirrored: not measured

static void test_measured_idle_levels_with_a_modelled_vconn() {
    CHECK_EQ(val(cls(kEmpty)), val(PullDown::None));     // the empty port
    CHECK_EQ(val(cls(kChipless)), val(PullDown::None));  // the chipless cable
    CHECK_EQ(val(cls(kBaseusB)), val(PullDown::Cc2));
    CHECK_EQ(val(cls(kBaseusA)), val(PullDown::Cc1));
}

static void test_the_unjudged_lines_do_not_change_the_verdict() {
    for (int other : {0, 6, 1700, 2870, 3294}) {
        CHECK_EQ(val(cls(kEmpty, other)), val(PullDown::None));
        CHECK_EQ(val(cls(kBaseusB, other)), val(PullDown::Cc2));
        CHECK_EQ(val(cls(kBaseusA, other)), val(PullDown::Cc1));
    }
}

static void test_a_held_plus_key_is_not_a_chip() {
    // PLUS held clamps the unplugged CC2 to ~2 mV (measured) and loads a ~85 ohm source by ~45 mV (inferred): the collapse is
    // there, the droop is not, so the verdict is unclear and never "pull-down on CC2".
    const Row plus = {{6, 6, 6}, {2, 2, 2}, {2868, 2870, 2872}, {2820, 2823, 2824}};
    CHECK_EQ(val(cls(plus)), val(PullDown::Unclear));
}

static void test_both_signs_are_needed() {
    // Droop on CC2 with the bias intact: unclear (not a CC2 chip, not a clean cable).
    const Row droop_only = {{6, 6, 6}, {1670, 1670, 1670}, {2868, 2870, 2872}, {2627, 2641, 2644}};
    CHECK_EQ(val(cls(droop_only)), val(PullDown::Unclear));
    // Collapse with both lines unloaded: unclear.
    const Row collapse_only = {{6, 6, 6}, {3, 3, 3}, {2868, 2870, 2872}, {2868, 2870, 2872}};
    CHECK_EQ(val(cls(collapse_only)), val(PullDown::Unclear));
    // Collapse together with a droop on CC1 (the wrong line for a collapse): unclear.
    const Row wrong_line = {{6, 6, 6}, {3, 3, 3}, {2627, 2641, 2644}, {2868, 2870, 2872}};
    CHECK_EQ(val(cls(wrong_line)), val(PullDown::Unclear));
    // Both lines drooped (a pull-down on each, or a load on the supply): unclear for either idle level.
    const Row both_low_bias = {{6, 6, 6}, {1670, 1670, 1670}, {2627, 2641, 2644}, {2627, 2641, 2644}};
    CHECK_EQ(val(cls(both_low_bias)), val(PullDown::Unclear));
    const Row both_low_collapsed = {{6, 6, 6}, {2, 2, 2}, {2627, 2641, 2644}, {2627, 2641, 2644}};
    CHECK_EQ(val(cls(both_low_collapsed)), val(PullDown::Unclear));
}

static void test_a_single_sample_never_decides() {
    // One idle sample off its band: unclear, whichever way it points.
    Row r = kEmpty; r.idle2[1] = 3;     CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
    r = kBaseusB; r.idle2[2] = 1670;    CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
    // The 10 ms and 100 ms samples of a VCONN phase must agree (the pin has settled).
    r = kBaseusB; r.v2[1] = 2870;       CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
    r = kEmpty; r.v1[1] = 2400;         CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
    // The 2 ms sample is a transient (a sweep older than the switch change): it is never read.
    r = kBaseusB; r.v2[0] = 2870;       CHECK_EQ(val(cls(r)), val(PullDown::Cc2));
    r = kEmpty; r.v1[0] = 2627;         CHECK_EQ(val(cls(r)), val(PullDown::None));
    r = kBaseusB; r.idle2[0] = 0; r.v1[0] = 0; CHECK_EQ(val(cls(r)), val(PullDown::Cc2));
}

static void test_a_source_level_on_cc1_is_unclear() {
    Row r = kEmpty; r.idle1[0] = r.idle1[1] = r.idle1[2] = 3294;   // an open Rp on CC1
    CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
    r = kEmpty; r.idle2[0] = r.idle2[1] = r.idle2[2] = 900;        // a level between the collapse and the bias band
    CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
}

static void test_missing_and_aborted_runs() {
    raprobe::Result r = make(kBaseusB);
    r.s[4].cc2_raw = raprobe::kNoRaw;                            // the unjudged line may be missing...
    CHECK_EQ(val(raclass::classify(r)), val(PullDown::Cc2));
    r = make(kBaseusB); r.s[7].cc2_raw = raprobe::kNoRaw;        // ...a judged sample may not
    CHECK_EQ(val(raclass::classify(r)), val(PullDown::Unclear));
    r = make(kBaseusB); r.s[1].cc2_raw = raprobe::kNoRaw;
    CHECK_EQ(val(raclass::classify(r)), val(PullDown::Unclear));
    r = make(kBaseusB); r.n = 8;
    CHECK_EQ(val(raclass::classify(r)), val(PullDown::Unclear));
    r = make(kBaseusB); r.flags |= raprobe::kFlagAborted;
    CHECK_EQ(val(raclass::classify(r)), val(PullDown::Unclear));
    r = make(kBaseusB); r.refusal = std::uint8_t(raprobe::Refusal::VbusHigh); r.n = 0;
    CHECK_EQ(val(raclass::classify(r)), val(PullDown::NotProbed));   // a refusal means the probe did not run
    CHECK_EQ(val(raclass::classify(raprobe::Result{})), val(PullDown::Unclear));   // nothing sampled and no refusal
}

static void test_margins_around_the_constants() {
    using namespace raclass;
    Levels l;
    l.idle_cc1_max = 6; l.idle_cc2_min = 0; l.idle_cc2_max = 0;
    l.v1_early = l.v1_settled = 2870;
    // Droop of exactly the margin counts, one millivolt less does not.
    l.v2_early = l.v2_settled = 2870 - kDroopMinMv;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Cc2));
    l.v2_early = l.v2_settled = 2870 - kDroopMinMv + 1;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Unclear));
    // The loaded pin must also lie under the unloaded floor: a droop between two loaded levels is not "unloaded vs loaded".
    l.v1_early = l.v1_settled = kUnloadedMinMv - 1; l.v2_early = l.v2_settled = kUnloadedMinMv - 1 - kDroopMinMv;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Unclear));
    l.v1_early = l.v1_settled = kUnloadedMinMv; l.v2_early = l.v2_settled = kUnloadedMinMv - kDroopMinMv;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Cc2));
    // The settle tolerance.
    l.v1_early = l.v1_settled = 2870; l.v2_settled = 2644; l.v2_early = 2644 + kStableMv;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Cc2));
    l.v2_early = 2644 + kStableMv + 1;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Unclear));
    // Collapse and bias limits.
    l.v2_early = l.v2_settled = 2644;
    l.idle_cc2_min = 0; l.idle_cc2_max = kCollapseMv - 1;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Cc2));
    l.idle_cc2_max = kCollapseMv;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Unclear));
    // The margins against the measured numbers: 6 / 1692 mV idle, 2870 / 2644 mV under VCONN (225 mV droop).
    CHECK_COND(kCollapseMv - 6 >= 300);
    CHECK_COND(1662 - kCollapseMv >= 1000);
    CHECK_COND(2870 - 2644 - kDroopMinMv >= 100);            // the measured droop clears the margin by 100 mV or more
    CHECK_COND(kDroopMinMv - 47 >= 70);                      // the modelled PLUS-key droop stays under it by 70 mV or more
    CHECK_COND(2867 - kUnloadedMinMv >= 100);
    CHECK_COND(kUnloadedMinMv - 2646 >= 100);
}

static void test_names() {
    CHECK_STR(raclass::name(PullDown::None), "none");
    CHECK_STR(raclass::name(PullDown::Cc1), "cc1");
    CHECK_STR(raclass::name(PullDown::Cc2), "cc2");
    CHECK_STR(raclass::name(PullDown::Unclear), "unclear");
    CHECK_STR(raclass::name(PullDown::NotProbed), "not-probed");
    CHECK_EQ(val(PullDown::NotProbed), 0);   // a zero-initialised wire byte reads as "not probed"
}

int main() {
    test_measured_idle_levels_with_a_modelled_vconn();
    test_the_unjudged_lines_do_not_change_the_verdict();
    test_a_held_plus_key_is_not_a_chip();
    test_both_signs_are_needed();
    test_a_single_sample_never_decides();
    test_a_source_level_on_cc1_is_unclear();
    test_missing_and_aborted_runs();
    test_margins_around_the_constants();
    test_names();
    return test_report("test_ra_class");
}
