// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Volume budget (docs/ARCHITECTURE.md "Volume budget"): what every store of the littlefs volume costs at its cap, and what
// the two ring logs beside it hold. Each littlefs store is filled alone on a large RAM disk (so none runs out) and must stay
// within its row of the table below, the same numbers as the document: a store that grows goes red here first. Then every
// store is filled on one volume of the ring layout (flash::kFsVolumeBlocks, 111 blocks), in the order the device fills it,
// and the journal of the largest plan must still fit. The report cards and the Black Box live in their own rings
// (k2/core/ringlog.hpp): their rows are filled through their own classes on a RAM flash.
//
// Files of stores whose records need a whole subsystem to build (e-marker library, twins, transition captures, loop probe,
// PD stress journal, the small settings files, the log snapshot) are written at their real paths with their real sizes
// through the same Fs::write_file the stores use: littlefs's cost depends on the path, the size and the count only.
// The charger journal is filled through its own class.
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "k2/chargertest/charger_journal.hpp"
#include "k2/chargertest/report_card_store.hpp"
#include "k2/core/bbstore.hpp"
#include "k2/core/rd_intent.hpp"
#include "k2/drivers/flash.hpp"
#include "k2/loopprobe/loop_probe.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/pd/transcap_store.hpp"
#include "k2/pdstress/journal.hpp"
#include "k2/twin/library.hpp"
#include "ramdisk.hpp"
#include "ramflash.hpp"
#include "test_util.h"

using namespace k2;
namespace rc = k2::report_card;
namespace cj = k2::charger_journal;
namespace ct = k2::charger_test;

