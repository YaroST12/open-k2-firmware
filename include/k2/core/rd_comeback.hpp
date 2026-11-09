// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Rd comeback on the real hardware: the boot half (read the intent marker, keep the Rd from the first instruction, arm the
// recorded identity, judge the comeback) and the run-time half (keep the marker in step with the session). The rules are pure
// and host-tested in k2/core/rd_intent.hpp; this file only binds them to the volume, the FUSB302 and the main loop.
//
// Boot order (src/core/main.cpp): begin() -> self_power_early() -> [pending(): fusb302::init(), arm(), the bring-up loop asks
// judge_step()] -> ... -> poll() from the main loop. begin() mounts the volume only on a cold plain boot (the one boot that can
// need it) and never formats: a volume that does not mount means no intent, and the ordinary init formats later as before.
#pragma once
#include <cstdint>

#include "k2/chargertest/ct_cable.hpp"
#include "k2/core/bootmode.hpp"
#include "k2/core/rd_intent.hpp"

namespace k2::rdcomeback {

// Before self_power_early(): the boot mode the Rd decision runs on (rdintent::boot_mode). Cold plain boots read the record.
bootmode::Mode begin(bootmode::Mode keys, bool warm);

// A record was found: this boot is a comeback and its Rd, Ra and spoof are being served until the verdict.
bool pending();
// The recorded intent (None unless pending() or a verdict has run).
const rdintent::Intent& found();

// Right after self_power_early() on a comeback boot: PC13 high (the Ra on CC1) in the Rd's first moments, when an identity was
// recorded, so the charger's cable look at its fresh attach finds it.
void early();
// After fusb302::init() on a comeback boot: arm the recorded spoof and take the Ra over. A recorded identity that cannot be armed
// (a deleted clone, a refused Ra) is dropped with its Ra; the Rd stays.
void arm();

// Bring-up loop step on a comeback boot: asks the judge (k2/core/rd_intent.hpp judge). True when the comeback was released
// (Rd, Ra, spoof and record gone: the boot is an ordinary plain one from here); false while waiting or once vouched.
bool judge_step(std::uint32_t ms_since_begin);

// Main loop: keeps the record in step with the session; the flash write waits for a quiet PD link (a program or erase stalls the
// CPU for milliseconds, and the charger's cable look and Request are timed in tens of them).
void poll();

// The charger test tells which spoof identity its run presents (Off = none / the run is over), so the comeback arms it.
void set_run_cable(ctcable::Choice cable);

// Charger test, at START: the planned re-attach. Writes the Reattach record for `cable` at once and reads it back (true = it is
// on the volume). The caller then drops the Rd for a moment (fusb302::rd_glitch_begin/end); end_reattach() follows once the K2
// is known to be alive (the record is then the follower's again).
bool begin_reattach(ctcable::Choice cable);
void end_reattach();

}  // namespace k2::rdcomeback
