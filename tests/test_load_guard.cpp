// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the PA8 load guard: bounded duty and time, forced off on every trip.
#include "k2/drivers/load_guard.hpp"
#include "test_util.h"

using namespace k2::load;

int main() {
    Inputs ok{5000, 450, true, false};
    Pulse p;
    CHECK_EQ(p.begin(30, 100, 1000, ok) == Abort::None, 1);
    CHECK_EQ(p.poll(1000, ok), 30);
    CHECK_EQ(p.poll(1099, ok), 30);
    CHECK_EQ(p.poll(1100, ok), 0);            // timeout at the deadline
    CHECK_EQ(p.active(), 0);
    CHECK_EQ(p.reason() == Abort::Done, 1);
    CHECK_EQ(p.poll(1101, ok), 0);            // stays off

    // wrap-safe deadline
    CHECK_EQ(p.begin(95, 100, 0xFFFFFFF0u, ok) == Abort::None, 1);
    CHECK_EQ(p.poll(0x10, ok), 95);
    CHECK_EQ(p.poll(0x80, ok), 0);

    // refusals
    CHECK_EQ(p.begin(96, 100, 0, ok) != Abort::None, 1);
    CHECK_EQ(p.begin(0, 100, 0, ok) != Abort::None, 1);
    CHECK_EQ(p.begin(30, 201, 0, ok) != Abort::None, 1);
    CHECK_EQ(p.begin(30, 0, 0, ok) != Abort::None, 1);
    CHECK_EQ(p.active(), 0);
    Inputs low = ok; low.vbus_mv = 4499;
    CHECK_EQ(p.begin(30, 100, 0, low) == Abort::VbusLow, 1);
    Inputs high = ok; high.vbus_mv = 9000;
    CHECK_EQ(p.begin(30, 100, 0, high) == Abort::VbusHigh, 1);
    Inputs hot = ok; hot.mcu_deci_c = 701;
    CHECK_EQ(p.begin(30, 100, 0, hot) == Abort::Temp, 1);
    Inputs stopf = ok; stopf.external_stop = true;
    CHECK_EQ(p.begin(30, 100, 0, stopf) == Abort::External, 1);

    // trips mid-pulse
    CHECK_EQ(p.begin(95, 200, 0, ok) == Abort::None, 1);
    CHECK_EQ(p.poll(10, low), 0);
    CHECK_EQ(p.reason() == Abort::VbusLow, 1);
    CHECK_EQ(p.begin(95, 200, 0, ok) == Abort::None, 1);
    CHECK_EQ(p.poll(10, hot), 0);
    CHECK_EQ(p.reason() == Abort::Temp, 1);
    CHECK_EQ(p.begin(95, 200, 0, ok) == Abort::None, 1);
    CHECK_EQ(p.poll(10, stopf), 0);
    CHECK_EQ(p.reason() == Abort::External, 1);
    CHECK_EQ(p.begin(95, 200, 0, ok) == Abort::None, 1);
    CHECK_EQ(p.stop(Abort::External), 0);
    CHECK_EQ(p.poll(1, ok), 0);
    return 0;
}
