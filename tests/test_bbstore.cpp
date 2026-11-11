// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black-box flash ring (k2/core/bbstore.hpp) over a fake flash: append, remount, wrap and eviction,
// torn writes, corruption, region policy and the idle gate.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#include "k2/core/bbstore.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;

namespace {
// One littlefs volume on a RAM disk (what survives a power cycle).
struct Vol {
    k2test::RamDisk disk{96};
    fs::Fs fs{disk, 96};
    Vol() { bool fm = false; fs.mount_or_format(fm); }
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

using Ring = bbstore::Ring;
std::array<std::uint8_t, 4096> g_scratch;

bbstore::Err add(Ring& r, unsigned seed, std::size_t n = 1500, std::uint32_t at_s = 10) {
    const Buf b = sample(seed, n);
    return r.append({1 + seed % 4, 1000, at_s}, b, g_scratch);
}
std::vector<unsigned> order(Ring& r) {
    std::vector<unsigned> seeds;
    r.each([&](const bbstore::Entry& e) {
        Buf out(2048);
        const std::size_t n = r.unpack(e, out);
        for (unsigned s = 0; s < 400; s++)
            if (n == e.raw_len && n >= 1500 && std::memcmp(out.data(), sample(s, n).data(), n) == 0) { seeds.push_back(s); break; }
    });
    return seeds;
}
}  // namespace

int main() {
    {   // layout and policy constants
        CHECK_EQ(bbstore::kMaxEntries, 32u);
        CHECK_EQ(bbstore::kMaxFile, 2044u);                  // one data block of a littlefs file
        CHECK_EQ(bbstore::kFileHeader + bbstore::kMaxPacked, bbstore::kMaxFile);
    }
    {   // empty volume
        Vol f;
        Ring r(f.fs);
        r.mount();
        const auto s = r.stats();
        CHECK_EQ(s.entries, 0);
        CHECK_EQ(s.used_bytes, 0);
        CHECK_EQ(s.free_bytes > 80 * fs::kBlockSize, 1);
        CHECK_EQ(r.next_boot(), 0);
        int n = 0;
        r.each([&](const bbstore::Entry&) { n++; });
        CHECK_EQ(n, 0);
    }
    {   // append, power cycle (a new Fs and Ring over the same disk), read back
        Vol f;
        {
            Ring r(f.fs);
            r.mount();
            CHECK_EQ(int(add(r, 1)), int(bbstore::Err::Ok));
            CHECK_EQ(int(add(r, 2, 900, 55)), int(bbstore::Err::Ok));
        }
        fs::Fs fs2(f.disk, 96);
        fs2.mount();
        Ring r(fs2);
        r.mount();
        CHECK_EQ(r.stats().entries, 2);
        CHECK_EQ(r.next_boot(), 1);
        struct Seen { bbstore::Entry e; Buf packed; };
        std::vector<Seen> es;
        r.each([&](const bbstore::Entry& e) { es.push_back({e, Buf(e.packed.begin(), e.packed.end())}); });
        CHECK_EQ(es.size(), 2);
        CHECK_EQ(es[0].e.trigger, 2);
        CHECK_EQ(es[0].e.raw_len, 1500);
        CHECK_EQ(es[0].e.pre, 1000);
        CHECK_EQ(es[1].e.raw_len, 900);
        CHECK_EQ(es[1].e.at_s, 55);
        CHECK_EQ(es[1].e.boot, 0);
        Buf out(2048);
        bbstore::Entry e1 = es[1].e;
        e1.packed = es[1].packed;
        CHECK_EQ(r.unpack(e1, out), 900);
        const Buf want = sample(2, 900);
        CHECK_EQ(std::memcmp(out.data(), want.data(), 900), 0);
        CHECK_EQ(es[0].e.packed_len < 1500, 1);
        CHECK_EQ(r.stats().used_bytes > 0, 1);
        CHECK_EQ(int(add(r, 3)), int(bbstore::Err::Ok));
        CHECK_EQ(r.stats().entries, 3);
    }
    {   // wrap: the store keeps the newest kMaxEntries captures and evicts the oldest one by one
        Vol f;
        Ring r(f.fs);
        r.mount();
        for (unsigned k = 0; k < 200; k++) CHECK_EQ(int(add(r, k)), int(bbstore::Err::Ok));
        const auto seeds = order(r);
        CHECK_EQ(seeds.size(), bbstore::kMaxEntries);
        CHECK_EQ(seeds.back(), 199);                       // newest kept
        CHECK_EQ(seeds.front(), 199 - bbstore::kMaxEntries + 1);
        for (std::size_t i = 1; i < seeds.size(); i++) CHECK_EQ(seeds[i], seeds[i - 1] + 1);   // contiguous, in order
        const auto s = r.stats();
        CHECK_EQ(s.entries, int(seeds.size()));
        // a remount finds the same newest entry and the same count
        fs::Fs fs2(f.disk, 96);
        fs2.mount();
        Ring r2(fs2);
        r2.mount();
        CHECK_EQ(r2.stats().entries, s.entries);
        CHECK_EQ(order(r2).back(), 199);
        CHECK_EQ(int(add(r2, 200)), int(bbstore::Err::Ok));
        CHECK_EQ(order(r2).back(), 200);
    }
    {   // a full volume evicts the oldest capture instead of failing (no reserve: the NoSpace path of the write itself)
        Vol f;
        Ring r(f.fs);
        r.set_reserve(0);
        r.mount();
        for (unsigned k = 0; k < 10; k++) add(r, k);
        // another store takes most of the volume
        std::vector<std::uint8_t> blob(2000, 0x5A);
        char name[16];
        int n = 0;
        for (; n < 200; n++) {
            std::snprintf(name, sizeof name, "/x%d", n);
            if (f.fs.write_file(name, blob) != fs::Err::Ok) break;
        }
        CHECK_EQ(n > 10, 1);
        CHECK_EQ(int(add(r, 50)), int(bbstore::Err::Ok));
        const auto seeds = order(r);
        CHECK_EQ(seeds.back(), 50);
        CHECK_EQ(seeds.size() < 11, 1);
    }
    {   // reserve: the ring leaves the journal its blocks. It evicts its own oldest captures to stay above the reserve
        // and never takes the free blocks below it, however full the other stores make the volume.
        constexpr std::uint32_t kReserve = 20;
        Vol f;
        Ring r(f.fs);
        r.set_reserve(kReserve);
        r.mount();
        for (unsigned k = 0; k < 10; k++) CHECK_EQ(int(add(r, k)), int(bbstore::Err::Ok));
        std::vector<std::uint8_t> blob(2000, 0x5A);
        char name[16];
        for (int n = 0; n < 200; n++) {                                  // another store fills the volume down to reserve + 3
            if (f.fs.block_count() - f.fs.used_blocks() <= kReserve + 3) break;
            std::snprintf(name, sizeof name, "/x%d", n);
            if (f.fs.write_file(name, blob) != fs::Err::Ok) break;
        }
        const std::uint32_t free0 = f.fs.block_count() - f.fs.used_blocks();
        CHECK_EQ(free0 <= kReserve + 3, 1);
        for (unsigned k = 20; k < 40; k++) {
            const auto e = add(r, k);
            CHECK_EQ(int(e), int(bbstore::Err::Ok));
            CHECK_EQ(f.fs.block_count() - f.fs.used_blocks() >= kReserve, 1);
        }
        CHECK_EQ(order(r).back(), 39);
        CHECK_EQ(r.stats().entries <= 12, 1);                             // 20 saves, and the ring hardly grew: each gave one back
    }
    {   // reserve: nothing to evict and still under it = Full, and the disk is not touched
        Vol f;
        Ring r(f.fs);
        r.set_reserve(20);
        r.mount();
        std::vector<std::uint8_t> blob(2000, 0x5A);
        char name[16];
        for (int n = 0; n < 200; n++) {
            if (f.fs.block_count() - f.fs.used_blocks() <= 10) break;
            std::snprintf(name, sizeof name, "/x%d", n);
            if (f.fs.write_file(name, blob) != fs::Err::Ok) break;
        }
        const auto progs = f.disk.progs, erases = f.disk.erases;
        CHECK_EQ(int(add(r, 1)), int(bbstore::Err::Full));
        CHECK_EQ(f.disk.progs, progs);
        CHECK_EQ(f.disk.erases, erases);
        CHECK_EQ(r.stats().entries, 0);
    }
    {   // the default reserve is the journal's room
        CHECK_EQ(bbstore::kJournalReserveBlocks, 20u);
    }
    {   // Full is cheap: on a volume the cards filled (hardware 2026-11-11: 61 cards, 5 free blocks, no capture of its own) the
        // ring refused every capture after compressing it and walking the whole volume, 41 ms with the loop stopped, twice per
        // VBUS cut. Now it refuses before the compression (the scratch buffer is never written) and, with nothing written in
        // between, without a second walk (Fs caches used_blocks).
        k2test::RamDisk disk{90};
        fs::Fs vol{disk, 90};
        bool fm = false;
        vol.mount_or_format(fm);
        Ring r(vol);
        r.mount();
        CHECK_EQ(vol.mkdir("/cards") == fs::Err::Ok, 1);
        std::vector<std::uint8_t> card(264, 0xC4);
        char name[32];
        for (unsigned n = 0; n < 64; n++) {                               // a volume of 264-byte cards, one data block each
            if (vol.block_count() - vol.used_blocks() <= 6) break;
            std::snprintf(name, sizeof name, "/cards/%08x.card", 0x1000 + n);
            CHECK_EQ(vol.write_file(name, card) == fs::Err::Ok, 1);
        }
        std::vector<std::uint8_t> blob(2000, 0x5A);
        for (int n = 0; n < 40 && vol.block_count() - vol.used_blocks() > 6; n++) {
            std::snprintf(name, sizeof name, "/x%d", n);
            CHECK_EQ(vol.write_file(name, blob) == fs::Err::Ok, 1);
        }
        CHECK_EQ(vol.block_count() - vol.used_blocks() <= 6, 1);
        CHECK_EQ(vol.write_file("/log.txt", blob) == fs::Err::Ok, 1);   // another store wrote last: the next figure is stale
        disk.reset_counters();
        vol.used_blocks();
        const auto walk = disk.read_bytes;                                // what one walk of this volume reads
        CHECK_EQ(vol.write_file("/log.txt", blob) == fs::Err::Ok, 1);
        g_scratch.fill(0xEE);
        disk.reset_counters();
        CHECK_EQ(int(add(r, 1)), int(bbstore::Err::Full));
        bool untouched = true;
        for (auto b : g_scratch) untouched = untouched && b == 0xEE;
        CHECK_EQ(untouched, 1);                                           // refused before lz::compress
        CHECK_EQ(disk.progs + disk.erases, 0u);
        disk.reset_counters();
        CHECK_EQ(int(add(r, 2)), int(bbstore::Err::Full));                // the second capture of the same cut
        if (disk.read_bytes * 4 >= walk) std::printf("  refusal read %llu bytes, a walk %llu\n", (unsigned long long)disk.read_bytes, (unsigned long long)walk);
        CHECK_EQ(disk.read_bytes * 4 < walk, 1);                          // no walk: a fraction of one
        for (auto b : g_scratch) untouched = untouched && b == 0xEE;
        CHECK_EQ(untouched, 1);
    }
    {   // too big: random data of a full capture does not fit one block; nothing is written
        Vol f;
        Ring r(f.fs);
        r.mount();
        const Buf b = noise(2048);
        f.disk.reset_counters();
        CHECK_EQ(int(r.append({1, 100, 1}, b, g_scratch)), int(bbstore::Err::TooBig));
        CHECK_EQ(f.disk.progs, 0);
        CHECK_EQ(int(r.append({1, 100, 1}, Buf{}, g_scratch)), int(bbstore::Err::TooBig));
    }
    {   // power loss at every operation of an append: the old captures survive, writing continues
        Vol probe;
        {
            Ring r(probe.fs);
            r.mount();
            add(r, 1);
            add(r, 2);
            probe.disk.reset_counters();
            add(r, 3);
        }
        const int ops = probe.disk.ops_done();
        CHECK_EQ(ops > 0, 1);
        for (int cut = 0; cut <= ops; cut++) {
            Vol f;
            {
                Ring r(f.fs);
                r.mount();
                add(r, 1);
                add(r, 2);
                f.disk.cut_after(cut);
                add(r, 3);
            }
            f.disk.revive();
            fs::Fs fs2(f.disk, 96);
            bool fm = false;
            fs2.mount_or_format(fm);
            Ring r2(fs2);
            r2.mount();
            const auto seeds = order(r2);
            CHECK_EQ(seeds.size() == 2 || seeds.size() == 3, 1);
            CHECK_EQ(seeds[0], 1);
            CHECK_EQ(seeds[1], 2);
            CHECK_EQ(int(add(r2, 4)), int(bbstore::Err::Ok));
            CHECK_EQ(order(r2).back(), 4);
        }
    }
    {   // a flipped byte in a stored file fails the checksum: the capture is skipped
        Vol f;
        Ring r(f.fs);
        r.mount();
        add(r, 1);
        add(r, 2);
        std::vector<std::uint8_t> raw(2048);
        std::size_t n = 0;
        CHECK_EQ(int(f.fs.read_file("/bb/00000001.bbe", raw, n)), int(fs::Err::Ok));
        raw[30] ^= 0x08;
        CHECK_EQ(int(f.fs.write_file("/bb/00000001.bbe", {raw.data(), n})), int(fs::Err::Ok));
        Ring r2(f.fs);
        r2.mount();
        const auto seeds = order(r2);
        CHECK_EQ(seeds.size(), 1);
        CHECK_EQ(seeds[0], 2);
    }
    {   // clear removes every capture
        Vol f;
        Ring r(f.fs);
        r.mount();
        for (unsigned k = 0; k < 10; k++) add(r, k);
        CHECK_EQ(int(r.clear()), int(bbstore::Err::Ok));
        CHECK_EQ(r.stats().entries, 0);
        Ring r2(f.fs);
        r2.mount();
        CHECK_EQ(r2.stats().entries, 0);
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
