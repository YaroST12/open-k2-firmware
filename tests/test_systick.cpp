// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// SysTick sample -> ticks/micros composition, around the reload wrap.
#include "k2/drivers/systick.hpp"
#include <stdint.h>
#include <initializer_list>
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

// With the AHB divider raised the core clock (and SysTick, which counts it) runs at 240 MHz / n. The reload shrinks to
// kReload / n counts per millisecond, and ticks() / micros() stay in nominal 240 MHz units: one count is n nominal ticks.
static void test_divided_clock_keeps_nominal_units() {
    for (const std::uint32_t div : {2u, 4u, 8u}) {
        const std::uint32_t reload = reload_for(div);
        CHECK_EQ(reload * div, kReload);
        const Sample s{7, reload - 1 - 240 / div, false};          // 240 nominal ticks = 1 us after the edge
        CHECK_EQ(micros_of(s, div), 7001);
        CHECK_EQ(ticks_of(s, div), 7 * kReload + 240);
        const Sample before{7, 0, false};
        const Sample after{7, reload - 1 - 240 / div, true};       // wrap pending, 1 us into ms 8
        CHECK_EQ(micros_of(before, div), 7000 + (reload - 1) * div / 240);
        CHECK_EQ(micros_of(after, div), 8001);
        CHECK_EQ(micros_of(after, div) > micros_of(before, div), 1);
        CHECK_EQ(ticks_of(after, div), 8 * kReload + 240);
    }
    // div 1 is the old formula
    const Sample s{3, 1000, false};
    CHECK_EQ(ticks_of(s, 1), ticks_of(s));
    CHECK_EQ(micros_of(s, 1), micros_of(s));
}

int main() {
    test_mid_millisecond();
    test_wrap_not_yet_counted();
    test_divided_clock_keeps_nominal_units();
    return test_report("test_systick");
}
