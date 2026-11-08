// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Boot mode decision from the buttons held at reset (modes 1..3, boot.hpp-free pure logic).
#include <initializer_list>

#include "k2/core/bootmode.hpp"
#include "test_util.h"

using namespace k2::bootmode;

int main() {
    CHECK_EQ(static_cast<int>(decide(0)), static_cast<int>(Mode::Plain));
    CHECK_EQ(static_cast<int>(decide(kOk)), static_cast<int>(Mode::OkRdCc1));
    CHECK_EQ(static_cast<int>(decide(kPlus)), static_cast<int>(Mode::PlusRdCc2));
    CHECK_EQ(static_cast<int>(decide(kPlus | kOk)), static_cast<int>(Mode::PlusRdCc2));  // PLUS wins
    CHECK_EQ(static_cast<int>(decide(kBack)), static_cast<int>(Mode::Plain));
    CHECK_EQ(static_cast<int>(decide(kBack | kPlus)), static_cast<int>(Mode::PlusRdCc2));
    // MINUS belongs to the bootloader: it never selects an app mode.
    CHECK_EQ(static_cast<int>(decide(kMinus)), static_cast<int>(Mode::Plain));
    CHECK_EQ(static_cast<int>(decide(kMinus | kPlus)), static_cast<int>(Mode::PlusRdCc2));
    CHECK_EQ(sw0(Mode::Plain), 0);
    CHECK_EQ(sw0(Mode::OkRdCc1), 0x01);
    CHECK_EQ(sw0(Mode::PlusRdCc2), 0x02);
    CHECK_STR(name(Mode::Plain), "plain");
    CHECK_STR(name(Mode::OkRdCc1), "ok-rd-cc1");
    CHECK_STR(name(Mode::PlusRdCc2), "plus-rd-cc2");
    CHECK_EQ(self_powered(Mode::Plain), 0);
    CHECK_EQ(self_powered(Mode::PlusRdCc2), 1);
    // CC analog switch routing per mode: bit0 = PC14 (CC1), bit1 = PC15 (CC2). Both switches close in every
    // mode. PC13 is low in all.
    CHECK_EQ(cc_mask(Mode::Plain), 3);
    CHECK_EQ(cc_mask(Mode::OkRdCc1), 3);
    // With PC15 open the Rd the K2 pinned on CC2 would never reach the connector, so when PLUS (which is itself an
    // Rd on CC2) was released the charger would see no Rd and drop VBUS. Both switches stay closed.
    CHECK_EQ(cc_mask(Mode::PlusRdCc2), 3);
    // Rd across a reset. A key mode pins its own line. A plain boot keeps an Rd only when this firmware held a pin before the
    // reset (the pin mark in a backup register: a charger-powered K2) AND the reset was a warm one. The chip's own SWITCHES0 is
    // never consulted: its power-on default 0x03 and a run's or a pd-reread's Rd are no decision, so a K2 powered over HID
    // (no pin, no mark) boots passive after a cold start, a flash, a reboot or a watchdog.
    CHECK_EQ(initial_rd(Mode::PlusRdCc2, false, 0), 0x02);
    CHECK_EQ(initial_rd(Mode::OkRdCc1, true, 0), 0x01);
    CHECK_EQ(initial_rd(Mode::Plain, false, pin_mark(0x02)), 0);          // cold start: whatever the register still says
    CHECK_EQ(initial_rd(Mode::Plain, true, 0), 0);                        // HID-powered warm reset = a detach
    CHECK_EQ(initial_rd(Mode::Plain, true, 0x0003), 0);                   // a stray register value is no mark
    CHECK_EQ(initial_rd(Mode::Plain, true, pin_mark(0x01)), 0x01);
    CHECK_EQ(initial_rd(Mode::Plain, true, pin_mark(0x02)), 0x02);
    // Rd on both lines is the Type-C Debug Accessory signature: a source that sees it on a direct plug turns its Rp off and
    // stays off until a physical detach (OnePlus 120W brick, hardware 2026-11-06). No pin is ever both lines, so no mark
    // carries it either, whatever a stale register says.
    CHECK_EQ(initial_rd(Mode::Plain, true, pin_mark(0x03)), 0);
    CHECK_EQ(int(pin_mark(0x03)), 0);
    CHECK_EQ(int(pin_from_mark(std::uint16_t(kPinMarkMagic | 0x03))), 0);
    CHECK_EQ(initial_rd(Mode::Plain, true, pin_mark(0)), 0);              // a cleared mark
    CHECK_EQ(int(pin_from_mark(pin_mark(0x02))), 2);
    CHECK_EQ(int(pin_from_mark(0xFFFF)), 0);                              // wrong magic
    CHECK_EQ(int(pin_from_mark(0x0003)), 0);
    CHECK_EQ(int(pin_from_mark(std::uint16_t(pin_mark(0x01) ^ 0x0100))), 0);
    // The charger may sit on the other orientation of the plug. A key mode never puts Rd on both lines to find out: when no VBUS
    // shows after kMoveAfterMs, the pinned line sees no Rp and the other line does, the single Rd MOVES to the other line.
    // 600 ms = tCCDebounce (up to 200) + tVBUSON (up to 275) + a sampling margin: a source that is going to answer our Rd has
    // answered by then. cc mv = the ADC level of each line: an open Rp reads ~3.3 V, an Rp over a sink Rd 0.2 V and up, a line
    // nothing drives ~0 V.
    CHECK_EQ(int(kMoveAfterMs), 600);
    CHECK_EQ(int(moved_pin(2, 600, 0, 3300, 0)), 1);                 // PLUS pinned CC2, the charger's Rp is on CC1
    CHECK_EQ(int(moved_pin(1, 600, 0, 0, 3300)), 2);                 // OK pinned CC1, Rp on CC2
    CHECK_EQ(int(moved_pin(2, 599, 0, 3300, 0)), 2);                 // too early
    CHECK_EQ(int(moved_pin(2, 5000, 0, 3300, 0)), 1);                // later still counts (the caller moves once)
    CHECK_EQ(int(moved_pin(2, 600, -1, 3300, 0)), 1);                // power monitor not answering counts as no VBUS
    CHECK_EQ(int(moved_pin(2, 600, 3999, 3300, 0)), 1);
    CHECK_EQ(int(moved_pin(2, 600, 4000, 3300, 0)), 2);              // VBUS is up: the Rd works where it is
    CHECK_EQ(int(moved_pin(2, 600, 5000, 3300, 0)), 2);
    CHECK_EQ(int(moved_pin(2, 600, 0, 3300, 410)), 2);               // the pinned line sees Rp over our Rd: a direct plug, stay
    CHECK_EQ(int(moved_pin(2, 600, 0, 3300, kRpSeenMv)), 2);
    CHECK_EQ(int(moved_pin(2, 600, 0, 3300, kRpSeenMv - 1)), 1);
    CHECK_EQ(int(moved_pin(2, 600, 0, 0, 0)), 2);                    // no Rp anywhere (a source already latched off): stay
    CHECK_EQ(int(moved_pin(2, 600, 0, kRpSeenMv - 1, 0)), 2);
    CHECK_EQ(int(moved_pin(1, 600, 0, 410, 3300)), 1);               // both lines see Rp: stay
    CHECK_EQ(int(moved_pin(0, 600, 0, 3300, 0)), 0);                 // plain boot: no pin, nothing to move
    CHECK_EQ(int(moved_pin(3, 600, 0, 3300, 0)), 3);                 // not a state this firmware reaches; never invented either
    for (std::uint8_t pin = 0; pin <= 2; pin++)                      // whatever the inputs, the answer is one line or none
        for (std::int32_t v : {-1, 0, 5000})
            for (std::int32_t a : {0, 150, 410, 3300})
                for (std::int32_t b : {0, 150, 410, 3300})
                    CHECK_EQ(int(moved_pin(pin, 900, v, a, b) == 3), 0);
    return test_report("bootmode");
}
