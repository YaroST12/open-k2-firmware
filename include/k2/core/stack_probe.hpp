// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PSP probe of a K2_STACKPROBE=1 build (off by default). SysTick reads the thread stack pointer (where the exception
// stacked the interrupted frame on the PSP) and keeps the lowest one with the interrupted PC and LR; the main loop logs each
// new low as "[STK] low=<free> pc=0x.. lr=0x..", so a hardware run names the code that was running at its deepest sample
// (arm-none-eabi-addr2line -f -C -e build/open_k2.elf <pc> <lr>). A 1 kHz sample, not the painted low-water mark: a peak
// shorter than a millisecond can pass between two samples, so [STK] low reads at or above the heartbeat's stack_free.
#pragma once
#include <cstdint>

namespace k2::stackprobe {

struct Low {
    std::uint32_t sp = 0;   // 0 = nothing sampled yet
    std::uint32_t pc = 0;
    std::uint32_t lr = 0;
};

// True when an 8-word exception frame at sp lies inside the PSP stack [bottom, top) and may be read (outside it the PSP is
// not in use yet, or it points into the MPU guard, which faults).
constexpr bool in_frame(std::uint32_t sp, std::uint32_t bottom, std::uint32_t top) {
    return (sp & 3u) == 0 && sp >= bottom && sp <= top && top - sp >= 32;
}

// One sample: a stack pointer inside the stack and below the lowest so far replaces the record. True for a new low.
constexpr bool note(Low& low, std::uint32_t sp, std::uint32_t bottom, std::uint32_t top, std::uint32_t pc, std::uint32_t lr) {
    if (!in_frame(sp, bottom, top) || (low.sp != 0 && sp >= low.sp)) return false;
    low = {sp, pc, lr};
    return true;
}

// The bytes left between the guard (bottom) and the lowest sample.
constexpr std::uint32_t free_below(const Low& low, std::uint32_t bottom) { return low.sp - bottom; }

// Firmware, K2_STACKPROBE=1 only: SysTick_Handler calls sample(); the main loop calls take(), which hands out each new low
// once (false when nothing deeper came since the last call).
void sample();
bool take(Low& out, std::uint32_t& free_bytes);

}  // namespace k2::stackprobe