namespace {

// The small records stay inline (no data block of their own, docs/ARCHITECTURE.md "Volume budget").
static_assert(twin::kMaxFileBytes <= fs::kInlineMax);
static_assert(transcap::kMetaBytes <= fs::kInlineMax);
static_assert(rdintent::kSize <= fs::kInlineMax);
static_assert(emlib::kFileSize <= fs::kInlineMax);

constexpr std::uint32_t kBase = flash::kFsBlocks;           // 90: the volume before it grows
constexpr std::uint32_t kVolume = flash::kFsVolumeBlocks;   // 111: the volume of the ring layout
constexpr std::uint32_t kLarge = 512;                       // one store alone, never out of room
static_assert(kBase == 90 && kVolume == 111);

struct Vol {
    std::uint32_t blocks;
    k2test::RamDisk disk;
    fs::Fs fs;
    std::uint32_t empty = 0;
    explicit Vol(std::uint32_t n) : blocks(n), disk(n), fs(disk, n) {
        bool fm = false;
        fs.mount_or_format(fm);
        empty = fs.used_blocks();
    }
    std::uint32_t used() { return fs.used_blocks(); }
    std::uint32_t free() { return blocks - used(); }
};

fs::Err put_file(fs::Fs& f, const char* path, std::size_t size, std::uint8_t salt) {
    std::vector<std::uint8_t> b(size);
    for (std::size_t i = 0; i < size; ++i) b[i] = std::uint8_t(i * 7 + salt);
    return f.write_file(path, b);
}

// ---- one filler per store: fills it to its cap, returns false when the volume ran out ------------------------------
bool fill_settings(fs::Fs& f) {
    // /usb.cfg, /cable.cfg, /prefs.cfg: 8 B records; /rd.rec 10 B; /.legacy 1 B
    return put_file(f, "/usb.cfg", 8, 1) == fs::Err::Ok && put_file(f, "/cable.cfg", 8, 2) == fs::Err::Ok &&
           put_file(f, "/prefs.cfg", 8, 3) == fs::Err::Ok && put_file(f, rdintent::kPath, rdintent::kSize, 4) == fs::Err::Ok &&
           put_file(f, "/.legacy", 1, 5) == fs::Err::Ok;
}
bool fill_log(fs::Fs& f) { return put_file(f, "/log.txt", 512, 6) == fs::Err::Ok; }   // storage.cpp kLogKeep
bool fill_lp(fs::Fs& f) { return f.mkdir("/lp") == fs::Err::Ok && put_file(f, "/lp/last.rec", loopprobe::kFileBytes, 7) == fs::Err::Ok; }
bool fill_ps(fs::Fs& f) { return f.mkdir("/ps") == fs::Err::Ok && put_file(f, "/ps/run.rec", pdstress::journal::kMaxBytes, 8) == fs::Err::Ok; }
bool fill_twin(fs::Fs& f) {
    if (f.mkdir(twin::kDir) != fs::Err::Ok) return false;
    for (unsigned i = 1; i <= twin::kCapacity; ++i) {
        char p[24];
        std::snprintf(p, sizeof p, "/twin/%02u.twn", i);
        if (put_file(f, p, twin::kMaxFileBytes, std::uint8_t(i)) != fs::Err::Ok) return false;
    }
    return true;
}
bool fill_emk_sized(fs::Fs& f, std::size_t size) {
    if (f.mkdir(emlib::kDir) != fs::Err::Ok) return false;
    for (unsigned i = 1; i <= emlib::kCapacity; ++i) {
        char p[24];
        std::snprintf(p, sizeof p, "/emk/%04u.emk", i);
        if (put_file(f, p, size, std::uint8_t(i)) != fs::Err::Ok) return false;
    }
    return true;
}
bool fill_emk(fs::Fs& f) { return fill_emk_sized(f, emlib::kFileSize); }             // 98 B: a cable without a quirk block
bool fill_emk_quirks(fs::Fs& f) { return fill_emk_sized(f, emlib::kMaxFileSize); }   // 517 B: every cable with a full quirk block
// Transition captures: kMaxStored + 1 pairs (a save writes the new pair before it prunes the oldest). The .tcd is the delta
// stream of a noisy 1920-sample ring, taken as 6 KB (3 data blocks); a quiet capture is much smaller.
constexpr std::size_t kTcdBytes = 6 * 1024;
bool fill_tcap(fs::Fs& f) {
    if (f.mkdir(transcap::kDir) != fs::Err::Ok) return false;
    for (unsigned i = 1; i <= transcap::kMaxStored + 1; ++i) {
        char p[32];
        std::snprintf(p, sizeof p, "/tcap/%08x.tcd", i);
        if (put_file(f, p, kTcdBytes, std::uint8_t(i)) != fs::Err::Ok) return false;
        std::snprintf(p, sizeof p, "/tcap/%08x.tci", i);
        if (put_file(f, p, transcap::kMetaBytes, std::uint8_t(i)) != fs::Err::Ok) return false;
    }
    return true;
}
// The charger journal of the largest plan (kMaxSteps rows over all five sections) with every small record written.
std::vector<ct::Step> max_plan() {
    const ct::Algorithm algs[] = {ct::Algorithm::Pd3Fixed, ct::Algorithm::Pps, ct::Algorithm::Qc3, ct::Algorithm::Vooc, ct::Algorithm::Ufcs};
    const ct::Group groups[] = {ct::Group::Pd, ct::Group::Pd, ct::Group::Qc3, ct::Group::Vooc, ct::Group::Ufcs};
    std::vector<ct::Step> v(ct::kMaxSteps);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i].algorithm = algs[i % 5]; v[i].group = groups[i % 5];
        v[i].pos = std::uint8_t(1 + i % 7); v[i].mv = std::uint16_t(5000 + 100 * i); v[i].ma = 1000; v[i].pdo = 0x1000u + std::uint32_t(i);
    }
    return v;
}
bool fill_journal(fs::Fs& f) {
    cj::Journal j(f);
    auto plan = max_plan();
    const ct::Options opt{.selected = ct::kAllAlgorithms};
    const ct::Evidence ev{};
    if (j.begin(7, opt, ev, plan) != cj::Err::Ok) return false;
    for (std::size_t i = 0; i < plan.size(); ++i) { plan[i].verdict = ct::Verdict::Pass; j.note(plan, i); }
    if (j.settle(7, opt, ev, plan) != cj::Err::Ok) return false;
    return j.phase(ct::Phase::Request, plan, 3) == cj::Err::Ok && j.resumed(2) == cj::Err::Ok && j.meta(cj::RunMeta{}) == cj::Err::Ok &&
           j.attempt(3, 1) == cj::Err::Ok;
}

