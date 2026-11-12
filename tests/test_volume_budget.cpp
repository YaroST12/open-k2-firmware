// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Volume budget (docs/ARCHITECTURE.md "Volume budget"): what every store of the 90-block littlefs volume costs at its cap.
// Each store is filled alone on a large RAM disk (so none runs out) and must stay within its row of the table below, the
// same numbers as the document: a store that grows goes red here first. Then every store is filled on one 90-block volume,
// in the order the device fills it, and the Black Box gets what is left above the journal reserve.
//
// Files of stores whose records need a whole subsystem to build (e-marker library, twins, transition captures, loop probe,
// PD stress journal and saved results, the small settings files, the log snapshot) are written at their real paths with their real sizes
// through the same Fs::write_file the stores use: littlefs's cost depends on the path, the size and the count only.
// The cards, the charger journal and the Black Box are filled through their own classes.
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "k2/chargertest/charger_journal.hpp"
#include "k2/chargertest/report_card_store.hpp"
#include "k2/core/bbstore.hpp"
#include "k2/core/rd_intent.hpp"
#include "k2/loopprobe/loop_probe.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/pd/transcap_store.hpp"
#include "k2/pdstress/journal.hpp"
#include "k2/pdstress/pdstress_store.hpp"
#include "k2/twin/library.hpp"
#include "ramdisk.hpp"
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
// Open: a v10 card file is 8 bytes over the limit and takes a data block each (open item in docs/ARCHITECTURE.md "Volume budget").
static_assert(report_card::kFileSize == fs::kInlineMax + 8);

