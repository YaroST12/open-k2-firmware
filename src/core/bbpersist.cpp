// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box persistence bound to the real flash, see k2/core/bbpersist.hpp.
#include "k2/core/log.hpp"
#include "k2/core/bbpersist.hpp"

#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::bbpersist {
namespace {
Persister g_p(fs::instance());
}  // namespace

void init() {
    g_p.init();
    const Status s = g_p.status();
    K2LOG("[BBSTORE] entries=%d used=%u free=%u reserve=%u evicted=%u\n", s.entries, static_cast<unsigned>(s.used_bytes),
          static_cast<unsigned>(s.free_bytes), static_cast<unsigned>(bbstore::kJournalReserveBlocks),
          static_cast<unsigned>(s.made_room));
}
// One line per save or refusal, with how long the main loop stood still: a save runs flash programs with interrupts off.
void poll() {
    const Status a = g_p.status();
    g_p.poll();
    const Status b = g_p.status();
    if (b.saves != a.saves || b.skipped != a.skipped || b.errors != a.errors)
        // kind = trigger of the last save (1 Contract, 2 HardReset, 3 VbusDip, 4 Detach, 5 Edges, 6 Refusal), held = the run's hold
        // was up when it was written, wait = captures the hold keeps back now, C/D/E = saves so far of Contract/VbusDip/Edges.
        K2LOG("[BB] saves=%u skipped=%u errors=%u last=%ums max=%ums entries=%d free=%u kind=%u held=%d wait=%u C=%u D=%u E=%u\n",
              static_cast<unsigned>(b.saves), static_cast<unsigned>(b.skipped), static_cast<unsigned>(b.errors),
              static_cast<unsigned>(b.last_ms), static_cast<unsigned>(b.max_ms), b.entries, static_cast<unsigned>(b.free_bytes),
              b.last_kind, b.last_while_held ? 1 : 0, static_cast<unsigned>(b.waiting),
              static_cast<unsigned>(b.by_kind[1]), static_cast<unsigned>(b.by_kind[3]), static_cast<unsigned>(b.by_kind[5]));
}
void hold_run() { g_p.hold_run(); }
// At START, before Rd goes on: the journal's blocks are free for the whole run (one line only when something went).
void make_room() {
    const unsigned n = g_p.make_room();
    if (n) K2LOG("[BB] made room: %u captures evicted, free=%u\n", n, static_cast<unsigned>(g_p.status().free_bytes));
}
Status status() { return g_p.status(); }

}  // namespace k2::bbpersist
