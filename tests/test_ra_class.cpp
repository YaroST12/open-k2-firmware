// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::raclass: the verdict on the silent-cable-chip probe's raw samples (none / pull-down on CC1 / pull-down on CC2 /
// unclear). kHw holds the raw counts of the six 2026-11-11 hardware runs (docs/K2_PORTS_SPEC.md, "The CC front end"); the
// synthetic rows below are built from the same measured levels (bare CC1 2870 mV, bare CC2 3275 mV, loaded 2642 mV).
#include "k2/pd/ra_class.hpp"
#include <cstdio>
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
// The measured levels in mV: a bare CC1 reads ~2870 and a bare CC2 ~3275 under VCONN, a loaded pin ~2642 on either line.
static const Row kEmpty    = {{6, 6, 7},    {1692, 1693, 1694}, {2869, 2871, 2870}, {3276, 3272, 3278}};
static const Row kChipless = {{7, 6, 6},    {1683, 1635, 1712}, {2870, 2871, 2868}, {3277, 3282, 3279}};
static const Row kBaseusB  = {{8, 5, 8},    {0, 0, 0},          {2870, 2868, 2872}, {2641, 2643, 2643}};   // chip on CC2
static const Row kBaseusA  = {{0, 0, 0},    {1666, 1650, 1661}, {2640, 2643, 2642}, {3270, 3279, 3277}};   // chip on CC1

// The six `k2_remote emarker probe --json` runs of build 1.0.0-509-0061d3a2, 2026-11-11, each cable alone in the bottom port:
// the raw ADC counts as the probe reported them (cc1, cc2 for idle x3, VCONN on CC1 x3, VCONN on CC2 x3 at 2/10/100 ms).
struct Hw { const char *name; std::uint16_t s[9][2]; PullDown verdict; };
static const Hw kHw[] = {
    {"2-empty port",  {{9, 2110}, {9, 2109}, {8, 2109}, {3562, 4066}, {3564, 4063}, {3563, 4071}, {0, 4067}, {0, 4062}, {4, 4069}}, PullDown::None},
    {"3-chipless",    {{9, 2090}, {8, 2030}, {8, 2125}, {3563, 4067}, {3564, 4057}, {3561, 4075}, {0, 4068}, {4, 4074}, {2, 4071}}, PullDown::None},
    {"4-baseus-a",    {{0, 2069}, {1, 2049}, {0, 2062}, {3278, 4069}, {3281, 4069}, {3280, 4071}, {0, 4059}, {0, 4070}, {0, 4068}}, PullDown::Cc1},
    {"5-baseus-b",    {{11, 0}, {7, 0}, {10, 0}, {3563, 0}, {3561, 0}, {3566, 0}, {0, 3279}, {2, 3281}, {4, 3281}}, PullDown::Cc2},
    {"6-cuktech",     {{0, 2082}, {0, 2081}, {0, 2083}, {3278, 1558}, {3283, 1568}, {3331, 1569}, {0, 4067}, {0, 4067}, {0, 4071}}, PullDown::Cc1},
};

static raprobe::Result make_hw(const Hw &h) {
    raprobe::Result res;
    res.flags = raprobe::kFlagRestored;
    res.n = 9;
    for (int i = 0; i < 9; i++) res.s[i] = {h.s[i][0], h.s[i][1]};
    return res;
}

static void test_the_measured_hardware_runs() {
    for (const Hw &h : kHw) {
        if (raclass::classify(make_hw(h)) != h.verdict) std::printf("  hardware fixture %s: got %s\n", h.name, raclass::name(raclass::classify(make_hw(h))));
        CHECK_EQ(val(raclass::classify(make_hw(h))), val(h.verdict));
    }
    // 1-empty: the port refused (a laptop's Rp on the top plug) and drove nothing: not probed, whatever the samples say.
    raprobe::Result refused;
    refused.refusal = std::uint8_t(raprobe::Refusal::SourceLevel);
    CHECK_EQ(val(raclass::classify(refused)), val(PullDown::NotProbed));
}

static void test_the_measured_levels_as_rows() {
    CHECK_EQ(val(cls(kEmpty)), val(PullDown::None));     // the empty port
    CHECK_EQ(val(cls(kChipless)), val(PullDown::None));  // the chipless cable
    CHECK_EQ(val(cls(kBaseusB)), val(PullDown::Cc2));
    CHECK_EQ(val(cls(kBaseusA)), val(PullDown::Cc1));
}

