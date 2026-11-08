// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// See k2/drivers/sysreset.hpp.
#include "k2/drivers/sysreset.hpp"

#include "k2/drivers/mpu.hpp"
#include "k2/drivers/regs.hpp"

namespace k2::sysreset {

namespace {
std::uint32_t g_flags;
bool g_captured;
}  // namespace

void capture_flags() {
    if (g_captured) return;
    g_flags = reg::crm().CTRLSTS;
    g_captured = true;
    reg::crm().CTRLSTS |= reg::Crm::CTRLSTS_RSTFC;
}

std::uint32_t reset_flags() { return g_captured ? g_flags : reg::crm().CTRLSTS; }

[[noreturn]] void wipe_and_reset(Mode mode) {
    using namespace reg;
    __asm volatile("cpsid i" ::: "memory");
    mpu::disable();                             // the loop below zeroes the stack guard along with the rest of SRAM
    scb().CCR |= 1u << 8;                      // BFHFNMIGN: bus faults are ignored at FAULTMASK
    __asm volatile("cpsid f" ::: "memory");
    // From here nothing may touch the stack: the loop below zeroes it. All state lives in
    // registers; r0..r2 and r4 are scratch, `mode` arrives in a register chosen by the compiler.
    __asm volatile(
        "movw r0, #0x0000\n"
        "movt r0, #0x2000\n"                    // r0 = 0x20000000
        "movw r1, #0x8000\n"
        "movt r1, #0x2001\n"                    // r1 = 0x20018000
        "movs r2, #0\n"
        "1:\n"
        "str r2, [r0], #4\n"
        "cmp r0, r1\n"
        "bne 1b\n"
        "cmp %[mode], #0\n"
        "bne 3f\n"
        // Sys: AIRCR = 0x05FA0004
        "movw r0, #0xED0C\n"
        "movt r0, #0xE000\n"
        "movw r1, #0x0004\n"
        "movt r1, #0x05FA\n"
        "str r1, [r0]\n"
        "dsb\n"
        "2: b 2b\n"
        "3:\n"
        // Iwdg: base 0x40003000. Start (also enables LSI), unlock, PR = /4, RLR = 1, wait for the
        // update (bounded; the reset value 0xFFF would still fire after ~0.4 s), reload, spin.
        "movw r0, #0x3000\n"
        "movt r0, #0x4000\n"
        "movw r1, #0xCCCC\n"
        "str r1, [r0]\n"
        "movw r1, #0x5555\n"
        "str r1, [r0]\n"
        "movs r1, #0\n"
        "str r1, [r0, #4]\n"
        "movs r1, #1\n"
        "str r1, [r0, #8]\n"
        "movw r4, #0x0000\n"
        "movt r4, #0x0004\n"                    // 0x40000 polls
        "4:\n"
        "ldr r1, [r0, #12]\n"
        "cbz r1, 5f\n"
        "subs r4, r4, #1\n"
        "bne 4b\n"
        "5:\n"
        "movw r1, #0xAAAA\n"
        "str r1, [r0]\n"
        "6: b 6b\n"
        :
        : [mode] "r"(static_cast<std::uint32_t>(mode))
        : "r0", "r1", "r2", "r4", "memory", "cc");
    __builtin_unreachable();
}

}  // namespace k2::sysreset
