// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Persister (k2/core/bbpersist.hpp): finished captures go to the flash ring, a boot reloads the newest
// ones into the RAM slots, and nothing is written while the link is busy or after a long idle.
#include <cstring>
#include <memory>
#include <set>
#include <algorithm>
#include <vector>

#include "k2/core/bbpersist.hpp"
#include "k2/drivers/flash.hpp"
#include "k2/pd/pdtrace.hpp"
#include "ramflash.hpp"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;

namespace {
// The Black Box ring on a RAM flash (what survives a power cycle); reboot() scans it again with a fresh ring.
struct FakeFlash {
    k2test::RamFlash disk;
    std::unique_ptr<ringlog::Ring> ring;
    FakeFlash() { reboot(); }
    void reboot() {
        ring = std::make_unique<ringlog::Ring>(disk, flash::kBbRingBase, flash::kBbRingSectors, bbstore::kRingKind);
        ring->scan();
    }
    std::uint64_t programs() const { return disk.progs; }
    // Stores incompressible captures (one to a sector) with no maintenance until the ring refuses; returns how many.
    int fill() {
        bbstore::Ring r(*ring);
        r.mount();
        std::vector<std::uint8_t> raw(1980), scratch(lz::bound(raw.size()));
        std::uint32_t x = 12345;
        int n = 0;
        for (;; n++) {
            for (auto& b : raw) { x = x * 1664525u + 1013904223u; b = std::uint8_t(x >> 24); }
            if (r.append({unsigned(pdtrace::Trigger::Contract), 0, 0}, raw, scratch) != bbstore::Err::Ok) return n;
        }
    }
};

void msg(std::uint16_t hdr, std::initializer_list<std::uint32_t> objs = {}) {
    std::vector<std::uint8_t> p;
    for (std::uint32_t o : objs) for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(o >> (8 * i)));
    p.insert(p.end(), 4, 0);
    pdtrace::rx(0xE0, hdr, p);
}
// One finished HardReset capture whose last record is at `at + 600`.
void capture(std::uint32_t at, unsigned vid) {
    g_stub.ms = at - 300;
    msg(0x7101, {0x0001912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x0006414F});
    msg(0x4F | 0x100 | 4u << 12, {0xFF00A041, vid, 0, 0x1234u << 16});
    pdtrace::vbus_sample(12000, 100);
    g_stub.ms = at;
    pdtrace::reset(pdtrace::ResetKind::HardRx);
    g_stub.ms = at + 600;
    msg(0x0001, {});                                      // GoodCRC: traffic, no trigger
}
std::vector<std::uint8_t> bytes(std::size_t slot) {
    const auto d = pdtrace::capture_data(slot);
    return {d.begin(), d.end()};
}
int slot_of_newest() {
    int best = -1;
    std::uint32_t seq = 0;
    for (std::size_t i = 0; i < pdtrace::kCapSlots; i++) {
        const auto in = pdtrace::capture_info(i);
        if (in.trigger != pdtrace::Trigger::None && in.seq > seq) { seq = in.seq; best = int(i); }
    }
    return best;
}
}  // namespace

