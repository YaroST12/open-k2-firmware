// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The main loop's work: one pass of the service loop (USB, remote, buttons, the 1 ms PD service, the 20 ms slow tasks with
// the UI tick, the heartbeat) and the LCD idle hook that keeps PD served during a long redraw. main.cpp runs pass() forever
// with a wfi after each; the host world-replay test (tests/test_world_replay.cpp) runs the same code against a virtual
// clock, so the timing the firmware has on hardware (a ~150 ms Home repaint holding poll() back) is the timing the test sees.
#pragma once
#include <cstdint>

#include "k2/ui/ui_layout.hpp"   // Button

namespace k2 {
class Ui;
class Remote;
namespace bootscreen { class Controller; }
namespace accelerometer { class Driver; }
}  // namespace k2

namespace k2::mainloop {

// What boot() set up and the loop works on. The objects belong to main.cpp (they are built before the loop runs).
struct Context {
    Ui& ui;
    Remote& remote;
    bootscreen::Controller& bootscreen;        // the boot-info QR screen of a key-mode boot
    accelerometer::Driver& accelerometer;
    void (*reboot)();                          // the 3 s OK + Back chord or REBOOT: does not return on the device
    void (*dfu)();                             // DFU_ENTER: returns only when entering the bootloader failed
    bool boot_key_swallow = false;             // the key that chose a key-mode boot is still down: its events are not the user's
    Button boot_key = Button::Plus;            // that key (PLUS for the CC2 mode, OK for the CC1 mode)
    bool rescan_landing_due = false;           // a Rescan comeback is being judged: the UI lands on Connection -> Source once vouched
};

// The delay_ms() wait hook (the transition capture's poll) and the LCD idle hook (idle_hook below). Called once by boot().
void install_hooks();

// The LCD idle hook: the 1 ms PD service during long draws (tSenderResponse ~30 ms) and, while nothing is attached, a direct
// VBUS reading every 10 ms that lets fusb302::scan_service() kick the CC scan (a Home repaint as VBUS appears is ~150 ms).
void idle_hook();

// Start the loop clocks (after boot()). ctx must outlive the loop.
void begin(Context& ctx);

// One pass of the loop body; the caller sleeps until the next SysTick (wfi) between passes.
void pass();

}  // namespace k2::mainloop
