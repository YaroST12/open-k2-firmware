// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box persistence bound to the real flash, see k2/core/bbpersist.hpp.
#include "k2/core/log.hpp"
#include "k2/core/bbpersist.hpp"

#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::bbpersist {
namespace {
Persister g_p(fs::bb_ring());
}  // namespace

void init() {
    g_p.init();
    const Status s = g_p.status();
    K2LOG("[BBSTORE] ring=%d entries=%d used=%u free=%u\n", s.ready ? 1 : 0, s.entries, static_cast<unsigned>(s.used_bytes),
          static_cast<unsigned>(s.free_bytes));
}
// One line per save or refusal, with how long the main loop stood still: a save runs flash programs with interrupts off.
void poll(bool may_erase) {
    const Status a = g_p.status();
    g_p.poll();
    const Status b = g_p.status();
    if (b.saves != a.saves || b.skipped != a.skipped || b.errors != a.errors || b.dropped != a.dropped)
        // kind = trigger of the last save (1 Contract, 2 HardReset, 3 VbusDip, 4 Detach, 5 Edges, 6 Refusal), held = the run's hold
        // was up when it was written, wait = captures the hold keeps back now, C/D/E = saves so far of Contract/VbusDip/Edges,
        // drop = waiting captures a newer one overwrote, defer = rare captures the run's budget made wait.
        K2LOG("[BB] saves=%u skipped=%u errors=%u last=%ums max=%ums entries=%d free=%u kind=%u held=%d wait=%u C=%u D=%u E=%u drop=%u defer=%u\n",
              static_cast<unsigned>(b.saves), static_cast<unsigned>(b.skipped), static_cast<unsigned>(b.errors),
              static_cast<unsigned>(b.last_ms), static_cast<unsigned>(b.max_ms), b.entries, static_cast<unsigned>(b.free_bytes),
              b.last_kind, b.last_while_held ? 1 : 0, static_cast<unsigned>(b.waiting),
              static_cast<unsigned>(b.by_kind[1]), static_cast<unsigned>(b.by_kind[3]), static_cast<unsigned>(b.by_kind[5]),
              static_cast<unsigned>(b.dropped), static_cast<unsigned>(b.deferred));
    if (g_p.maintain(may_erase)) {
        const Status s = g_p.status();
        K2LOG("[BB] ring erase: dropped=%u erases=%u entries=%d free=%u\n", static_cast<unsigned>(s.made_room - b.made_room),
              static_cast<unsigned>(s.erases), s.entries, static_cast<unsigned>(s.free_bytes));
    }
}
void hold_run() { g_p.hold_run(); }
// At START, before Rd goes on: the ring's spare sector is blank for the whole run (one line only when something was erased).
void make_room() {
    const std::uint32_t e0 = g_p.status().erases;
    const unsigned n = g_p.make_room();
    if (g_p.status().erases != e0)
        K2LOG("[BB] made room: %u captures dropped, free=%u\n", n, static_cast<unsigned>(g_p.status().free_bytes));
}
Status status() { return g_p.status(); }

}  // namespace k2::bbpersist