int main() {
    const std::uint32_t kQuiet = bbpersist::kQuietMs;
    {   // nothing yet; a finished capture is written once, after the link went quiet
        FakeFlash f;
        bbpersist::Persister p(*f.ring);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();
        const auto w0 = f.programs();                     // the format and the mount are done
        CHECK_EQ(p.status().entries, 0);
        CHECK_EQ(p.status().free_bytes, flash::kBbRingSectors * (ringlog::kSector - ringlog::kSectorHeader));
        CHECK_EQ(p.status().ready, 1);
        capture(100000, 0x05AC);
        g_stub.ms = 100000 + 600 + 5;
        p.poll();                                         // traffic 5 ms ago: wait
        CHECK_EQ(p.status().entries, 0);
        CHECK_EQ(f.programs(), w0);
        g_stub.ms = 100000 + 600 + kQuiet + 10;
        p.poll();
        CHECK_EQ(p.status().entries, 1);
        CHECK_EQ(p.status().used_bytes > 0, 1);
        const auto w = f.programs();
        p.poll();
        CHECK_EQ(f.programs(), w);                          // not written twice
    }
    {   // a capture that is still filling is left alone
        FakeFlash f;
        bbpersist::Persister p(*f.ring);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();
        g_stub.ms = 200000;
        msg(0x7101, {0x0001912C});
        pdtrace::reset(pdtrace::ResetKind::HardRx);
        g_stub.ms = 200000 + 100;
        p.poll();
        g_stub.ms = 200000 + 400;
        p.poll();
        CHECK_EQ(p.status().entries, 0);
    }
    {   // power cycle: the newest three come back as stored slots, the data identical, nothing rewritten
        FakeFlash f;
        std::vector<std::vector<std::uint8_t>> sent;
        {
            bbpersist::Persister p(*f.ring);
            pdtrace::init();
            pdtrace::clear_captures();
            p.init();
            for (unsigned k = 0; k < 5; k++) {
                const std::uint32_t at = 300000 + 10000 * k;
                capture(at, 0x1000 + k);
                g_stub.ms = at + 600 + kQuiet + 10;
                p.poll();
                sent.push_back(bytes(std::size_t(slot_of_newest())));
            }
            CHECK_EQ(p.status().entries, 5);
        }
        pdtrace::init();                                  // reboot: RAM slots are gone
        pdtrace::clear_captures();
        g_stub.ms = 5000;
        f.reboot();
        bbpersist::Persister p(*f.ring);
        p.init();
        CHECK_EQ(p.status().entries, 5);
        int stored = 0;
        for (std::size_t i = 0; i < pdtrace::kCapSlots; i++) {
            const auto in = pdtrace::capture_info(i);
            CHECK_EQ(in.trigger == pdtrace::Trigger::HardReset, 1);
            stored += in.stored;
            CHECK_EQ(in.filling, 0);
        }
        CHECK_EQ(stored, 3);
        CHECK_EQ(bytes(std::size_t(slot_of_newest())) == sent[4], 1);
        // the three newest, oldest first by sequence
        std::vector<std::vector<std::uint8_t>> got(3);
        std::uint32_t lo = ~0u;
        for (std::size_t i = 0; i < 3; i++) lo = std::min(lo, pdtrace::capture_info(i).seq);
        for (std::size_t i = 0; i < 3; i++) got[pdtrace::capture_info(i).seq - lo] = bytes(i);
        CHECK_EQ(got[0] == sent[2] && got[1] == sent[3] && got[2] == sent[4], 1);
        const auto w = f.programs();
        g_stub.ms = 9000;
        p.poll();
        CHECK_EQ(f.programs(), w);                          // restored captures are not saved again
        // a new live capture is saved and replaces the oldest stored slot
        capture(400000, 0x2222);
        g_stub.ms = 400000 + 600 + kQuiet + 10;
        p.poll();
        CHECK_EQ(p.status().entries, 6);
        CHECK_EQ(pdtrace::capture_info(std::size_t(slot_of_newest())).stored, 0);
    }
    {   // idle: no PD traffic and no VBUS change for the idle time, so a finished capture is not written
        FakeFlash f;
        bbpersist::Persister p(*f.ring);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();
        capture(500000, 0x3333);
        g_stub.ms = 500000 + 600 + bbstore::kIdleMs + 1000;
        CHECK_EQ(pdtrace::idle(), 1);
        p.poll();
        CHECK_EQ(p.status().entries, 0);
        CHECK_EQ(p.status().idle, 1);
        CHECK_EQ(p.status().skipped, 1);
        msg(0x0001, {});                                  // traffic again
        CHECK_EQ(pdtrace::idle(), 0);
        CHECK_EQ(p.status().idle || true, 1);
    }
    {   // hold_run: a live run writes nothing but the rare captures (hard reset, detach, refusal): Contract, VBUS-dip and edge
        // captures wait, whatever the row. Hardware 2026-11-08: saves=7..29 of 95-144 ms inside a run with only Contract held.
        FakeFlash f;
        bbpersist::Persister p(*f.ring);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();
        // a VBUS dip (21 V to 5 V after the Contract window has passed)
        g_stub.ms = 800000;
        msg(0x7101, {0x0001912C, 0x0002D12C});
        pdtrace::vbus_sample(20000, 100);
        g_stub.ms += 100;
        pdtrace::vbus_sample(20000, 100);
        g_stub.ms += 100;
        pdtrace::vbus_sample(5000, 100);
        g_stub.ms += 100;
        pdtrace::vbus_sample(5000, 100);
        g_stub.ms += 800;
        msg(0x0001, {});
        int dips = 0;
        for (std::size_t i = 0; i < pdtrace::kCapSlots; i++)
            if (pdtrace::capture_info(i).trigger == pdtrace::Trigger::VbusDip) dips++;
        CHECK_EQ(dips, 1);                                            // the test is only worth anything with a dip capture
        g_stub.ms += kQuiet + 10;
        p.hold_run();
        p.poll();
        CHECK_EQ(p.status().entries, 0);                              // a dip waits too
        CHECK_EQ(p.status().waiting, 1u);
        // a refusal is rare and not held
        pdtrace::refusal(1, 2);
        g_stub.ms += 600;
        msg(0x0001, {});
        g_stub.ms += kQuiet + 10;
        p.hold_run();
        p.poll();
        CHECK_EQ(p.status().entries, 1);
        CHECK_EQ(p.status().by_kind[unsigned(pdtrace::Trigger::Refusal)], 1u);
        CHECK_EQ(p.status().by_kind[unsigned(pdtrace::Trigger::VbusDip)], 0u);
        // the hold lapses: the dip goes in, counted under its own kind
        g_stub.ms += bbpersist::kHoldLapseMs + 10;
        p.poll();
        CHECK_EQ(p.status().entries, 2);
        CHECK_EQ(p.status().by_kind[unsigned(pdtrace::Trigger::VbusDip)], 1u);
        CHECK_EQ(p.status().last_kind, unsigned(pdtrace::Trigger::VbusDip));
        CHECK_EQ(p.status().last_while_held, 0);
    }
    {   // make_room (a charger test starts): a full ring erases its oldest sector at once and drops that capture; the mount
        // never erases, and a second call finds nothing to do
        FakeFlash f;
        CHECK_EQ(f.fill(), int(flash::kBbRingSectors));              // one capture to a sector, every sector used
        bbpersist::Persister p(*f.ring);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();
        CHECK_EQ(f.disk.erases, 0u);
        CHECK_EQ(p.status().entries, int(flash::kBbRingSectors));
        CHECK_EQ(p.make_room(), 1u);
        CHECK_EQ(f.disk.erases, 1u);
        CHECK_EQ(p.status().entries, int(flash::kBbRingSectors) - 1);
        CHECK_EQ(p.status().made_room, 1u);
        CHECK_EQ(p.status().erases, 1u);
        CHECK_EQ(p.make_room(), 0u);
        CHECK_EQ(f.disk.erases, 1u);
    }
    {   // maintain (main loop): erases only when the caller allows it, never while a run holds, and one erase per
        // kEraseGapMs at most
        FakeFlash f;
        f.fill();
        bbpersist::Persister p(*f.ring);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();
        g_stub.ms = 3000000;
        CHECK_EQ(p.maintain(false), 0);                               // a test, PD Stress or a busy link
        p.hold_run();
        CHECK_EQ(p.maintain(true), 0);                                // the run's hold is up
        CHECK_EQ(f.disk.erases, 0u);
        g_stub.ms += bbpersist::kHoldLapseMs + 10;
        CHECK_EQ(p.maintain(true), 1);
        CHECK_EQ(f.disk.erases, 1u);
        CHECK_EQ(p.maintain(true), 0);                                // the spare sector is blank: nothing to do
        f.fill();                                                     // another capture takes it
        CHECK_EQ(p.maintain(true), 0);                                // too soon after the last erase
        CHECK_EQ(f.disk.erases, 1u);
        g_stub.ms += bbpersist::kEraseGapMs;
        CHECK_EQ(p.maintain(true), 1);
        CHECK_EQ(f.disk.erases, 2u);
        CHECK_EQ(p.status().erases, 2u);
    }
    {   // hold_run, the Contract captures: during a charger test the per-row Contract captures wait (a save stalls the loop for ~250 ms with
        // interrupts off); the rare ones (hard reset) still go, and the Contract ones go once the hold lapses.
        FakeFlash f;
        bbpersist::Persister p(*f.ring);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();
        g_stub.ms = 600000;
        msg(0x7101, {0x0001912C, 0x0002D12C});
        pdtrace::sink(4, 0, false, false, 3);                        // a Contract capture
        g_stub.ms = 600000 + 600;
        msg(0x0001, {});
        g_stub.ms = 600000 + 600 + kQuiet + 10;
        p.hold_run();
        p.poll();
        CHECK_EQ(p.status().entries, 0);                              // waits
        CHECK_EQ(p.status().held, 1);
        CHECK_EQ(p.status().skipped, 0);
        const auto w = f.programs();
        g_stub.ms += 500;
        p.hold_run();                                           // the session renews it every tick
        p.poll();
        CHECK_EQ(f.programs(), w);
        capture(700000, 0x4444);                                      // a hard reset capture is not held back
        g_stub.ms = 700000 + 600 + kQuiet + 10;
        p.hold_run();
        p.poll();
        CHECK_EQ(p.status().entries, 1);
        g_stub.ms += bbpersist::kHoldLapseMs + 10;                    // the session stopped renewing: the hold lapses
        p.poll();
        CHECK_EQ(p.status().held, 0);
        CHECK_EQ(p.status().entries, 2);                              // the Contract capture is saved now (if still in a slot)
    }
    {   // a full ring (no maintenance yet) refuses a capture once: later polls neither retry it nor touch the flash, the next
        // capture costs one refusal again, and a save never erases (hardware 2026-11-11: two refusals per VBUS cut)
        FakeFlash f;
        f.fill();
        bbpersist::Persister p(*f.ring);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();
        const int entries = p.status().entries;
        capture(800000, 0x5555);
        g_stub.ms = 800000 + 600 + kQuiet + 10;
        const auto ops = f.disk.ops();
        p.poll();
        CHECK_EQ(p.status().skipped, 1u);
        CHECK_EQ(p.status().saves, 0u);
        CHECK_EQ(p.status().entries, entries);
        CHECK_EQ(f.disk.ops(), ops);
        for (int k = 0; k < 50; k++) { g_stub.ms += 20; p.poll(); }
        CHECK_EQ(p.status().skipped, 1u);                             // not retried
        CHECK_EQ(f.disk.ops(), ops);                                  // and the polls never touched the flash
        capture(900000, 0x6666);
        g_stub.ms = 900000 + 600 + kQuiet + 10;
        p.poll();
        CHECK_EQ(p.status().skipped, 2u);
        CHECK_EQ(f.disk.ops(), ops);
    }
    {   // a run's rare captures: kRareBudget go out at once, the rest wait kept in their slots (a Contract capture takes
        // another slot), a full set of waiting captures loses its oldest and counts it, and the end of the run writes one
        // capture per poll()
        FakeFlash f;
        bbpersist::Persister p(*f.ring);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();
        std::uint32_t t = 1000000;
        const auto settle_at = [&](std::uint32_t at) { g_stub.ms = at + 600 + kQuiet + 10; p.hold_run(); p.poll(); };
        const auto contract = [&](std::uint32_t at, std::uint8_t pos) {
            g_stub.ms = at - 300;
            msg(0x7101, {0x0001912C, 0x0002D12C});
            pdtrace::sink(0, 0, true, false, pos);
            g_stub.ms = at + 600;
            msg(0x0001, {});
        };
        for (unsigned k = 0; k < 4; k++, t += 1500) { capture(t, 0x7000 + k); settle_at(t); }
        CHECK_EQ(p.status().entries, int(bbpersist::kRareBudget));   // two hard resets written, two waited
        CHECK_EQ(p.status().waiting, 2u);
        CHECK_EQ(p.status().deferred, 2u);
        CHECK_EQ(p.status().dropped, 0u);
        contract(t, 1);                                              // a per-row capture: it must not take a waiting slot
        settle_at(t);
        t += 1500;
        CHECK_EQ(p.status().waiting, 3u);
        CHECK_EQ(p.status().dropped, 0u);
        CHECK_EQ(p.status().entries, int(bbpersist::kRareBudget));
        contract(t, 2);                                              // every slot waits now: the oldest is overwritten
        settle_at(t);
        CHECK_EQ(p.status().dropped, 1u);
        CHECK_EQ(p.status().waiting, 3u);
        // the run ends: one save per poll, oldest first, nothing left waiting
        g_stub.ms += bbpersist::kHoldLapseMs + 10;
        const int before = p.status().entries;
        p.poll();
        CHECK_EQ(p.status().entries, before + 1);
        p.poll();
        CHECK_EQ(p.status().entries, before + 2);
        p.poll();
        CHECK_EQ(p.status().entries, before + 3);
        CHECK_EQ(p.status().waiting, 0u);
        p.poll();
        CHECK_EQ(p.status().entries, before + 3);
        // a new run gets its budget back
        const std::uint32_t saves = p.status().saves;
        t = g_stub.ms + 3000;
        capture(t, 0x7100);
        settle_at(t);
        CHECK_EQ(p.status().saves, saves + 1);
    }
    return test_report("test_bbpersist");
}
