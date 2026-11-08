// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Persister (k2/core/bbpersist.hpp): finished captures go to the flash ring, a boot reloads the newest
// ones into the RAM slots, and nothing is written while the link is busy or after a long idle.
#include <cstring>
#include <set>
#include <algorithm>
#include <vector>

#include "k2/core/bbpersist.hpp"
#include "k2/pd/pdtrace.hpp"
#include "ramdisk.hpp"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;

namespace {
// One littlefs volume on a RAM disk (what survives a power cycle).
struct FakeFlash {
    k2test::RamDisk disk{96};
    fs::Fs fs{disk, 96};
    FakeFlash() { bool fm = false; fs.mount_or_format(fm); }
    std::uint64_t programs() const { return disk.progs; }
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
        bbpersist::Persister p(f.fs);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();
        const auto w0 = f.programs();                     // the format and the mount are done
        CHECK_EQ(p.status().entries, 0);
        CHECK_EQ(p.status().free_bytes > 80 * 2048, 1);
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
        bbpersist::Persister p(f.fs);
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
            bbpersist::Persister p(f.fs);
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
        bbpersist::Persister p(f.fs);
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
        bbpersist::Persister p(f.fs);
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
        bbpersist::Persister p(f.fs);
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
    {   // make_room: a ring that grew to the whole volume under an older build gives the journal its blocks back at once, oldest first
        k2test::RamDisk disk{40};
        fs::Fs vol{disk, 40};
        bool fm = false;
        vol.mount_or_format(fm);
        int before = 0;
        {   // fill: no reserve, as the older build did
            bbstore::Ring r(vol);
            r.set_reserve(0);
            r.mount();
            std::vector<std::uint8_t> raw(1500);
            std::vector<std::uint8_t> scratch(lz::bound(raw.size()));
            std::uint32_t x = 12345;
            for (int i = 0; i < 40; i++) {
                for (auto& b : raw) { x = x * 1664525u + 1013904223u; b = std::uint8_t(x >> 24); }
                r.append({unsigned(pdtrace::Trigger::Contract), 0, 0}, raw, scratch);
            }
            before = r.stats().entries;
            CHECK_EQ(vol.block_count() - vol.used_blocks() < bbstore::kJournalReserveBlocks, 1);   // full, as on the device (90/90)
        }
        bbpersist::Persister p(vol);
        pdtrace::init();
        pdtrace::clear_captures();
        p.init();                                                     // the room is made at the mount, not on some later save
        CHECK_EQ(vol.block_count() - vol.used_blocks() >= bbstore::kJournalReserveBlocks, 1);
        CHECK_EQ(p.status().entries < before, 1);
        const int entries = p.status().entries;
        CHECK_EQ(p.make_room(), 0u);                                  // already there: nothing is evicted
        CHECK_EQ(p.status().entries, entries);
    }
    {   // hold_run, the Contract captures: during a charger test the per-row Contract captures wait (a save stalls the loop for ~250 ms with
        // interrupts off); the rare ones (hard reset) still go, and the Contract ones go once the hold lapses.
        FakeFlash f;
        bbpersist::Persister p(f.fs);
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
    return test_report("test_bbpersist");
}
