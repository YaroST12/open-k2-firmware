// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Start-up: vector table, bootloader meta objects and Reset_Handler.
//
// The bootloader (0x08000000..0x08003800) validates what sits at fixed addresses
// in the application area, so every object below carries a section name that link.ld
// pins to its address. Do not rename the sections or the unmangled symbols.
#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/drivers/regs.hpp"

using Handler = void (*)();

// Linker-script symbols (link.ld).
extern "C" {
extern std::uint32_t _estack, _psp_top, _eguard, _sidata, _sdata, _edata, _sbss, _ebss;
extern Handler __preinit_array_start[], __preinit_array_end[];
extern Handler __init_array_start[], __init_array_end[];
int main();

void Reset_Handler();
void Default_Handler();

// Core exceptions and the peripheral IRQs with handlers elsewhere. Anything not defined
// by another translation unit falls back to Default_Handler (weak alias).
#define K2_WEAK_HANDLER(name) void name() __attribute__((weak, alias("Default_Handler")))
K2_WEAK_HANDLER(NMI_Handler);
K2_WEAK_HANDLER(HardFault_Handler);
K2_WEAK_HANDLER(MemManage_Handler);
K2_WEAK_HANDLER(BusFault_Handler);
K2_WEAK_HANDLER(UsageFault_Handler);
K2_WEAK_HANDLER(SVC_Handler);
K2_WEAK_HANDLER(DebugMon_Handler);
K2_WEAK_HANDLER(PendSV_Handler);
K2_WEAK_HANDLER(SysTick_Handler);
K2_WEAK_HANDLER(USB_HP_CAN1_TX_IRQHandler);
K2_WEAK_HANDLER(USBWakeUp_IRQHandler);
K2_WEAK_HANDLER(EXTI15_10_IRQHandler);
K2_WEAK_HANDLER(EXTI2_IRQHandler);
K2_WEAK_HANDLER(EXTI9_5_IRQHandler);
#undef K2_WEAK_HANDLER
void USB_LP_CAN1_RX0_IRQHandler();  // defined in usb.cpp
}

// 76 entries (0x130 bytes) at the flash origin 0x08003800. Entry 0 is the initial stack
// pointer, entry 16 + n is external IRQ n. Unused slots point to Default_Handler.
namespace {
constexpr std::size_t kExceptions = 16, kIrqs = 60;

struct VectorTable {
    const void* initial_sp;  // a data address: no Thumb bit, so not a Handler
    std::array<Handler, kExceptions - 1 + kIrqs> v;
};

constexpr auto make_vectors() {
    std::array<Handler, kExceptions - 1 + kIrqs> v{};
    v.fill(Default_Handler);
    v[0] = Reset_Handler;
    v[1] = NMI_Handler;
    v[2] = HardFault_Handler;
    v[3] = MemManage_Handler;
    v[4] = BusFault_Handler;
    v[5] = UsageFault_Handler;
    for (std::size_t i : {6, 7, 8, 9, 12}) v[i] = nullptr;  // reserved
    v[10] = SVC_Handler;
    v[11] = DebugMon_Handler;
    v[13] = PendSV_Handler;
    v[14] = SysTick_Handler;
    v[kExceptions - 1 + 19] = USB_HP_CAN1_TX_IRQHandler;   // IRQ 19
    v[kExceptions - 1 + 20] = USB_LP_CAN1_RX0_IRQHandler;  // IRQ 20
    v[kExceptions - 1 + 8] = EXTI2_IRQHandler;             // IRQ 8: D- (PB2) edge, k2/fastcharge/dpsniff.hpp
    v[kExceptions - 1 + 23] = EXTI9_5_IRQHandler;          // IRQ 23: D+ (PA7) edge, k2/fastcharge/dpsniff.hpp
    v[kExceptions - 1 + 40] = EXTI15_10_IRQHandler;        // IRQ 40: FUSB302 INT_N (EXTI12)
    v[kExceptions - 1 + 42] = USBWakeUp_IRQHandler;        // IRQ 42
    return v;
}
}  // namespace

