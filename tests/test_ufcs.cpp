// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UFCS / VIVO request arithmetic and watchdog timing, pure.
#include "k2/fastcharge/ufcs.hpp"
#include "test_util.h"

using namespace k2;

#define CHECK(c) CHECK_EQ(!!(c), 1)

static void test_units() {
    CHECK_EQ(ufcs::mv_to_units(9000), 900);     // 10 mV per unit
    CHECK_EQ(ufcs::units_to_mv(900), 9000);
    CHECK_EQ(ufcs::ma_to_units(3000), 300);     // 10 mA per unit
    CHECK_EQ(ufcs::units_to_ma(325), 3250);
}

static void test_clamp() {
    CHECK_EQ(ufcs::clamp_request_mv(9000, 36000), 9000);
    CHECK_EQ(ufcs::clamp_request_mv(20000, 12000), 12000);   // never above the ceiling
    CHECK_EQ(ufcs::clamp_request_mv(1000, 36000), ufcs::kMinMv);
    CHECK_EQ(ufcs::clamp_request_mv(9005, 36000), 9000);     // rounds down to a 10 mV unit
    CHECK_EQ(ufcs::clamp_request_mv(9000, 0), 0);            // no ceiling: nothing may be requested
}

static void test_watchdog() {
    ufcs::Watchdog w;
    CHECK(!w.due(0));
    w.fed(1000);
    CHECK(!w.due(1999));   // the source's default watchdog is 1 s
    CHECK(w.due(2000));
    CHECK(w.due(5000));
    w.fed(5000);
    CHECK(!w.due(5100));
}

int main() {
    test_units();
    test_clamp();
    test_watchdog();
    return test_report("test_ufcs");
}
