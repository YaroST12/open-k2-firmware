// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Clock tree bring-up. The reset masks reproduce the Artery SDK's two-step
// "crm_reset + system_clock_config" so the PLL starts from a known state.
#include "k2/drivers/crm.hpp"

#include <cstdint>

#include "k2/core/critical.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::crm {

namespace {
std::uint32_t g_entry_trim;   // CTRL bits 15:2 as found at entry (reset / bootloader hand-over state)
std::uint8_t g_div = 1;       // the AHB divider in force (init() and deinit() leave it at 1)
Listener g_listeners[kMaxListeners];
unsigned g_n_listeners;
}

std::uint8_t hclk_div() { return g_div; }
std::uint32_t hclk_hz() { return hclk_hz_for(g_div); }
std::uint32_t apb1_hz() { return apb1_hz_for(g_div); }
std::uint32_t timer_hz() { return timer_hz_for(g_div); }

void on_hclk_change(Listener fn) {
    for (unsigned i = 0; i < g_n_listeners; i++) {
        if (g_listeners[i] == fn) return;
    }
    if (g_n_listeners < kMaxListeners) g_listeners[g_n_listeners++] = fn;
}

bool set_hclk_div(std::uint8_t n) {
    if (!hclk_div_valid(n)) return false;
    if (n == g_div) return true;
    systick::wait_reload();   // land on a millisecond edge so SysTick's period is whole in either clock
    {
        Critical cs;   // the divider, the time base and the bookkeeping change as one step
        reg::Crm& c = reg::crm();
        c.CFG = (c.CFG & ~reg::Crm::CFG_AHBDIV_MASK) | (ahbdiv_code(n) << reg::Crm::CFG_AHBDIV_SHIFT);
        g_div = n;
        systick::set_div(n);
    }
    for (unsigned i = 0; i < g_n_listeners; i++) g_listeners[i]();
    return true;
}

void init() {
    using namespace reg;
    Crm& c = reg::crm();
    g_entry_trim = c.CTRL & Crm::CTRL_TRIM_MASK;

    scb().CPACR |= Scb::CPACR_FPU_FULL;  // also done in Reset_Handler; harmless to repeat

    // Back to the reset state: HICK on and selected, no PLL, no HEXT, no clock interrupts.
    c.CTRL |= Crm::CTRL_HICKEN;
    c.CFG &= 0xE8FF000C;
    c.CTRL &= 0xFEF6FFFF;               // HEXT and PLL off
    c.CTRL &= ~Crm::CTRL_HEXTBYPS;
    c.CFG &= 0x1700FFFF;
    c.MISC1 &= 0xFEFEFF00;
    c.INT = 0x009F0000;                 // clear all clock interrupt flags

    // Bus dividers for 240 MHz: APB2 /2 = 120 MHz, APB1 /2 = 120 MHz (SPI3, USART2 sit on APB1). The constant
    // was once named APB1DIV_4 and everything on APB1 assumed 60 MHz: USART2 ran at twice its baud (UFCS/VIVO
    // never answered) until 2026-11-07; the LCD has always run at the 120 MHz rates.
    c.CFG = (c.CFG & ~Crm::CFG_APB2DIV_MASK) | Crm::CFG_APB2DIV_2;
    c.CFG = (c.CFG & ~Crm::CFG_APB1DIV_MASK) | Crm::CFG_APB1DIV_2;

    // 8 MHz crystal on the K2. Give it a bounded time to start.
    c.CTRL |= Crm::CTRL_HEXTEN;
    for (std::uint32_t timeout = 0x3000; !(c.CTRL & Crm::CTRL_HEXTSTBL) && --timeout;) {}

    // PLL input is always 4 MHz (source / 2), multiplier 60 -> 240 MHz.
    //   0xA0370000: PLLRCS=HEXT (bit 16), HEXT/2 (bit 17), mult field 13 at bits 21:18
    //               with bits 31/29 as the extended-multiplier bits.
    //   0xA0360000: the same with HICK/2 as the source (bit 16 clear).
    const std::uint32_t pll = (c.CTRL & Crm::CTRL_HEXTSTBL) ? 0xA0370000 : 0xA0360000;
    c.CFG = (c.CFG & 0x1FC0FFFF) | pll;

    c.CTRL |= Crm::CTRL_PLLEN;
    while (!(c.CTRL & Crm::CTRL_PLLSTBL)) {}

    c.CFG = (c.CFG & ~Crm::CFG_SCLKSEL_MASK) | Crm::CFG_SCLKSEL_PLL;
    while (((c.CFG >> Crm::CFG_SCLKSTS_SHIFT) & 0x3) != Crm::CFG_SCLKSEL_PLL) {}
}

void deinit() {
    using namespace reg;
    Crm& c = reg::crm();

    // 1. Run from HICK again before touching the PLL.
    c.CTRL |= Crm::CTRL_HICKEN;
    for (std::uint32_t t = 0x30000; !(c.CTRL & Crm::CTRL_HICKSTBL) && --t;) {}
    c.CFG &= ~Crm::CFG_SCLKSEL_MASK;
    for (std::uint32_t t = 0x30000; ((c.CFG >> Crm::CFG_SCLKSTS_SHIFT) & 0x3) != 0 && --t;) {}

    // 2. PLL and HEXT off, dividers / USBDIV / PLL source cleared (CFG reset value is 0).
    c.CTRL &= ~(Crm::CTRL_PLLEN | Crm::CTRL_HEXTEN | Crm::CTRL_HEXTBYPS);
    c.CFG = 0;
    g_div = 1;                           // CFG = 0 cleared the AHB divider with the rest
    c.INT = 0x009F0000;                  // clear clock interrupt flags

    // 3. USB clock path: HICK-to-USB gates closed, USB core clock off.
    c.MISC1 &= ~(1u << 25);
    c.MISC2 &= ~(1u << 8);
    c.APB1EN &= ~Crm::APB1EN_USB;

    // 4. ACC off (needs its APB2 clock while the registers are written), then its clock off.
    c.APB2EN |= Crm::APB2EN_ACC;
    acc().CTRL1 = 0;
    acc().C1 = 0;
    acc().C2 = 0;
    acc().C3 = 0;
    c.APB2EN &= ~Crm::APB2EN_ACC;

    // 5. HICK trim/calibration back to the entry values (the ACC had steered them for 48 MHz).
    c.CTRL = (c.CTRL & ~Crm::CTRL_TRIM_MASK) | g_entry_trim;
}

}  // namespace k2::crm