extern "C" __attribute__((section(".isr_vector"), used))
const VectorTable g_pfnVectors = {&_estack, make_vectors()};
static_assert(sizeof(VectorTable) == 0x130);

// Meta area read by the bootloader. `extern` gives these C++ consts external linkage.
extern "C" {
// Firmware version word at 0x08004000 (0x10 = v1.0).
__attribute__((section(".meta_version"), used))
extern const std::uint32_t k2_version_marker = 0x00000010;

// Hardware model magic at 0x08004010: "K200", padded to 8 bytes.
__attribute__((section(".meta_model"), used))
extern const char k2_model_signature[8] = {'K', '2', '0', '0', 0, 0, 0, 0};

// Commit slot at 0x08004400, left erased (0xFFFFFFFF) for the packaging tool to fill in.
__attribute__((section(".meta_commit"), used))
extern const std::uint32_t k2_commit_slot = 0xFFFFFFFF;

constexpr std::uint32_t kStackPaint = 0xA5A5A5A5;

// Bytes of painted application (PSP) stack never touched since reset, counted up from the MPU guard
// (link.ld .stack_guard). 0 means the stack reached the guard: it overflowed (the MPU traps that).
std::uint32_t k2_stack_free_bytes() {
    const std::uint32_t* p = &_eguard;
    while (p < &_psp_top && *p == kStackPaint) p++;
    return static_cast<std::uint32_t>(p - &_eguard) * 4;
}

// The same for the exception (MSP) stack: ISRs and the [FAULT] handler. It is painted from _psp_top up.
std::uint32_t k2_msp_free_bytes() {
    const std::uint32_t* p = &_psp_top;
    while (p < &_estack && *p == kStackPaint) p++;
    return static_cast<std::uint32_t>(p - &_psp_top) * 4;
}

[[gnu::used, gnu::noinline]] void k2_reset_main();   // named in Reset_Handler's asm: keep it out of LTO's reach

// Reset entry: the application runs in thread mode on PSP (link.ld: .psp_stack), so exceptions, including the
// fault raised when PSP overflows into the MPU guard, run on MSP, a stack the overflow cannot have damaged.
// No C code runs before the switch.
[[gnu::naked]] void Reset_Handler() {
    __asm volatile(
        "ldr r0, =_psp_top\n"
        "msr psp, r0\n"
        "movs r0, #2\n"        // CONTROL.SPSEL = 1, still privileged
        "msr control, r0\n"
        "isb\n"
        "bl k2_reset_main\n"
        "b .\n");
}

[[gnu::used, gnu::noinline]] void k2_reset_main() {
    using namespace k2::reg;

    scb().VTOR = 0x08003800;                 // our table, not the bootloader's
    scb().CPACR |= Scb::CPACR_FPU_FULL;      // the FPU must be on before any float code runs

    // The bootloader may leave interrupts enabled or pending; start from a clean NVIC.
    for (int i = 0; i < 8; i++) {
        nvic().ICER[i] = 0xFFFFFFFF;
        nvic().ICPR[i] = 0xFFFFFFFF;
    }

    // C runtime: .data from flash, then zero .bss. Constructors need both, so they run last.
    const std::uint32_t* src = &_sidata;
    for (std::uint32_t* dst = &_sdata; dst < &_edata;) *dst++ = *src++;
    for (std::uint32_t* dst = &_sbss; dst < &_ebss;) *dst++ = 0;

    // Stack watermarks: paint the free PSP stack below the current frame, and the whole (still unused) MSP stack.
    {
        std::uint32_t* sp;
        __asm volatile("mov %0, sp" : "=r"(sp));
        for (std::uint32_t* p = &_eguard; p + 16 < sp;) *p++ = kStackPaint;
        for (std::uint32_t* p = &_psp_top; p < &_estack;) *p++ = kStackPaint;
    }

    for (Handler* f = __preinit_array_start; f < __preinit_array_end; f++) (*f)();
    for (Handler* f = __init_array_start; f < __init_array_end; f++) (*f)();

    main();
    for (;;) __asm volatile("wfi");          // main never returns; park if it does
}

void Default_Handler() {
    for (;;) __asm volatile("nop");
}
}
