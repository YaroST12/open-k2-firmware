// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Flow-rail animation mapping: power relative to the allowed maximum -> heads (density) and speed.
#include "test_util.h"
#include <initializer_list>
#include <initializer_list>
#include "k2/ui/flow_anim.hpp"

using namespace k2::flow_anim;

int main() {
    // tiers snap the advertised maximum up to the next of 15/60/100/140/165/240 W; no PD = 15 W
    CHECK_EQ(tier_index(0), 0);
    CHECK_EQ(tier_index(15), 0);
    CHECK_EQ(tier_index(16), 1);
    CHECK_EQ(tier_index(45), 1);
    CHECK_EQ(tier_index(100), 2);
    CHECK_EQ(tier_index(140), 3);
    CHECK_EQ(tier_index(165), 4);
    CHECK_EQ(tier_index(240), 5);
    CHECK_EQ(tier_index(500), 5);
    // load fraction in permille, clamped
    CHECK_EQ(load_permille(30000, 60), 500);
    CHECK_EQ(load_permille(-30000, 60), 500);
    CHECK_EQ(load_permille(90000, 60), 1000);
    CHECK_EQ(load_permille(5000, 0), 333);           // max 0 -> 15 W default
    // idle: no motion
    CHECK_EQ(compute(0, 60).heads, 0);
    CHECK_EQ(compute(0, 60).speed_q8, 0);
    // known points (60 W tier)
    const Params lo = compute(6000, 60), mid = compute(30000, 60), hi = compute(60000, 60);
    CHECK_EQ(lo.heads, 1);
    CHECK_EQ(mid.heads, 2);
    CHECK_EQ(hi.heads, 3);
    CHECK_EQ(lo.speed_q8, 2 * 256 + 5 * 256 * 100 / 1000);   // 2 chev/s floor + 5 * 10 %
    // 240 W tier: full load gets 4 heads and the top speed
    CHECK_EQ(compute(240000, 240).heads, 4);
    CHECK_EQ(compute(240000, 240).speed_q8, 11 * 256);
    CHECK_EQ(compute(60000, 60).speed_q8, 7 * 256);   // 60 W tier tops out at 7 chev/s
    // monotonic in power, same max
    for (int mx : {15, 60, 100, 140, 165, 240}) {
        Params prev{0, 0};
        for (int mw = 0; mw <= mx * 1000; mw += 500) {
            const Params p = compute(mw, mx);
            CHECK_EQ(p.heads >= prev.heads, 1);
            CHECK_EQ(p.speed_q8 >= prev.speed_q8, 1);
            prev = p;
        }
    }
    // same load fraction: a bigger tier is never slower
    CHECK_EQ(compute(120000, 240).speed_q8 >= compute(30000, 60).speed_q8, 1);
    // multi-head levels: heads spread evenly, each with its trail; max over heads
    int bright = 0;
    for (int k = 0; k < 9; k++) if (rail_level(k, 0, 3, false) == 2) bright++;
    CHECK_EQ(bright, 3);
    CHECK_EQ(rail_level(0, 0, 1, false), 2);   // head 0 at chevron 0 moving down
    CHECK_EQ(rail_level(1, 0, 1, false), 0);
    CHECK_EQ(rail_level(1, 256, 1, false), 2); // phase advances one chevron per 256
    CHECK_EQ(rail_level(8, 0, 1, true), 2);    // moving up starts from the bottom
    CHECK_EQ(rail_level(5, 255, 1, false), 0); // fractional phase does not move the head
    // accumulator: ticks never go backwards and wrap inside one ring
    uint32_t ph = 0;
    for (int i = 0; i < 1000; i++) { ph = advance(ph, 11 * 256, 33); CHECK_EQ(ph < 9u * 256u, 1); }
    CHECK_EQ(advance(0, 256, 1000), 256u);
    // status-bar chevrons: one head round a ring of three chevrons and a gap
    CHECK_EQ(bar_level(0, 0, 1, true), 2);      // forward: the head starts at the first chevron
    CHECK_EQ(bar_level(1, 0, 1, true), 0);
    CHECK_EQ(bar_level(1, 256, 1, true), 2);    // one step later it is on the second, the first trails
    CHECK_EQ(bar_level(0, 256, 1, true), 1);
    CHECK_EQ(bar_level(0, 256, 3, true), 1);
    CHECK_EQ(bar_level(2, 2 * 256, 3, true), 2);
    CHECK_EQ(bar_level(0, 2 * 256, 3, true), 0);    // 3 heads: a trail of three steps (2,1,0) ...
    CHECK_EQ(bar_level(1, 2 * 256, 3, true), 1);
    CHECK_EQ(bar_level(2, 0, 1, false), 2);     // reverse: starts at the last chevron, runs toward 0
    CHECK_EQ(bar_level(2, 256, 1, false), 1);
    CHECK_EQ(bar_level(1, 256, 1, false), 2);
    CHECK_EQ(bar_level(0, 3 * 256, 1, false), 1);   // the head has left through the first chevron: trail only
    CHECK_EQ(bar_level(1, 3 * 256, 1, false), 0);
    CHECK_EQ(bar_level(2, 3 * 256, 1, false), 0);
    CHECK_EQ(bar_level(2, 3 * 256, 1, true), 1);    // forward gap step: head beyond the last chevron, trail on it
    CHECK_EQ(bar_level(1, 3 * 256, 1, true), 0);
    for (int i = 0; i < 100; i++) { ph = advance_bar(ph, 11 * 256, 33); CHECK_EQ(ph < 4u * 256u, 1); }
    CHECK_EQ(advance_bar(0, 256, 4000), 0u);    // four steps = one lap
    return test_report("test_flow_anim");
}
