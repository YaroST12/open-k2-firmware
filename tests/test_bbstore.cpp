// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box store (k2/core/bbstore.hpp) over its ring log on a RAM flash: append, reboot, the newest 32, a full ring that
// refuses without erasing, maintenance that drops the oldest sector, torn writes, corruption and the idle gate.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "k2/core/bbstore.hpp"
#include "k2/drivers/flash.hpp"
#include "ramflash.hpp"
#include "test_util.h"

using namespace k2;

namespace {
// The Black Box ring on a RAM flash; reboot() rescans it with a fresh ring and store (no RAM state survives). idle()
// runs the maintenance the main loop runs while nothing is busy, until it has nothing left to do.
struct Vol {
    k2test::RamFlash disk;
    std::unique_ptr<ringlog::Ring> ring;
    std::unique_ptr<bbstore::Ring> r;
    Vol() { reboot(); }
    void reboot() {
        r.reset();
        ring = std::make_unique<ringlog::Ring>(disk, flash::kBbRingBase, flash::kBbRingSectors, bbstore::kRingKind);
        ring->scan();
        r = std::make_unique<bbstore::Ring>(*ring);
        r->mount();
    }
    int idle() {
        int dropped = 0;
        for (int d; (d = r->maintain(true)) >= 0;) dropped += d;
        return dropped;
    }
};

using Buf = std::vector<std::uint8_t>;
// Repetitive, record-shaped data that differs per seed.
Buf sample(unsigned seed, std::size_t n) {
    Buf b(n);
    for (std::size_t i = 0; i < n; i++) b[i] = std::uint8_t((i % 41 < 30 ? i * 3 : seed + i / 41) + seed);
    return b;
}
Buf noise(std::size_t n) {
    std::srand(7);
    Buf b(n);
    for (auto& x : b) x = std::uint8_t(std::rand());
    return b;
}

std::array<std::uint8_t, 4096> g_scratch;

bbstore::Err add(bbstore::Ring& r, unsigned seed, std::size_t n = 1500, std::uint32_t at_s = 10) {
    const Buf b = sample(seed, n);
    return r.append({1 + seed % 4, 1000, at_s}, b, g_scratch);
}
// The seeds of the listed captures, oldest first.
std::vector<unsigned> order(const bbstore::Ring& r) {
    std::vector<unsigned> seeds;
    r.each([&](const bbstore::Entry& e) {
        Buf out(2048);
        const std::size_t n = r.unpack(e, out);
        for (unsigned s = 0; s < 400; s++)
            if (n == e.raw_len && n >= 1500 && std::memcmp(out.data(), sample(s, n).data(), n) == 0) { seeds.push_back(s); break; }
    });
    return seeds;
}
// Appends with no maintenance until the ring refuses; returns how many went in.
unsigned fill(Vol& v, unsigned first) {
    unsigned k = first;
    while (add(*v.r, k) == bbstore::Err::Ok) k++;
    return k - first;
}
}  // namespace

