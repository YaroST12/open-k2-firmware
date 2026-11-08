// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// See k2/core/fault.hpp.
#include "k2/core/fault.hpp"

#include "k2/drivers/crm.hpp"
#include "k2/core/fault_calc.hpp"
#include "k2/core/log.hpp"
#include "k2/drivers/mpu.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/sysreset.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

// Linker-script symbols (link.ld): the MPU guard under the application stack.
extern "C" std::uint32_t _sguard, _eguard;

namespace k2::fault {

namespace {

bool g_in_fault;

Guard stack_guard() {
    return {reinterpret_cast<std::uint32_t>(&_sguard), reinterpret_cast<std::uint32_t>(&_eguard)};
}

void report(Kind kind, const std::uint32_t* frame) {
    using namespace reg;
    Scb& s = scb();
    kind = classify(kind, s.CFSR, s.MMFAR, stack_guard());
    Info i{kind, frame[6], frame[5], frame[7], s.CFSR, s.HFSR, s.BFAR, s.MMFAR,
           reinterpret_cast<std::uint32_t>(frame)};
    char line[64];
    usb::log({line, line_regs(i, line, sizeof line)});
    usb::log({line, line_status(i, line, sizeof line)});
    // Interrupts cannot run here: drive the USB stack by hand so the lines leave the device.
    for (int n = 0; n < 80; n++) {
        usb::pump();
        systick::delay_us(1000);
    }
}

}  // namespace

// [[gnu::used]]: referenced only from the asm entry stubs below, LTO must keep it.
// The handler runs on MSP (link.ld .msp_stack), not on the stack that overflowed. The MPU goes off first: the frame
// (or the place it should have been written) can sit inside the guard, and the report reads it.
extern "C" [[noreturn, gnu::used]] void k2_fault_main(const std::uint32_t* frame, std::uint32_t kind) {
    __asm volatile("cpsid i" ::: "memory");
    mpu::disable();
    if (!g_in_fault) {
        g_in_fault = true;
        report(static_cast<Kind>(kind), frame);
        usb::detach();
        crm::deinit();
    }
    reg::scb().AIRCR = reg::Scb::AIRCR_SYSRESET;
    for (;;) __asm volatile("dsb");
}

void init() {
    reg::scb().SHCSR |= (1u << 16) | (1u << 17) | (1u << 18);  // MEMFAULTENA, BUSFAULTENA, USGFAULTENA
    const Guard g = stack_guard();
    const bool on = mpu::guard(g.lo, g.hi - g.lo);              // no-access region under the application stack
    K2LOG("[MPU] stack guard %08X..%08X %s\n", static_cast<unsigned>(g.lo), static_cast<unsigned>(g.hi),
          on ? "on" : "NOT ACTIVE");
}

namespace {
// Runaway recursion for trigger(2): each level burns about 80 bytes of stack until the guard faults. The volatile
// counter keeps the compiler from turning it into a loop or proving it infinite.
[[gnu::noinline]] std::uint32_t recurse(volatile std::uint32_t* depth) {
    volatile std::uint32_t pad[16];
    pad[0] = *depth;
    *depth = pad[0] + 1;
    if (*depth == 0xFFFFFFFFu) return pad[0];
    return recurse(depth) + pad[0];
}
}  // namespace

void trigger(std::uint8_t kind) {
    if (kind == 0) {
        __asm volatile(".short 0xDE00");   // udf #0
    } else if (kind == 2) {
        volatile std::uint32_t depth = 0;
        (void)recurse(&depth);
    } else {
        volatile std::uint32_t sink = *reinterpret_cast<volatile std::uint32_t*>(0x20037FC0);
        (void)sink;
    }
}

}  // namespace k2::fault

// Exception entry: pick the stack the fault frame is on (EXC_RETURN bit 2), pass it and the
// kind to k2_fault_main. Strong definitions replace the weak Default_Handler aliases.
#define K2_FAULT_ENTRY(name, kind)                                                          \
    extern "C" __attribute__((naked)) void name() {                                         \
        __asm volatile("tst lr, #4\n"                                                       \
                       "ite eq\n"                                                           \
                       "mrseq r0, msp\n"                                                    \
                       "mrsne r0, psp\n"                                                    \
                       "movs r1, %0\n"                                                      \
                       "movw r2, #:lower16:k2_fault_main\n"                                \
                       "movt r2, #:upper16:k2_fault_main\n"                                \
                       "bx r2\n" ::"i"(kind));                                              \
    }
K2_FAULT_ENTRY(NMI_Handler, 0)
K2_FAULT_ENTRY(HardFault_Handler, 1)
K2_FAULT_ENTRY(MemManage_Handler, 2)
K2_FAULT_ENTRY(BusFault_Handler, 3)
K2_FAULT_ENTRY(UsageFault_Handler, 4)
