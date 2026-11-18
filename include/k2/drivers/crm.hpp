// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Clock tree bring-up: 8 MHz HEXT (HICK fallback) -> PLL -> 240 MHz SYSCLK.
#pragma once
#include <cstdint>

namespace k2::crm {

// The AHB divider: HCLK = 240 MHz / n. APB1 and APB2 stay at /2 of HCLK, and a timer on a /2 bus runs at twice the bus
// clock, which is HCLK again. The USB core's 48 MHz comes from the PLL through USBDIV and does not move with it.
// Valid dividers are 1, 2, 4 and 8: at /16 APB1 would be 7.5 MHz, under the 8 MHz the USB register interface needs
// (inferred from the STM32F1-family rule; not measured on this part).
inline constexpr std::uint32_t kHclkFullHz = 240'000'000;
constexpr bool hclk_div_valid(unsigned n) { return n == 1 || n == 2 || n == 4 || n == 8; }
constexpr unsigned ahbdiv_code(unsigned n) { return n == 1 ? 0u : n == 2 ? 8u : n == 4 ? 9u : 10u; }   // CFG bits 7:4
constexpr std::uint32_t hclk_hz_for(unsigned n) { return kHclkFullHz / n; }
constexpr std::uint32_t apb1_hz_for(unsigned n) { return hclk_hz_for(n) / 2; }
constexpr std::uint32_t timer_hz_for(unsigned n) { return hclk_hz_for(n); }

// The single source of truth for the divider: every driver that derives a rate from the core, bus or timer clock reads
// these (or registers a listener, below) instead of assuming 240 / 120 MHz.
std::uint8_t hclk_div();
std::uint32_t hclk_hz();
std::uint32_t apb1_hz();
std::uint32_t timer_hz();

// Switch the AHB divider (1, 2, 4 or 8; false for anything else, true when already there). SysTick is re-programmed in
// the same step, aligned to a millisecond edge, so millis(), micros() and ticks() keep running in real time; then every
// listener runs once. Call it from the main loop with interrupts enabled. It does not check what is running: DWT cycle
// timing (the D+/D- engines), a running one-shot or load pulse and the LCD stream must be idle (the idle glue,
// src/core/idle_hw.cpp, refuses a raise while any of them is).
bool set_hclk_div(std::uint8_t n);

// A driver that programmed a clock-derived value (timer prescaler, baud divisor) registers a function that re-programs it
// from the new divider. Up to kMaxListeners; registering the same function twice is a no-op.
inline constexpr unsigned kMaxListeners = 6;
using Listener = void (*)();
void on_hclk_change(Listener fn);

// Run once, first thing in main(). Leaves SYSCLK 240 MHz, APB2 120 MHz, APB1 120 MHz.
void init();

// Undo everything the app changed in the clock/USB-clock registers so a system reset hands the
// bootloader the state it saw at power-on: SYSCLK back on HICK, PLL and HEXT off, bus
// dividers and USBDIV cleared, the HICK-to-USB gates (MISC1 bit 25, MISC2 bit 8) closed, ACC
// off with its limits cleared, USB clock off, HICK trim/calibration bits restored to what init()
// found at entry. Call with interrupts off, after usb::detach(); nothing may depend on the
// 240 MHz clock afterwards (delays, USB, LCD). Bounded waits, no hang.
void deinit();

}  // namespace k2::crm

