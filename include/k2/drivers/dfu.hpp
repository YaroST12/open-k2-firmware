// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Entering the DFU bootloader from the running application, without a button.
//
// The bootloader decides at reset whether to run the application. enter() invalidates one
// marker word in the app's meta page (k2::flash, allow-list of exactly that word), verifies it
// and takes a system reset; the bootloader then stays in DFU. Flashing rewrites the whole page,
// so the marker comes back with the next firmware. Holding MINUS while plugging in USB remains
// the fallback.
#pragma once
#include <concepts>
#include <string_view>

#include "k2/drivers/sysreset.hpp"

namespace k2::dfu {

// How the final reset is taken (sys = SYSRESETREQ, iwdg = watchdog); chosen per DFU_ENTER.
using ResetMode = sysreset::Mode;

// Environment of the sequence (real one in src/drivers/dfu.cpp, a fake in the host tests).
template <class E>
concept Env = requires(E& e, std::string_view msg) {
    e.say(msg);                         // log a line
    e.flush();                          // let queued USB log reports leave
    { e.clear_marker() } -> std::same_as<bool>;
    { e.marker_cleared() } -> std::same_as<bool>;   // readback: the app signature is gone
    e.dump_ram();                       // log a few SRAM windows (flushes itself)
    e.park();                           // external chips and the LCD into a power-on-like state
    e.detach();                         // drop off the USB bus so the host sees a disconnect
    e.reset(ResetMode::Sys);            // wipe SRAM, then system reset or watchdog reset
};

// announce -> flush -> clear -> verify readback -> flush -> RAM dump -> park chips -> flush ->
// USB detach -> reset. Without the
// detach the host never notices the reset (same VID/PID, a few ms gap) and keeps talking to the
// bootloader with the app's enumeration state. Returns false (and never resets) when
// the clear fails or the readback still shows the marker; the app then keeps running.
template <Env E>
bool enter_sequence(E& e, ResetMode mode = ResetMode::Sys) {
    e.say("[DFU] entering bootloader\n");
    e.flush();
    if (!e.clear_marker()) {
        e.say("[DFU] marker clear failed, staying in the app\n");
        return false;
    }
    if (!e.marker_cleared()) {
        e.say("[DFU] marker readback failed, staying in the app\n");
        return false;
    }
    e.say(mode == ResetMode::Iwdg ? "[DFU] marker cleared, resetting (iwdg)\n"
                                  : "[DFU] marker cleared, resetting (sys)\n");
    e.flush();
    e.dump_ram();
    e.park();
    e.say("[DFU] chips parked, detaching\n");
    e.flush();
    e.detach();
    e.reset(mode);
    return true;
}

// Runs the sequence on the real hardware. Returns only on failure.
bool enter(ResetMode mode = ResetMode::Sys);

}  // namespace k2::dfu