int main() {
    {   // layout and policy constants
        CHECK_EQ(bbstore::kMaxEntries, 32u);
        CHECK_EQ(bbstore::kHeader + bbstore::kMaxPacked, ringlog::kMaxPayload);   // one capture at most fills a sector
        CHECK_EQ(bbstore::kRingKind, std::uint8_t('B'));
        CHECK_EQ(flash::kBbRingSectors, 20u);
    }
    {   // empty ring
        Vol v;
        const auto s = v.r->stats();
        CHECK_EQ(s.entries, 0);
        CHECK_EQ(s.used_bytes, 0u);
        CHECK_EQ(s.free_bytes, flash::kBbRingSectors * (ringlog::kSector - ringlog::kSectorHeader));
        CHECK_EQ(v.r->next_boot(), 0);
        CHECK_EQ(v.r->ready(), 1);
        CHECK_EQ(v.r->needs_maintenance(), 0);
        int n = 0;
        v.r->each([&](const bbstore::Entry&) { n++; });
        CHECK_EQ(n, 0);
    }
    {   // a ring that was never scanned refuses cleanly
        k2test::RamFlash disk;
        ringlog::Ring rl(disk, flash::kBbRingBase, flash::kBbRingSectors, bbstore::kRingKind);
        bbstore::Ring r(rl);
        CHECK_EQ(r.ready(), 0);
        CHECK_EQ(int(add(r, 1)), int(bbstore::Err::Flash));
        CHECK_EQ(r.maintain(true), -1);
        CHECK_EQ(disk.ops(), 0);
    }
    {   // append, power cycle, read back
        Vol v;
        CHECK_EQ(int(add(*v.r, 1)), int(bbstore::Err::Ok));
        CHECK_EQ(int(add(*v.r, 2, 900, 55)), int(bbstore::Err::Ok));
        v.reboot();
        CHECK_EQ(v.r->stats().entries, 2);
        CHECK_EQ(v.r->next_boot(), 1);
        std::vector<bbstore::Entry> es;
        v.r->each([&](const bbstore::Entry& e) { es.push_back(e); });   // e.packed maps the flash: valid until an erase
        CHECK_EQ(es.size(), 2u);
        CHECK_EQ(es[0].trigger, 2);
        CHECK_EQ(es[0].raw_len, 1500);
        CHECK_EQ(es[0].pre, 1000);
        CHECK_EQ(es[1].raw_len, 900);
        CHECK_EQ(es[1].at_s, 55u);
        CHECK_EQ(es[1].boot, 0);
        Buf out(2048);
        CHECK_EQ(v.r->unpack(es[1], out), 900u);
        const Buf want = sample(2, 900);
        CHECK_EQ(std::memcmp(out.data(), want.data(), 900), 0);
        CHECK_EQ(es[0].packed_len < 1500, 1);
        CHECK_EQ(v.r->stats().used_bytes, 2 * bbstore::kHeader + es[0].packed_len + es[1].packed_len);
        CHECK_EQ(int(add(*v.r, 3)), int(bbstore::Err::Ok));
        CHECK_EQ(v.r->stats().entries, 3);
        v.reboot();
        bbstore::Entry last{};
        v.r->each([&](const bbstore::Entry& e) { last = e; });
        CHECK_EQ(last.boot, 1);                      // written by the second boot
        CHECK_EQ(v.r->next_boot(), 2);
    }
    {   // a full ring refuses without erasing (no erase, no program); maintenance only when the caller allows it, then
        // the next capture fits. Seeds stay below 256: sample() repeats with seed mod 256.
        Vol v;
        const unsigned n = fill(v, 0);
        CHECK_EQ(n > bbstore::kMaxEntries, 1);       // the ring holds more than the newest 32 a boot lists
        CHECK_EQ(v.ring->blank(), 0u);
        CHECK_EQ(v.disk.erases, 0u);
        CHECK_EQ(v.r->needs_maintenance(), 1);
        const auto ops = v.disk.ops();
        CHECK_EQ(int(add(*v.r, 250)), int(bbstore::Err::Full));
        CHECK_EQ(v.disk.ops(), ops);
        CHECK_EQ(v.r->maintain(false), -1);          // not now: a test or a busy link
        CHECK_EQ(v.disk.ops(), ops);
        const int dropped = v.r->maintain(true);     // the oldest sector goes, with its captures
        CHECK_EQ(dropped > 0, 1);
        CHECK_EQ(v.disk.erases, 1u);
        CHECK_EQ(v.r->needs_maintenance(), 0);
        CHECK_EQ(v.r->maintain(true), -1);           // nothing left to do
        CHECK_EQ(int(add(*v.r, 250)), int(bbstore::Err::Ok));
        const auto seeds = order(*v.r);
        CHECK_EQ(seeds.size(), std::size_t(bbstore::kMaxEntries));
        CHECK_EQ(seeds.back(), 250u);
    }
    {   // a ring with no room for even a small capture refuses before the compression: the scratch buffer is not written
        Vol v;
        const Buf b = noise(1980);                   // one capture fills a sector to its last few bytes
        while (v.r->append({1, 100, 1}, b, g_scratch) == bbstore::Err::Ok) {}
        CHECK_EQ(v.r->stats().free_bytes < ringlog::record_bytes(bbstore::kHeader + 16), 1);
        g_scratch.fill(0xEE);
        CHECK_EQ(int(add(*v.r, 1)), int(bbstore::Err::Full));
        bool untouched = true;
        for (auto x : g_scratch) untouched = untouched && x == 0xEE;
        CHECK_EQ(untouched, 1);
    }
    {   // wrap: with idle moments between captures the ring keeps going; a boot lists the newest kMaxEntries, in order
        Vol v;
        for (unsigned k = 0; k < 200; k++) {
            v.idle();
            CHECK_EQ(int(add(*v.r, k)), int(bbstore::Err::Ok));
        }
        const auto seeds = order(*v.r);
        CHECK_EQ(seeds.size(), std::size_t(bbstore::kMaxEntries));
        CHECK_EQ(seeds.back(), 199u);
        CHECK_EQ(seeds.front(), 199 - bbstore::kMaxEntries + 1);
        for (std::size_t i = 1; i < seeds.size(); i++) CHECK_EQ(seeds[i], seeds[i - 1] + 1);
        const auto s = v.r->stats();
        v.reboot();
        CHECK_EQ(v.r->stats().entries, s.entries);
        CHECK_EQ(order(*v.r).back(), 199u);
        v.idle();
        CHECK_EQ(int(add(*v.r, 200)), int(bbstore::Err::Ok));
        CHECK_EQ(order(*v.r).back(), 200u);
    }
    {   // worst case: incompressible captures, one to a sector; the ring holds 20, 19 after the next idle moment (one
        // sector kept blank)
        Vol v;
        const Buf b = noise(1900);
        unsigned n = 0;
        for (unsigned k = 0; k < 40; k++) {
            v.idle();
            if (v.r->append({1, 100, k}, b, g_scratch) == bbstore::Err::Ok) n++;
        }
        CHECK_EQ(n, 40u);
        CHECK_EQ(v.r->stats().entries, int(flash::kBbRingSectors));
        CHECK_EQ(v.idle(), 1);
        CHECK_EQ(v.r->stats().entries, int(flash::kBbRingSectors - bbstore::kSpare));
    }
    {   // too big: random data of a full capture does not fit one record; nothing is written
        Vol v;
        const Buf b = noise(2048);
        const auto ops = v.disk.ops();
        CHECK_EQ(int(v.r->append({1, 100, 1}, b, g_scratch)), int(bbstore::Err::TooBig));
        CHECK_EQ(int(v.r->append({1, 100, 1}, Buf{}, g_scratch)), int(bbstore::Err::TooBig));
        CHECK_EQ(int(v.r->append({1, 100, 1}, sample(1, 1500), std::span<std::uint8_t>(g_scratch).first(16))), int(bbstore::Err::TooBig));
        CHECK_EQ(v.disk.ops(), ops);
    }
    {   // power loss at every program of an append: the old captures survive, writing continues after a reboot
        Vol probe;
        add(*probe.r, 1);
        add(*probe.r, 2);
        const int before = probe.disk.ops();
        add(*probe.r, 3);
        const int ops = probe.disk.ops() - before;
        CHECK_EQ(ops > 0, 1);
        for (int cut = 0; cut <= ops; cut++) {
            Vol v;
            add(*v.r, 1);
            add(*v.r, 2);
            v.disk.cut_after(cut);
            add(*v.r, 3);
            v.disk.revive();
            v.reboot();
            const auto seeds = order(*v.r);
            CHECK_EQ(seeds.size() == 2 || seeds.size() == 3, 1);
            CHECK_EQ(seeds.size() >= 2 && seeds[0] == 1 && seeds[1] == 2, 1);
            v.idle();
            CHECK_EQ(int(add(*v.r, 4)), int(bbstore::Err::Ok));
            CHECK_EQ(order(*v.r).back(), 4u);
        }
    }
    {   // power loss at every program and erase of the maintenance of a full ring: the captures of the other sectors stay,
        // a torn erase leaves a dirty sector the next maintenance erases, then captures fit again
        for (int cut = 0;; cut++) {
            Vol v;
            fill(v, 0);
            const auto before = order(*v.r);
            v.disk.cut_after(cut);
            v.r->maintain(true);
            const bool lost = v.disk.dead();
            v.disk.revive();
            v.reboot();
            const auto after = order(*v.r);
            CHECK_EQ(after.size(), before.size());     // the newest 32 sit far from the oldest sector
            CHECK_EQ(after.back(), before.back());
            v.idle();
            CHECK_EQ(v.ring->dirty(), 0u);
            CHECK_EQ(int(add(*v.r, 210)), int(bbstore::Err::Ok));
            if (!lost) break;
        }
    }
    {   // a flipped bit in a stored capture fails the record CRC: that capture is skipped, the others load
        Vol v;
        add(*v.r, 1);
        add(*v.r, 2);
        long at = -1;
        v.r->each([&](const bbstore::Entry& e) { if (at < 0) at = e.packed.data() - v.disk.image().data(); });
        CHECK_EQ(at > 0, 1);
        v.disk.image()[std::size_t(at) + 20] ^= 0x08;
        v.reboot();
        const auto seeds = order(*v.r);
        CHECK_EQ(seeds.size(), 1u);
        CHECK_EQ(seeds.size() == 1 && seeds[0] == 2, 1);
        CHECK_EQ(v.r->stats().entries, 1);
    }
    {   // another owner's data in a ring sector (a dirty sector) is never read as a capture and is erased by maintenance
        Vol v;
        const std::array<std::uint8_t, 4> junk{0x12, 0x34, 0x56, 0x78};
        CHECK_EQ(v.disk.program(flash::kBbRingBase + 3 * flash::kSectorSize, junk), 1);
        v.reboot();
        CHECK_EQ(v.ring->dirty(), 1u);
        CHECK_EQ(v.r->needs_maintenance(), 1);
        CHECK_EQ(v.r->maintain(true), 0);
        CHECK_EQ(v.ring->dirty(), 0u);
        CHECK_EQ(v.disk.blank(flash::kBbRingBase + 3 * flash::kSectorSize), 1);
    }
    {   // idle gate
        bbstore::IdleGate g;
        CHECK_EQ(g.idle(0), 0);
        CHECK_EQ(g.idle(bbstore::kIdleMs - 1), 0);
        CHECK_EQ(g.idle(bbstore::kIdleMs + 1), 1);
        g.touch(bbstore::kIdleMs + 5);
        CHECK_EQ(g.idle(bbstore::kIdleMs + 6), 0);
        CHECK_EQ(g.idle(2 * bbstore::kIdleMs + 6), 1);
        g.touch(0xFFFFFFF0u);                                  // millis wraps
        CHECK_EQ(g.idle(0x10), 0);
    }
    return test_report("test_bbstore");
}