struct Row {
    const char* name;
    bool (*fill)(fs::Fs&);
    std::uint32_t budget;   // worst-case blocks, docs/ARCHITECTURE.md "Volume budget"
};
// The table of docs/ARCHITECTURE.md, in the order the stores are filled on the shared volume (small first).
const Row kRows[] = {
    {"settings (usb/cable/prefs/rd/.legacy)", fill_settings, 0},
    {"/log.txt", fill_log, 1},
    {"/lp/last.rec", fill_lp, 3},
    {"/ps/run.rec", fill_ps, 3},
    {"/twin (12 x 129 B)", fill_twin, 4},
    {"/tcap (4 + 1 in flight)", fill_tcap, 24},
    {"/emk (140 x 98 B)", fill_emk, 54},
    {"/jr (largest plan)", fill_journal, 7},
};
constexpr Row kEmkQuirks{"/emk (140 x 517 B, every quirk block full)", fill_emk_quirks, 152};
constexpr std::uint32_t kSuperblock = 2;

std::uint32_t measure(const Row& r) {
    Vol v(kLarge);
    const bool ok = r.fill(v.fs);
    const std::uint32_t n = v.used() - v.empty;
    std::printf("[budget] %-42s %3u blocks (budget %u)\n", r.name, n, r.budget);
    CHECK_EQ(ok, 1);
    if (n > r.budget) std::printf("  %s grew past its budget row: update docs/ARCHITECTURE.md \"Volume budget\"\n", r.name);
    CHECK_EQ(n <= r.budget, 1);
    return n;
}

void test_each_store_within_its_budget() {
    std::uint32_t sum = 0, budget = 0;
    for (const Row& r : kRows) { sum += measure(r); budget += r.budget; }
    measure(kEmkQuirks);
    std::printf("[budget] littlefs worst case: superblock %u + stores %u (budget %u) = %u of %u blocks\n", kSuperblock, sum, budget,
                kSuperblock + sum, kVolume);
    CHECK_EQ(kSuperblock + budget <= kVolume, 1);   // the table's sum fits the volume of the ring layout
}

// Every store at its cap on one volume of `blocks`, in the device's order; returns the first that did not fit (nullptr: all
// fit) and the free blocks left.
const Row* all_stores_on(std::uint32_t blocks, std::uint32_t& free) {
    Vol v(blocks);
    const Row* out = nullptr;
    for (const Row& r : kRows)
        if (!r.fill(v.fs)) { out = &r; break; }
    free = v.free();
    std::printf("[budget] all stores at their cap on %u blocks: %s%s; %u free\n", blocks, out ? "ran out at " : "all fit",
                out ? out->name : "", free);
    return out;
}

// The volume of the ring layout holds every littlefs store at its cap (typical e-marker files), the journal of the largest
// plan last, with room to spare; the 90-block base volume does not (pinned: the reason the layout needs the run).
void test_all_stores_on_one_volume() {
    std::uint32_t free = 0;
    CHECK_EQ(all_stores_on(kVolume, free) == nullptr, 1);
    CHECK_EQ(free >= 8, 1);
    CHECK_EQ(all_stores_on(kBase, free) != nullptr, 1);
}

// ---- the rings --------------------------------------------------------------------------------------------------------
rc::Card card(std::uint32_t fp, unsigned variant) {
    rc::Card c;
    c.id.fp = fp; c.fw = 0xABCD; c.uptime_s = fp;
    for (unsigned g = 0; g < 6; ++g) c.group[g] = {rc::GV::Pass, std::uint16_t(9000 + 1000 * ((g + variant) % 4)), 0, 0};
    rc::set_name(c, "Budget charger");
    return c;
}

