// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD page tabs: tab cycling, the inline double-OK gate and the three-party limiter.
#include "test_util.h"
#include "k2/pd/pd_tabs.hpp"
#include <string>

using namespace k2::pdtabs;

static void test_tabs() {
    CHECK_EQ(next(Tab::Source) == Tab::Cable, true);
    CHECK_EQ(next(Tab::Cable) == Tab::Sink, true);
    CHECK_EQ(next(Tab::Sink) == Tab::Source, true);
    CHECK_EQ(prev(Tab::Source) == Tab::Sink, true);
    CHECK_EQ(prev(Tab::Sink) == Tab::Cable, true);
    Tab t = Tab::Source;
    for (int i = 0; i < 3; i++) t = next(t);
    CHECK_EQ(t == Tab::Source, true);
}

static void test_gate() {
    OkGate g;
    CHECK_EQ(g.press(false, 100) == OkStep::Refresh, true);   // not inline: no prompt
    CHECK_EQ(g.press(false, 200) == OkStep::Refresh, true);
    CHECK_EQ(g.press(true, 1000) == OkStep::Confirm, true);   // inline: arm
    CHECK_EQ(g.armed(1500), true);
    CHECK_EQ(g.press(true, 2000) == OkStep::Query, true);     // second OK
    CHECK_EQ(g.armed(2001), false);
    CHECK_EQ(g.press(true, 3000) == OkStep::Confirm, true);   // consumed: arm again
    CHECK_EQ(g.press(true, 3000 + kConfirmMs + 1) == OkStep::Confirm, true);   // too late: arm again, no query
    g.press(true, 9000);
    g.reset();
    CHECK_EQ(g.armed(9001), false);
    CHECK_EQ(g.press(true, 9002) == OkStep::Confirm, true);
    OkGate w;   // clock wrap: arming just before the 32-bit rollover still counts
    CHECK_EQ(w.press(true, 0xFFFFFFF0u) == OkStep::Confirm, true);
    CHECK_EQ(w.press(true, 0x10u) == OkStep::Query, true);
}

static void test_limit() {
    Limited r = limit({100, 100, 45});
    CHECK_EQ(r.value, 45); CHECK_EQ(r.limit == Who::Sink, true);
    r = limit({100, 60, 100});
    CHECK_EQ(r.value, 60); CHECK_EQ(r.limit == Who::Cable, true);
    r = limit({18, 60, 100});
    CHECK_EQ(r.value, 18); CHECK_EQ(r.limit == Who::Source, true);
    r = limit({60, 60, 60});
    CHECK_EQ(r.value, 60); CHECK_EQ(r.limit == Who::None, true);     // nobody holds it back
    r = limit({100, kUnknown, kUnknown});
    CHECK_EQ(r.value, 100); CHECK_EQ(r.limit == Who::None, true);    // one known term cannot limit
    r = limit({kUnknown, 5000, 10000});
    CHECK_EQ(r.value, 5000); CHECK_EQ(r.limit == Who::Cable, true);   // unknown terms are skipped
    r = limit({});
    CHECK_EQ(r.value, kUnknown); CHECK_EQ(r.limit == Who::None, true);
    r = limit({0, 60, 100});
    CHECK_EQ(r.value, 0); CHECK_EQ(r.limit == Who::Source, true);
}

static std::string rate(int mbps) { char b[8]; fmt_rate(b, mbps); return b; }
static std::string watts(int w) { char b[8]; fmt_watts(b, w); return b; }

static void test_format() {
    CHECK_STR(rate(480).c_str(), "480M");
    CHECK_STR(rate(5000).c_str(), "5G");
    CHECK_STR(rate(10000).c_str(), "10G");
    CHECK_STR(rate(40000).c_str(), "40G");
    CHECK_STR(rate(80000).c_str(), "80G");
    CHECK_STR(rate(kUnknown).c_str(), "?");
    CHECK_STR(watts(100).c_str(), "100W");
    CHECK_STR(watts(0).c_str(), "0W");
    CHECK_STR(watts(kUnknown).c_str(), "?");
    char tiny[3];
    CHECK_EQ(int(fmt_watts(tiny, 240)), 2);   // truncated, NUL kept
    CHECK_STR(tiny, "24");
}

static void test_cable_speed() {
    CHECK_EQ(cable_mbps(0, 0), 480);
    CHECK_EQ(cable_mbps(1, 0), 5000);
    CHECK_EQ(cable_mbps(2, 0), 10000);
    CHECK_EQ(cable_mbps(3, 0), 40000);
    CHECK_EQ(cable_mbps(4, 0), 80000);
    CHECK_EQ(cable_mbps(2, 0x8087), 40000);
    CHECK_EQ(cable_mbps(1, 0x8087), 5000);
}

int main() {
    test_tabs();
    test_gate();
    test_limit();
    test_format();
    test_cable_speed();
    return test_report("test_pd_tabs");
}