static void test_a_bare_cc2_is_not_a_droop_against_a_bare_cc1() {
    // The two bare lines read different levels (CC1 ~2870, CC2 ~3275 mV): a cross-line difference of ~400 mV is not a
    // droop. Judging CC1 against CC2 read the empty port as a pull-down on CC1; each line is judged against its own level.
    CHECK_COND(kEmpty.v2[2] - kEmpty.v1[2] > 300);
    CHECK_EQ(val(cls(kEmpty)), val(PullDown::None));
    CHECK_EQ(val(cls(kChipless)), val(PullDown::None));
}

static void test_the_unjudged_lines_do_not_change_the_verdict() {
    // 1260 mV is what a CUKTECH cable's other CC line read while VCONN sat on CC1 (a chip side effect, measured).
    for (int other : {0, 6, 1260, 1700, 2870, 3294}) {
        CHECK_EQ(val(cls(kEmpty, other)), val(PullDown::None));
        CHECK_EQ(val(cls(kBaseusB, other)), val(PullDown::Cc2));
        CHECK_EQ(val(cls(kBaseusA, other)), val(PullDown::Cc1));
    }
}

static void test_a_chip_that_creeps_while_settling_still_decides() {
    // The CUKTECH cable's CC1 under VCONN read 2640 / 2644 / 2683 mV at 2 / 10 / 100 ms: 39 mV of creep, still loaded.
    Row r = kBaseusA; r.v1[0] = 2640; r.v1[1] = 2644; r.v1[2] = 2683;
    CHECK_EQ(val(cls(r)), val(PullDown::Cc1));
    // Even 90 mV of creep stays loaded; a bare-looking 100 ms sample after a loaded 10 ms sample does not decide.
    r.v1[2] = 2740;                     CHECK_EQ(val(cls(r)), val(PullDown::Cc1));
    r.v1[2] = 2870;                     CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
}

static void test_a_held_plus_key_is_not_a_chip() {
    // PLUS held clamps the unplugged CC2 to ~2 mV (measured): the collapse is there, the load under VCONN is not, so the
    // verdict is unclear and never "pull-down on CC2" (a bare CC2 ~3275 mV, minus ~47 mV of the key's load, inferred).
    const Row plus = {{6, 6, 6}, {2, 2, 2}, {2870, 2871, 2868}, {3228, 3232, 3229}};
    CHECK_EQ(val(cls(plus)), val(PullDown::Unclear));
}

static void test_both_signs_are_needed() {
    // Load on CC2 with the bias intact: unclear (not a CC2 chip, not a clean cable).
    const Row load_only = {{6, 6, 6}, {1670, 1670, 1670}, {2870, 2871, 2868}, {2641, 2643, 2643}};
    CHECK_EQ(val(cls(load_only)), val(PullDown::Unclear));
    // Collapse with both lines bare: unclear.
    const Row collapse_only = {{6, 6, 6}, {3, 3, 3}, {2870, 2871, 2868}, {3276, 3272, 3278}};
    CHECK_EQ(val(cls(collapse_only)), val(PullDown::Unclear));
    // Collapse together with a load on CC1 (the wrong line for a collapse): unclear.
    const Row wrong_line = {{6, 6, 6}, {3, 3, 3}, {2640, 2643, 2642}, {3276, 3272, 3278}};
    CHECK_EQ(val(cls(wrong_line)), val(PullDown::Unclear));
    // Both lines loaded (a pull-down on each, or a load on the supply): unclear for either idle level.
    const Row both_low_bias = {{6, 6, 6}, {1670, 1670, 1670}, {2640, 2643, 2642}, {2641, 2643, 2643}};
    CHECK_EQ(val(cls(both_low_bias)), val(PullDown::Unclear));
    const Row both_low_collapsed = {{6, 6, 6}, {2, 2, 2}, {2640, 2643, 2642}, {2641, 2643, 2643}};
    CHECK_EQ(val(cls(both_low_collapsed)), val(PullDown::Unclear));
}