// The card ring: kMaxCards cards, then 400 re-tests of random cards with new results and idle maintenance between them (the
// steady state of a used device). Every card stays listed, and the live cards take 10 of the 14 sectors at most when
// compact (7 to a sector).
void test_card_ring_budget() {
    k2test::RamFlash disk;
    ringlog::Ring ring(disk, flash::kCardRingBase, flash::kCardRingSectors, rc::kRingKind, rc::kRingReserve);
    ring.scan();
    rc::Store s(ring);
    std::mt19937 rng(11);
    std::vector<std::uint32_t> fps;
    for (unsigned i = 0; i < rc::kMaxCards; ++i) fps.push_back(rng() | 1u);
    unsigned refused = 0;
    const auto put = [&](rc::Card& c) {
        while (s.maintain(true)) {}
        if (s.put(c) != rc::Err::Ok) ++refused;
    };
    for (unsigned i = 0; i < rc::kMaxCards; ++i) { rc::Card c = card(fps[i], 0); put(c); }
    for (unsigned k = 1; k <= 400; ++k) { rc::Card c = card(fps[rng() % fps.size()], k); put(c); }
    while (s.maintain(true)) {}
    CHECK_EQ(refused, 0u);
    CHECK_EQ(s.count(), std::size_t(rc::kMaxCards));
    constexpr unsigned kCompact = (rc::kMaxCards + 6) / 7;
    std::printf("[budget] card ring: %u cards, %u sectors live, %u blank of %u (compact: %u sectors), %llu erases for 464 saves\n",
                unsigned(s.count()), ring.live(), ring.blank(), flash::kCardRingSectors, kCompact, (unsigned long long)disk.erases);
    CHECK_EQ(kCompact, 10u);
    CHECK_EQ(kCompact + rc::kSpare <= flash::kCardRingSectors, 1);
    CHECK_EQ(ring.blank() >= rc::kSpare, 1);
}

// The Black Box ring: captures of a typical size (two to a sector) fill more than the 32 a boot lists; incompressible ones
// (one to a sector, the worst case) keep 19 with the spare sector blank.
void test_bb_ring_budget() {
    const auto run = [](std::size_t raw_bytes, bool random) {
        k2test::RamFlash disk;
        ringlog::Ring ring(disk, flash::kBbRingBase, flash::kBbRingSectors, bbstore::kRingKind);
        ring.scan();
        bbstore::Ring r(ring);
        r.mount();
        std::vector<std::uint8_t> raw(raw_bytes), scratch(lz::bound(raw_bytes));
        std::uint32_t x = 99;
        for (unsigned i = 0; i < 200; ++i) {
            for (std::size_t k = 0; k < raw.size(); ++k) {
                x = x * 1664525u + 1013904223u;
                raw[k] = random ? std::uint8_t(x >> 24) : std::uint8_t((k % 41 < 30 ? k * 3 : i + k / 41) + i);
            }
            while (r.maintain(true) >= 0) {}
            CHECK_EQ(int(r.append({1, 0, i}, raw, scratch)), int(bbstore::Err::Ok));
        }
        while (r.maintain(true) >= 0) {}
        unsigned held = 0;
        ring.each([&](const ringlog::Record&) { ++held; return true; });
        std::printf("[budget] Black Box ring, %s %zu B captures: %u held, %d listed\n", random ? "incompressible" : "typical",
                    raw_bytes, held, r.stats().entries);
        return held;
    };
    CHECK_EQ(run(1500, false) > bbstore::kMaxEntries, 1);
    CHECK_EQ(run(1980, true), flash::kBbRingSectors - bbstore::kSpare);
}

}  // namespace

int main() {
    test_each_store_within_its_budget();
    test_all_stores_on_one_volume();
    test_card_ring_budget();
    test_bb_ring_budget();
    return test_report("test_volume_budget");
}
