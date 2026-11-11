// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD half of boot(): the Rd decision of this reset (the boot keys, the warm-reset pin mark, the Rd intent marker of a
// comeback), the comeback's early PD service and the self-power bring-up. main.cpp calls early() right after the I2C init and
// late() where the FUSB302 is started on an ordinary boot; the host world-replay test (tests/test_world_replay.cpp) boots its
// firmware lives through the same two calls, so a power loss and its cold boot run the real decision.
#pragma once
#include "k2/core/bootmode.hpp"

namespace k2::bootpd {

struct Early {
    bootmode::Mode mode;   // the mode the Rd decision ran on (the keys, or the record's pin on a comeback)
    bool comeback;         // a comeback boot: the FUSB302, the Ra, the spoof and the bring-up have run already
};

// rdcomeback::begin(), fusb302::self_power_early(), rdcomeback::early(); on a comeback also fusb302::init(),
// rdcomeback::arm() and the bring-up loop (the charger is served before the slow init).
Early early(bootmode::Mode keys, bool warm);

// After the slow init: an ordinary boot starts the FUSB302 here, and a self-powered one runs the bring-up loop. No-op after a
// comeback (early() did both).
void late(const Early& e);

// Serves the PD link (service() every ms, poll() every 10 ms) until the self-hold contract is Ready, the comeback is released,
// or 3 s pass. A self-powered boot runs on the charger's VBUS, and the charger wants a Request within tSenderResponse
// (24-30 ms) of every capabilities message the chip acknowledges; the slow init keeps the main loop away for about a second.
void self_power_bringup();

}  // namespace k2::bootpd