static void test_a_single_sample_never_decides() {
    // One idle sample off its band: unclear, whichever way it points.
    Row r = kEmpty; r.idle2[1] = 3;     CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
    r = kBaseusB; r.idle2[2] = 1670;    CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
    // The 10 ms and 100 ms samples of a VCONN phase must land on the same side of the line's threshold.
    r = kBaseusB; r.v2[1] = 3275;       CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
    r = kEmpty; r.v1[1] = 2400;         CHECK_EQ(val(cls(r)), val(PullDown::Unclear));
    // The 2 ms sample is a transient (a sweep older than the switch change): it is never read.
    r = kBaseusB; r.v2[0] = 3275;       CHECK_EQ(val(cls(r)), val(PullDown::Cc2));
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
    // Chip on CC2: CC1 bare, CC2 loaded. The threshold itself counts as bare, one millivolt under it as loaded.
    l.v1_early = l.v1_settled = kCc1BareMinMv;
    l.v2_early = l.v2_settled = kCc2BareMinMv - 1;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Cc2));
    l.v2_early = l.v2_settled = kCc2BareMinMv;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Unclear));         // both bare + collapse
    l.v1_early = l.v1_settled = kCc1BareMinMv - 1; l.v2_early = l.v2_settled = kCc2BareMinMv - 1;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Unclear));         // both loaded
    // One sample on each side of a threshold is a pin in motion: unclear.
    l.v1_early = kCc1BareMinMv; l.v1_settled = kCc1BareMinMv - 1; l.v2_early = l.v2_settled = kCc2BareMinMv - 1;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Unclear));
    l.v1_early = l.v1_settled = kCc1BareMinMv; l.v2_early = kCc2BareMinMv - 1; l.v2_settled = kCc2BareMinMv;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Unclear));
    // Collapse and bias limits.
    l.v2_early = l.v2_settled = 2644;
    l.idle_cc2_min = 0; l.idle_cc2_max = kCollapseMv - 1;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Cc2));
    l.idle_cc2_max = kCollapseMv;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Unclear));
    // Chip on CC1, bias intact, the CUKTECH creep at the edge of the margin.
    l.idle_cc2_min = 1650; l.idle_cc2_max = 1680;
    l.v1_early = 2644; l.v1_settled = kCc1BareMinMv - 1; l.v2_early = l.v2_settled = 3275;
    CHECK_EQ(val(classify_levels(l)), val(PullDown::Cc1));

    // The margins against every measured level (mV): idle CC1 0-11, idle CC2 collapsed 0 / biased 1635-1712; CC1 under VCONN
    // loaded 2640-2683 / bare 2868-2872; CC2 under VCONN loaded 2641-2643 / bare 3268-3283.
    CHECK_COND(kCollapseMv - 11 >= 380 && kCollapseMv - 0 >= 400 && 1635 - kCollapseMv >= 1200);
    CHECK_COND(raprobe::kBiasMinMv <= 1635 - 200 && raprobe::kBiasMaxMv >= 1712 + 280);
    CHECK_COND(kCc1BareMinMv - 2683 >= 90 && 2868 - kCc1BareMinMv >= 90);
    CHECK_COND(kCc2BareMinMv - 2643 >= 300 && 3268 - kCc2BareMinMv >= 300);
    // The verdict survives every judged VCONN level moving by 80 mV (CC1) / 300 mV (CC2) in either direction, which is the
    // margin the thresholds leave to the measured levels, on the measured idle levels.
    for (const Hw &h : kHw) {
        for (int d1 : {-80, 0, 80}) {
            for (int d2 : {-300, 0, 300}) {
                Levels m;
                m.idle_cc1_max = 0; m.idle_cc2_min = m.idle_cc2_max = raprobe::raw_to_mv(h.s[0][1]);
                for (int i = 0; i < 3; i++) {
                    const int a = raprobe::raw_to_mv(h.s[i][0]), b = raprobe::raw_to_mv(h.s[i][1]);
                    if (a > m.idle_cc1_max) m.idle_cc1_max = a;
                    if (b < m.idle_cc2_min) m.idle_cc2_min = b;
                    if (b > m.idle_cc2_max) m.idle_cc2_max = b;
                }
                m.v1_early = raprobe::raw_to_mv(h.s[4][0]) + d1; m.v1_settled = raprobe::raw_to_mv(h.s[5][0]) + d1;
                m.v2_early = raprobe::raw_to_mv(h.s[7][1]) + d2; m.v2_settled = raprobe::raw_to_mv(h.s[8][1]) + d2;
                CHECK_EQ(val(classify_levels(m)), val(h.verdict));
            }
        }
    }
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
    test_the_measured_hardware_runs();
    test_the_measured_levels_as_rows();
    test_a_bare_cc2_is_not_a_droop_against_a_bare_cc1();
    test_a_chip_that_creeps_while_settling_still_decides();
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
