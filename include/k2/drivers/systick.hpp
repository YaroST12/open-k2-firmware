// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// 1 ms SysTick time base and busy-wait delays.
#pragma once
#include <cstdint>

namespace k2::systick {

inline constexpr std::uint32_t kSysclkHz = 240'000'000;
inline constexpr std::uint32_t kReload = kSysclkHz / 1000;  // ticks per millisecond

void init();

// One consistent reading of the time base: the millisecond count, SysTick VAL, and whether the
// SysTick interrupt was pending (VAL wrapped but the handler has not counted it yet, as with
// interrupts masked or inside a higher-priority ISR). When pending, `val` was re-read after the
// flag was seen, so it belongs to millisecond ms + 1.
struct Sample {
    std::uint32_t ms, val;
    bool pending;
};
// SysTick counts the core clock, which the AHB divider (crm::set_hclk_div) can lower to 240 MHz / div: the reload then
// shrinks to kReload / div counts per millisecond. ticks() and micros() stay in nominal 240 MHz units whatever the divider
// (one count is `div` nominal ticks), so every caller's arithmetic and every elapsed-time measurement keeps its meaning.
constexpr std::uint32_t reload_for(std::uint32_t div) { return kReload / div; }
constexpr std::uint32_t ticks_of(Sample s, std::uint32_t div = 1) {
    return (s.ms + (s.pending ? 1u : 0u)) * kReload + (reload_for(div) - 1 - s.val) * div;
}
constexpr std::uint32_t micros_of(Sample s, std::uint32_t div = 1) {
    return (s.ms + (s.pending ? 1u : 0u)) * 1000u + (reload_for(div) - 1 - s.val) * div / (kSysclkHz / 1'000'000);
}

// The AHB divider switch, called only by crm::set_hclk_div: wait_reload() spins (bounded) until SysTick has just reloaded,
// so the switch lands within microseconds of a millisecond edge; set_div() then reloads SysTick for the new core clock.
// Both run with the divider the clock tree already has; ticks()/micros() may step back by the few microseconds the
// switch took, once.
void wait_reload();
void set_div(std::uint32_t div);
std::uint32_t div();

// Called from SysTick_Handler once per millisecond.
void tick();

// Milliseconds since init(); wraps after ~49.7 days.
std::uint32_t millis();

// Microseconds from the same clock (millis * 1000 + elapsed ticks / 240); wraps every ~71 min.
std::uint32_t micros();

// Core clock cycles since init(), wrapping modulo 2^32. kReload is a whole number of ms, so the
// wrap is seamless: subtract two readings to time an interval shorter than ~17 s.
std::uint32_t ticks();

// Busy-wait (the iteration count scales with the AHB divider, so the time stays real). delay_ms() also calls usb::service() every millisecond so that long waits
// (LCD init, e-marker scan) do not starve the USB device.
// A hook (set_delay_hook) runs once per millisecond of delay_ms() too: the boot power trace samples
// through it while the slow init steps (LCD, USB) wait. It must not call delay_ms() itself.
void set_delay_hook(void (*hook)());
// A second, permanent hook of delay_ms() (once per millisecond as well): main.cpp registers the transition
// capture's poll, so the FUSB302's blocking CC scan (about 17 ms, twice a second) leaves no hole in
// the 1 kHz timeline. Same rule: it must not call delay_ms().
void set_wait_hook(void (*hook)());
// Runs in the SysTick interrupt every millisecond (k2::load's hard timeout). Must be short and
// must not touch anything but its own registers.
void set_tick_hook(void (*hook)());
void delay_us(std::uint32_t us);
void delay_ms(std::uint32_t ms);

}  // namespace k2::systick
