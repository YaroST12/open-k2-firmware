// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The K2_STACKPROBE=1 PSP probe's record (include/k2/core/stack_probe.hpp): SysTick hands it the thread stack pointer and
// the interrupted PC / LR, it keeps the lowest stack pointer inside the PSP stack and says when a new low came in.
#include "k2/core/stack_probe.hpp"
#include "test_util.h"

using namespace k2::stackprobe;

constexpr std::uint32_t kBottom = 0x20015C80, kTop = 0x20017AC0;   // _eguard, _psp_top of the shipping link

static void test_the_first_sample_inside_the_stack_is_a_low() {
    Low l;
    CHECK_COND(note(l, kTop - 0x400, kBottom, kTop, 0x08012345, 0x08023457));
    CHECK_EQ(l.sp, kTop - 0x400);
    CHECK_EQ(l.pc, 0x08012345u);
    CHECK_EQ(l.lr, 0x08023457u);
}

static void test_only_a_deeper_sample_replaces_the_low() {
    Low l;
    note(l, kTop - 0x400, kBottom, kTop, 1, 2);
    CHECK_COND(!note(l, kTop - 0x200, kBottom, kTop, 3, 4));   // shallower
    CHECK_COND(!note(l, kTop - 0x400, kBottom, kTop, 5, 6));   // the same depth: the first one stays
    CHECK_EQ(l.pc, 1u);
    CHECK_COND(note(l, kTop - 0x1000, kBottom, kTop, 7, 8));
    CHECK_EQ(l.sp, kTop - 0x1000);
    CHECK_EQ(l.pc, 7u);
    CHECK_EQ(l.lr, 8u);
}

static void test_a_stack_pointer_outside_the_psp_stack_is_ignored() {
    Low l;
    CHECK_COND(!in_frame(0, kBottom, kTop));                    // before the switch to the PSP
    CHECK_COND(!in_frame(kBottom - 4, kBottom, kTop));          // in the MPU guard: reading it would fault
    CHECK_COND(!in_frame(kTop - 16, kBottom, kTop));            // no room for the 8-word exception frame
    CHECK_COND(!in_frame(kTop - 0x402, kBottom, kTop));         // not word aligned
    CHECK_COND(in_frame(kBottom, kBottom, kTop));
    CHECK_COND(in_frame(kTop - 32, kBottom, kTop));
    CHECK_COND(!note(l, kBottom - 4, kBottom, kTop, 1, 2));
    CHECK_EQ(l.sp, 0u);
}

static void test_free_bytes_count_from_the_guard() {
    Low l;
    note(l, kBottom + 2600, kBottom, kTop, 1, 2);
    CHECK_EQ(free_below(l, kBottom), 2600u);
}

int main() {
    test_the_first_sample_inside_the_stack_is_a_low();
    test_only_a_deeper_sample_replaces_the_low();
    test_a_stack_pointer_outside_the_psp_stack_is_ignored();
    test_free_bytes_count_from_the_guard();
    return test_report("test_stack_probe");
}
