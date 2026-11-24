// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD half of boot(), see k2/core/boot_pd.hpp.
#include "k2/core/boot_pd.hpp"

#include <cstdint>

#include "k2/core/boottrace_store.hpp"
#include "k2/core/log.hpp"
#include "k2/core/rd_comeback.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::bootpd {

// The init after fusb302::init() (black box reload, report cards, the first UI build) keeps the main loop away for about a
// second with a full store: Auto-CRC came on late, an acknowledged capabilities message went unanswered, the source sent a
// Hard Reset and cut VBUS, and a K2 without another supply died (CUKTECH 120 W, 2026-11-09, runs/selfpwr-drop/cap1). So the
// PD link is served here, before that init, until the self-hold contract is Ready or kBringupMs pass.
[[gnu::noinline]] void self_power_bringup() {
    constexpr std::uint32_t kBringupMs = 3000;
    const std::uint32_t t0 = systick::millis();
    std::uint32_t last = t0, last_poll = t0;
    while (systick::millis() - t0 < kBringupMs) {
        const std::uint32_t t = systick::millis();
        if (t != last) { last = t; fusb302::service(); }
        if (t - last_poll >= 10) { last_poll = t; fusb302::poll(); }
        boottrace::poll();   // the power trace and the meter's late-probe go on through the bring-up (no-op before boottrace::init)
        if (rdcomeback::pending() && rdcomeback::judge_step(t - t0)) break;   // released: an ordinary plain boot from here
        if (fusb302::sink_status().state == pd::SinkState::Ready) break;
    }
    K2LOG("[PD] self-power bring-up: %s after %u ms\n",
          fusb302::sink_status().state == pd::SinkState::Ready ? "contract ready" : "no contract",
          static_cast<unsigned>(systick::millis() - t0));
}

Early early(bootmode::Mode keys, bool warm) {
    // A cold plain boot reads the Rd intent marker (k2/core/rd_comeback.hpp): the comeback of a self-powered session the charger
    // cut VBUS under, or of a planned re-attach. Only that boot mounts the volume here (read-only, never formats).
    const bootmode::Mode mode = rdcomeback::begin(keys, warm);
    // Before the LCD and USB init: sink modes put Rd out so the charger starts VBUS (and keeps it) at once;
    // a plain boot writes SWITCHES0 = 0 (no Rd; the chip's default is Rd on both) BEFORE the CC switches close, and keeps an
    // Rd only after a warm reset that follows a pinned boot (the pin mark in DR21): a K2 powered over HID has no pin, so a
    // flash, `reboot` or watchdog is a detach for it. Both CC switches are routed closed in every mode.
    fusb302::self_power_early(mode, warm);
    rdcomeback::early();                 // a recorded cable identity: the Ra rises with the Rd, before the charger's cable look
    // A comeback serves the charger NOW: the source looks at the cable ~130 ms after its attach and wants a Request within ~25 ms
    // of its capabilities (measured: the look at 136 ms uptime, hardware 2026-11-09). Chip, Ra, spoof and the bring-up loop run
    // before the slow init; the loop also gives the verdict (k2/core/rd_intent.hpp judge): no source, no capabilities = released.
    const bool comeback = rdcomeback::pending();
    if (comeback) {
        fusb302::init();
        rdcomeback::arm();
        self_power_bringup();
    }
    return {mode, comeback};
}

void late(const Early& e) {
    if (e.comeback) return;   // early() did both
    fusb302::init();
    if (fusb302::self_powered()) self_power_bringup();   // before the slow init that follows: the charger feeds this boot
}

}  // namespace k2::bootpd
