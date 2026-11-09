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

static void test_power_line() {
    struct C { Terms t; bool assumed; const char *text; bool warn; };
    static const C cases[] = {
        {{100, 100, 18}, false, "18W of 100W", false},          // the phone draws part of the offer: normal, neutral
        {{100, 60, 18}, false, "18W of 100W", false},           // a thin cable that still carries the draw is no finding
        {{100, 60, kUnknown}, false, "60W by cable", true},     // the cable holds a bigger offer down: the one amber case
        {{100, 60, 80}, false, "60W by cable", true},
        {{100, 60, kUnknown}, true, "60W by cable?", false},    // no e-marker read: 3 A assumed, not alarming
        {{100, kUnknown, kUnknown}, false, "100W", false},
        {{60, 60, 60}, false, "60W", false},
        {{30, 100, 100}, false, "30W", false},                  // the source is the small one: plain
        {{kUnknown, kUnknown, kUnknown}, false, "?", false},
        {{kUnknown, 100, 18}, false, "18W of 100W", false},
        {{240, 240, 240}, false, "240W", false},
        {{240, 100, kUnknown}, true, "100W by cable?", false},
        {{240, 240, 100}, false, "100W of 240W", false},
    };
    for (const C &c : cases) {
        char b[32];
        bool warn = !c.warn;
        const std::size_t n = power_line(b, c.t, c.assumed, warn);
        CHECK_STR(b, c.text);
        CHECK_EQ(n, strlen(c.text));
        CHECK_EQ(warn, c.warn);
        CHECK_EQ(n <= 19, true);                                // the value column of the Connection page
    }
}

// The Type-C default budget of a link without a decoded PD contract, labelled as a budget ("up to ..."),
// never as a measurement: exact (5 V x mA is a whole mW, so no rounding that changes the number) and
// within the 19-cell value column of the Connection page in every case.
static void test_budget_line() {
    struct C { std::uint32_t ma; const char *text; };
    static const C cases[] = {
        {500, "up to 2.5W"},      // 5 V x 500 mA
        {900, "up to 4.5W"},
        {1500, "up to 7.5W"},     // 5 V x 1.5 A
        {3000, "up to 15W"},      // 5 V x 3 A
        {5000, "up to 25W"},
        {1234, "up to 6.17W"},    // an odd current still prints exactly in hundredths
        {0, "?"},                 // no current advertised
    };
    for (const C &c : cases) {
        char b[32];
        const std::size_t n = budget_line(b, c.ma);
        CHECK_STR(b, c.text);
        CHECK_EQ(n, strlen(c.text));
        CHECK_EQ(n <= 19, true);  // the value column of the Connection page (kValCells)
    }
    char tiny[3];
    CHECK_EQ(int(budget_line(tiny, 3000)), 2);   // truncated, NUL kept
    CHECK_STR(tiny, "up");
}

static void test_data_line() {
    struct C { Terms t; bool dp; const char *text; bool warn; };
    static const C cases[] = {
        {{kUnknown, 480, 480}, false, "480M", false},
        {{kUnknown, 10000, 5000}, false, "5G of 10G", false},
        {{kUnknown, 5000, 10000}, false, "5G by cable", true},
        {{kUnknown, 40000, 10000}, true, "10G of 40G +DP", false},
        {{kUnknown, 5000, 10000}, true, "5G by cable +DP", true},
        {{kUnknown, 80000, kUnknown}, false, "80G cable", false},   // only the cable is known
        {{kUnknown, kUnknown, 480}, false, "480M", false},
        {{kUnknown, kUnknown, kUnknown}, false, "?", false},
        {{kUnknown, kUnknown, kUnknown}, true, "?", false},        // nothing known: no DP tag either
        {{kUnknown, 5000, 480}, true, "480M of 5G +DP", false},
    };
    for (const C &c : cases) {
        char b[32];
        bool warn = !c.warn;
        const std::size_t n = data_line(b, c.t, c.dp, warn);
        CHECK_STR(b, c.text);
        CHECK_EQ(n, strlen(c.text));
        CHECK_EQ(warn, c.warn);
        CHECK_EQ(n <= 19, true);
    }
}

static void test_speed_short() {
    struct C { const char *in, *out; };
    static const C cases[] = {
        {"Thunderbolt (40G)", "TB 40G"}, {"USB 3.2 Gen1 (5G)", "3.2 Gen1 5G"}, {"USB 3.2 Gen2 (10G)", "3.2 Gen2 10G"},
        {"USB4 Gen3 (40G)", "USB4 40G"}, {"USB4 Gen4 (80G)", "USB4 80G"}, {"USB 2.0 (480M)", "2.0 480M"},
        {"", ""}, {"odd", "odd"},
    };
    for (const C &c : cases) {
        char b[32];
        const std::size_t n = speed_short(b, c.in);
        CHECK_STR(b, c.out);
        CHECK_EQ(n, strlen(c.out));
    }
    // The longest result plus both tags still fits the 19-cell value column ("3.2 Gen2 10G x2 TB" = 18).
    CHECK_EQ(strlen("3.2 Gen2 10G") + strlen(" x2") + strlen(" TB") <= 19, true);
}

int main() {
    test_speed_short();
    test_power_line();
    test_budget_line();
    test_data_line();
    test_tabs();
    test_gate();
    test_limit();
    test_format();
    test_cable_speed();
    return test_report("test_pd_tabs");
}
