// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// SysTick sample -> ticks/micros composition, around the reload wrap.
#include "k2/drivers/systick.hpp"
#include <stdint.h>
#include "test_util.h"

using namespace k2::systick;

static void test_mid_millisecond() {
    // VAL counts down from kReload-1: 240 ticks elapsed = 1 us.
    const Sample s{7, kReload - 1 - 240, false};
    CHECK_EQ(micros_of(s), 7001);
    CHECK_EQ(ticks_of(s), 7 * kReload + 240);
}

static void test_wrap_not_yet_counted() {
    // Interrupts masked (or a higher-priority ISR): VAL wrapped, the handler has not run, so
    // g_millis is still 7 and the pending flag is set. The time is 8 ms, not 7 ms.
    const Sample before{7, 0, false};                  // last tick of ms 7
    const Sample after{7, kReload - 1 - 240, true};    // 1 us into ms 8, wrap pending
    CHECK_EQ(micros_of(before), 7999);
    CHECK_EQ(micros_of(after), 8001);
    CHECK_EQ(micros_of(after) > micros_of(before), 1);       // never goes backwards
    CHECK_EQ(ticks_of(after), 8 * kReload + 240);
}

int main() {
    test_mid_millisecond();
    test_wrap_not_yet_counted();
    return test_report("test_systick");
}