constexpr std::uint32_t kBlocks = 90;      // fs_flash::kFlashBlocks
constexpr std::uint32_t kGrownBlocks = 145;   // fs_flash::kGrownBlocks: grown into the blank run (k2/core/fs_grow.hpp)
constexpr std::uint32_t kLarge = 512;      // one store alone, never out of room

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
// PD Stress saved results: kMaxSaved blobs plus the one in flight (a save writes the new result before it drops the oldest).
// The directory is the journal's, so it may already be there on the shared volume.
bool fill_psr(fs::Fs& f) {
    const fs::Err d = f.mkdir(pdstress::store::kDir);
    if (d != fs::Err::Ok && d != fs::Err::Exists) return false;
    for (std::uint16_t id = 1; id <= pdstress::store::kMaxSaved + 1; ++id) {
        std::array<char, pdstress::store::kPathLen> p{};
        pdstress::store::path_of(id, p);
        if (put_file(f, p.data(), pdstress::store::kFileBytes, std::uint8_t(id)) != fs::Err::Ok) return false;
    }
    return true;
}
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
rc::Card card(std::uint32_t fp, unsigned variant) {
    rc::Card c;
    c.id.fp = fp; c.fw = 0xABCD; c.uptime_s = fp;
    for (unsigned g = 0; g < 6; ++g) c.group[g] = {rc::GV::Pass, std::uint16_t(9000 + 1000 * ((g + variant) % 4)), 0, 0};
    rc::set_name(c, "Budget charger");
    return c;
}
// kMaxCards cards, then 100 re-tests of random cards with new results (the steady state of a used device: every re-test
// rewrites its card).
bool fill_cards(fs::Fs& f) {
    rc::Store s(f);
    std::mt19937 rng(11);
    std::vector<std::uint32_t> fps;
    for (unsigned i = 0; i < rc::kMaxCards; ++i) fps.push_back(rng() | 1u);
    for (unsigned i = 0; i < rc::kMaxCards; ++i) {
        rc::Card c = card(fps[i], 0);
        if (s.put(c) != rc::Err::Ok) return false;
    }
    for (unsigned k = 1; k <= 100; ++k) {
        rc::Card c = card(fps[rng() % fps.size()], k);
        if (s.put(c) != rc::Err::Ok) return false;
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
// Black Box captures that pack to one data block each (the largest file the ring writes); returns how many were saved.
unsigned fill_bb(fs::Fs& f, std::uint32_t reserve) {
    bbstore::Ring r(f);
    r.set_reserve(reserve);
    r.mount();
    std::vector<std::uint8_t> raw(1900);
    std::vector<std::uint8_t> scratch(lz::bound(raw.size()));
    std::uint32_t x = 99;
    unsigned saved = 0;
    for (unsigned i = 0; i < bbstore::kMaxEntries; ++i) {
        for (auto& b : raw) { x = x * 1664525u + 1013904223u; b = std::uint8_t(x >> 24); }
        if (r.append({1, 0, i}, raw, scratch) != bbstore::Err::Ok) break;
        ++saved;
    }
    return saved;
}

struct Row {
    const char* name;
    bool (*fill)(fs::Fs&);
    std::uint32_t budget;   // worst-case blocks, docs/ARCHITECTURE.md "Volume budget"
};
// The table of docs/ARCHITECTURE.md, in the order the stores are filled on the shared volume (small first, cards last).
const Row kRows[] = {
    {"settings (usb/cable/prefs/rd/.legacy)", fill_settings, 0},
    {"/log.txt", fill_log, 1},
    {"/lp/last.rec", fill_lp, 3},
    {"/ps/run.rec", fill_ps, 3},
    {"/ps results (8 x 1376 B + 1 in flight)", fill_psr, 11},
    {"/twin (12 x 129 B)", fill_twin, 4},
    {"/tcap (4 + 1 in flight)", fill_tcap, 24},
    {"/emk (140 x 98 B)", fill_emk, 54},
    {"/cards (64 x 264 B, after re-tests)", fill_cards, 70},
};
constexpr Row kEmkQuirks{"/emk (140 x 517 B, every quirk block full)", fill_emk_quirks, 152};
constexpr std::uint32_t kJournalBudget = 7;    // /jr of the largest plan; bbstore::kJournalReserveBlocks keeps 20 free for it
constexpr std::uint32_t kBbBudget = 34;        // /bb, 32 captures of one block each

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
    std::uint32_t sum = 0;
    for (const Row& r : kRows) sum += measure(r);
    measure(kEmkQuirks);
    {
        Vol v(kLarge);
        CHECK_EQ(fill_journal(v.fs), 1);
        const std::uint32_t n = v.used() - v.empty;
        std::printf("[budget] %-42s %3u blocks (budget %u, reserve %u)\n", "/jr (largest plan)", n, kJournalBudget, bbstore::kJournalReserveBlocks);
        CHECK_EQ(n <= kJournalBudget, 1);
        CHECK_EQ(kJournalBudget <= bbstore::kJournalReserveBlocks, 1);
    }
    {
        Vol v(kLarge);
        const unsigned saved = fill_bb(v.fs, 0);
        const std::uint32_t n = v.used() - v.empty;
        std::printf("[budget] %-42s %3u blocks (budget %u)\n", "/bb (32 captures)", n, kBbBudget);
        CHECK_EQ(saved, bbstore::kMaxEntries);
        CHECK_EQ(n <= kBbBudget, 1);
    }
    std::printf("[budget] worst case: superblock 2 + stores %u + journal reserve %u + Black Box %u = %u of %u blocks\n", sum,
                bbstore::kJournalReserveBlocks, kBbBudget, 2 + sum + bbstore::kJournalReserveBlocks + kBbBudget, kBlocks);
}

struct Picture {
    const Row* ran_out = nullptr;   // the first store that did not fit, nullptr = all fit
    std::uint32_t free_before_bb = 0;
    unsigned saved = 0;             // Black Box captures above the journal reserve
};

// Every store at its cap on one volume of `blocks`, in the device's order: where the volume runs out, and how many captures
// the Black Box gets above the journal reserve.
Picture all_stores_on(std::uint32_t blocks) {
    Vol v(blocks);
    Picture p;
    for (const Row& r : kRows) {
        if (!r.fill(v.fs)) { p.ran_out = &r; break; }
    }
    p.free_before_bb = v.free();
    p.saved = fill_bb(v.fs, bbstore::kJournalReserveBlocks);
    std::printf("[budget] all stores at their cap on %u blocks: %s%s; %u free; the Black Box saved %u of %u captures\n", blocks,
                p.ran_out ? "ran out at " : "all fit", p.ran_out ? p.ran_out->name : "", p.free_before_bb, p.saved, bbstore::kMaxEntries);
    return p;
}

// The cards alone at their cap after re-tests, every other store empty, then the Black Box above the journal reserve.
Picture cards_alone_on(std::uint32_t blocks) {
    Vol v(blocks);
    Picture p;
    CHECK_EQ(fill_cards(v.fs), 1);
    p.free_before_bb = v.free();
    p.saved = fill_bb(v.fs, bbstore::kJournalReserveBlocks);
    std::printf("[budget] 64 cards alone on %u blocks: %u free (reserve %u); the Black Box saved %u of %u captures\n", blocks,
                p.free_before_bb, bbstore::kJournalReserveBlocks, p.saved, bbstore::kMaxEntries);
    return p;
}

// Today's caps do not fit the 90-block volume (docs/ARCHITECTURE.md "Volume budget"). Pinned, so a change to any cap or
// format shows up here and the document follows it.
void test_all_stores_on_one_volume() {
    const Picture p = all_stores_on(kBlocks);
    CHECK_EQ(p.ran_out != nullptr, 1);
    CHECK_EQ(p.saved, 0u);
}

// 64 card files leave fewer free blocks than the journal reserve on 90 blocks: the Black Box gets nothing. This is the
// state the device reached with 61 cards.
void test_cards_alone_starve_the_black_box() {
    const Picture p = cards_alone_on(kBlocks);
    CHECK_EQ(p.free_before_bb < bbstore::kJournalReserveBlocks + 1, 1);
    CHECK_EQ(p.saved, 0u);
}

// The grown volume (145 blocks, k2/core/fs_grow.hpp) at today's caps: every store in device order still does not fit (the
// e-marker library and the cards alone ask for more than the 55 new blocks give), but 64 cards, the journal reserve and all
// 32 Black Box captures do.
void test_grown_volume_picture() {
    const Picture all = all_stores_on(kGrownBlocks);
    CHECK_EQ(all.ran_out != nullptr, 1);
    const Picture cards = cards_alone_on(kGrownBlocks);
    CHECK_EQ(cards.free_before_bb >= bbstore::kJournalReserveBlocks + kBbBudget, 1);
    CHECK_EQ(cards.saved, bbstore::kMaxEntries);
}

}  // namespace

int main() {
    test_each_store_within_its_budget();
    test_all_stores_on_one_volume();
    test_cards_alone_starve_the_black_box();
    test_grown_volume_picture();
    return test_report("test_volume_budget");
}
