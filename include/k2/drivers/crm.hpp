// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Clock tree bring-up: 8 MHz HEXT (HICK fallback) -> PLL -> 240 MHz SYSCLK.
#pragma once

namespace k2::crm {

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

