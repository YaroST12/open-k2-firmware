// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The K2_STACKPROBE=1 PSP probe (include/k2/core/stack_probe.hpp). Empty in every other build.
#include "k2/core/stack_probe.hpp"

#if K2_STACKPROBE
#include "k2/core/critical.hpp"
#include "k2/core/hot.h"

extern "C" std::uint32_t _eguard, _psp_top;

namespace k2::stackprobe {

namespace {
Low g_low;                   // written by SysTick only
std::uint32_t g_reported;    // the sp take() last handed out (main loop only)

std::uint32_t bottom() { return reinterpret_cast<std::uint32_t>(&_eguard); }
std::uint32_t top() { return reinterpret_cast<std::uint32_t>(&_psp_top); }
}  // namespace

// SysTick (handler mode, MSP): the PSP is the thread's stack pointer with the interrupted frame on it (r0-r3, r12, lr, pc,
// xPSR from the bottom up). A SysTick that preempted another ISR finds the thread's frame there too.
K2_HOT void sample() {
    std::uint32_t sp;
    asm volatile("mrs %0, psp" : "=r"(sp));
    if (!in_frame(sp, bottom(), top()) || (g_low.sp != 0 && sp >= g_low.sp)) return;
    const auto* frame = reinterpret_cast<const volatile std::uint32_t*>(sp);
    note(g_low, sp, bottom(), top(), frame[6], frame[5]);
}

bool take(Low& out, std::uint32_t& free_bytes) {
    {
        const Critical c;
        out = g_low;
    }
    if (out.sp == 0 || out.sp == g_reported) return false;
    g_reported = out.sp;
    free_bytes = free_below(out, bottom());
    return true;
}

}  // namespace k2::stackprobe
#endif
