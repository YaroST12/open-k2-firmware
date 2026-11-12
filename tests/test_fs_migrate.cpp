// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the one-time move of a wide volume (145 blocks) into the ring layout (k2/core/fs_migrate.hpp): the boot
// sequence of src/drivers/fs_flash.cpp (mount, then migrate a wide volume) over a RAM flash that is both the littlefs block
// device and the rings' flash, with a power cut at every program and erase. Every kept file comes back byte for byte, the
// report cards and Black Box captures are gone, the volume ends at 111 blocks and the ring range ends blank (or holds only
// sectors the rings' maintenance erases).
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "k2/chargertest/report_card_store.hpp"
#include "k2/core/bbstore.hpp"
#include "k2/core/fs.hpp"
#include "k2/core/fs_grow.hpp"
#include "k2/core/fs_migrate.hpp"
#include "k2/drivers/flash.hpp"
#include "ramflash.hpp"
#include "test_util.h"

using namespace k2;
using k2test::RamFlash;
namespace mig = k2::fsmigrate;

namespace {

constexpr std::uint32_t kBase = flash::kFsBlocks;          // 90
constexpr std::uint32_t kVolume = flash::kFsVolumeBlocks;  // 111
constexpr std::uint32_t kWide = flash::kFsGrownBlocks;     // 145

using Snapshot = std::map<std::string, std::string>;   // path -> contents ("<dir>" for a directory)

std::string content(std::size_t n, unsigned salt) {
    std::string s(n, '\0');
    std::uint32_t x = salt * 2654435761u + 1;
    for (auto& c : s) { x = x * 1664525u + 1013904223u; c = char(x >> 24); }
    return s;
}
std::span<const std::uint8_t> sv(const std::string& s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }

void snap(fs::Fs& f, const std::string& dir, Snapshot& out) {
    std::vector<std::pair<std::string, bool>> kids;
    f.list(dir.c_str(), [&](const fs::Info& i) { kids.emplace_back((dir == "/" ? "/" : dir + "/") + i.name, i.dir); return true; });
    for (const auto& [p, d] : kids) {
        if (d) { out[p] = "<dir>"; snap(f, p, out); continue; }
        std::vector<std::uint8_t> buf(32 * 1024);
        std::size_t n = 0;
        out[p] = f.read_file(p.c_str(), buf, n) == fs::Err::Ok ? std::string(reinterpret_cast<char*>(buf.data()), n) : "<unreadable>";
    }
}
Snapshot snapshot(fs::Fs& f) { Snapshot s; snap(f, "/", s); return s; }
// The snapshot without the paths the move drops.
Snapshot without(Snapshot s, std::initializer_list<const char*> gone) {
    for (auto it = s.begin(); it != s.end();) {
        bool drop = false;
        for (const char* g : gone) drop |= it->first == g || it->first.rfind(std::string(g) + "/", 0) == 0;
        it = drop ? s.erase(it) : std::next(it);
    }
    return s;
}

// What a unit grown before the ring layout carries: every kept store (a nested directory too), the cards and captures,
// after enough churn that littlefs has spread its blocks over the whole run (so kept blocks sit in the ring range).
// `emk` files of `emk_bytes` make the e-marker library.
void make_wide(RamFlash& d, unsigned emk = 40, std::size_t emk_bytes = 98) {
    fs::Fs f(d, kWide);
    CHECK_EQ(f.format() == fs::Err::Ok, 1);
    CHECK_EQ(f.mount() == fs::Err::Ok, 1);
    for (unsigned i = 0; i < 60; ++i) {   // churn: the allocator walks on through the run
        char p[16];
        std::snprintf(p, sizeof p, "/churn%u", i);
        f.write_file(p, sv(content(2048, 900 + i)));
        if (i >= 4) { std::snprintf(p, sizeof p, "/churn%u", i - 4); f.remove(p); }
    }
    for (unsigned i = 56; i < 60; ++i) { char p[16]; std::snprintf(p, sizeof p, "/churn%u", i); f.remove(p); }
    f.write_file("/usb.cfg", sv(content(8, 1)));
    f.write_file("/prefs.cfg", sv(content(8, 2)));
    f.write_file("/cable.cfg", sv(content(8, 3)));
    f.write_file("/rd.rec", sv(content(10, 4)));
    f.write_file(fsgrow::kLegacyMarker, sv("1"));
    f.write_file("/log.txt", sv(content(512, 5)));
    f.mkdir("/emk");
    for (unsigned i = 1; i <= emk; ++i) { char p[24]; std::snprintf(p, sizeof p, "/emk/%04u.emk", i); f.write_file(p, sv(content(emk_bytes, 100 + i))); }
    f.mkdir("/twin");
    for (unsigned i = 1; i <= 5; ++i) { char p[24]; std::snprintf(p, sizeof p, "/twin/%02u.twn", i); f.write_file(p, sv(content(129, 300 + i))); }
    f.mkdir("/jr");
    f.write_file("/jr/run.rec", sv(content(5000, 400)));
    f.write_file("/jr/rows.rec", sv(content(700, 401)));
    f.mkdir("/jr/old");
    f.write_file("/jr/old/a.rec", sv(content(2500, 403)));
    f.mkdir("/ps");
    f.write_file("/ps/run.rec", sv(content(3000, 402)));
    f.mkdir("/tcap");
    for (unsigned i = 1; i <= 4; ++i) {
        char p[32];
        std::snprintf(p, sizeof p, "/tcap/%08x.tcd", i); f.write_file(p, sv(content(6 * 1024, 500 + i)));
        std::snprintf(p, sizeof p, "/tcap/%08x.tci", i); f.write_file(p, sv(content(64, 600 + i)));
    }
    f.mkdir("/cards");
    for (unsigned i = 1; i <= 64; ++i) { char p[24]; std::snprintf(p, sizeof p, "/cards/%04u.rc", i); f.write_file(p, sv(content(274, 700 + i))); }
    f.mkdir("/bb");
    for (unsigned i = 1; i <= 12; ++i) { char p[24]; std::snprintf(p, sizeof p, "/bb/%04u.bb", i); f.write_file(p, sv(content(1900, 800 + i))); }
    f.unmount();
}

Snapshot read_wide(RamFlash& d) {
    fs::Fs f(d, kWide);
    f.mount();
    return snapshot(f);
}

struct Env {
    RamFlash& d;
    bool blank(std::uint32_t a) { return d.blank(a); }
    void open_run() {}
};
static_assert(fsgrow::Env<Env>);

struct Boot {
    mig::Result mig;
    bool formatted = false;
    fs::Err err = fs::Err::Ok;
};

// The boot of src/drivers/fs_flash.cpp init(): the volume mounts, a wide one is moved into the ring layout.
Boot boot(RamFlash& d, fs::Fs& g) {
    Boot b;
    Env env{d};
    b.err = fsgrow::mount(g, env, b.formatted);
    if (b.err == fs::Err::Ok && g.wide()) {
        std::vector<std::uint8_t> buf(2048);
        b.mig = mig::migrate(g, d, buf);
        if (!g.ready()) b.err = g.mount();
    }
    return b;
}

bool ring_range_blank(RamFlash& d) {
    for (unsigned i = 0; i < flash::kRingSectors; ++i)
        if (!d.blank(flash::kRingBase + i * flash::kSectorSize)) return false;
    return true;
}

// The rings come up over what the move left: nothing live, and their maintenance erases every leftover sector.
void rings_clean_up(RamFlash& d) {
    ringlog::Ring cr(d, flash::kCardRingBase, flash::kCardRingSectors, report_card::kRingKind, report_card::kRingReserve);
    ringlog::Ring br(d, flash::kBbRingBase, flash::kBbRingSectors, bbstore::kRingKind);
    cr.scan();
    br.scan();
    CHECK_EQ(cr.live(), 0u);
    CHECK_EQ(br.live(), 0u);
    report_card::Store cs(cr);
    while (cs.maintain(true)) {}
    bbstore::Ring bs(br);
    bs.mount();
    while (bs.maintain(true) >= 0) {}
    CHECK_EQ(ring_range_blank(d), 1);
}

// A unit of the old layout boots once: its files are rewritten below 111, the cards and captures are dropped, the volume
// shrinks to 111 and the ring range is left blank. The next boot finds an ordinary 111-block volume.
void test_wide_volume_moves() {
    RamFlash d;
    make_wide(d);
    const Snapshot before = read_wide(d);
    CHECK_EQ(before.count("/cards/0001.rc"), 1u);
    const Snapshot want = without(before, {"/cards", "/bb"});
    fs::Fs g(d, kBase, kVolume, kWide);
    d.progs = d.erases = 0;
    const Boot b = boot(d, g);
    std::printf("[migrate] used=%u above=%u passes=%u files=%u erased=%u: %llu programs, %llu erases\n", unsigned(b.mig.used_blocks),
                unsigned(b.mig.above), b.mig.passes, b.mig.files, b.mig.erased, (unsigned long long)d.progs, (unsigned long long)d.erases);
    CHECK_EQ(int(b.mig.outcome), int(mig::Outcome::Migrated));
    CHECK_EQ(b.mig.resumed, 0);
    CHECK_EQ(b.mig.tight, 0);
    CHECK_EQ(b.mig.above > 4, 1);     // littlefs held blocks in the ring range: the move had work to do
    CHECK_EQ(b.mig.passes, 1u);       // one pass empties it
    CHECK_EQ(b.mig.files, 9u);        // the files larger than an inline record: log.txt, 3 in jr, 1 in ps, 4 in tcap
    CHECK_EQ(int(b.err), int(fs::Err::Ok));
    CHECK_EQ(g.block_count(), kVolume);
    CHECK_EQ(g.grown(), 1);
    CHECK_EQ(g.used_from(kVolume), 0u);
    CHECK_EQ(snapshot(g) == want, 1);
    CHECK_EQ(ring_range_blank(d), 1);
    fs::Fs h(d, kBase, kVolume, kWide);
    const Boot again = boot(d, h);
    CHECK_EQ(int(again.mig.outcome), int(mig::Outcome::None));
    CHECK_EQ(again.formatted, 0);
    CHECK_EQ(h.block_count(), kVolume);
    CHECK_EQ(snapshot(h) == want, 1);
}

// A wide volume that never used the top blocks (a unit that grew just before this layout) shrinks without a rewrite.
void test_unused_top_shrinks_at_once() {
    RamFlash d;
    {
        fs::Fs f(d, kWide);
        CHECK_EQ(f.format() == fs::Err::Ok, 1);
        CHECK_EQ(f.mount() == fs::Err::Ok, 1);
        f.write_file("/usb.cfg", sv(content(8, 1)));
        f.mkdir("/emk");
        f.write_file("/emk/0001.emk", sv(content(98, 2)));
        CHECK_EQ(f.used_from(kVolume), 0u);
    }
    const Snapshot want = read_wide(d);
    fs::Fs g(d, kBase, kVolume, kWide);
    const Boot b = boot(d, g);
    CHECK_EQ(int(b.mig.outcome), int(mig::Outcome::Migrated));
    CHECK_EQ(b.mig.passes, 0u);
    CHECK_EQ(b.mig.files, 0u);
    CHECK_EQ(g.block_count(), kVolume);
    CHECK_EQ(snapshot(g) == want, 1);
}

// After a cut at any program or erase, the next boot (and, for a second cut during that boot, the one after it) ends on
// a 111-block volume holding exactly the kept files; what is left in the ring range is only for the rings to erase.
void sweep(const std::vector<std::uint8_t>& image, const Snapshot& want, int stride, int second_stride) {
    int total = 0;
    {
        RamFlash d;
        d.image() = image;
        fs::Fs g(d, kBase, kVolume, kWide);
        boot(d, g);
        total = d.ops();
    }
    unsigned runs = 0, resumed = 0, again = 0, shrunk = 0;
    for (int cut = 0; cut <= total; cut += stride) {
        RamFlash d;
        d.image() = image;
        {
            fs::Fs g(d, kBase, kVolume, kWide);
            d.cut_after(cut);
            boot(d, g);
        }
        d.revive();
        const std::vector<std::uint8_t> after_cut = d.image();
        const int last2 = second_stride ? total : -1;
        for (int cut2 = second_stride ? 0 : -1; cut2 <= last2; cut2 += second_stride ? second_stride : 1) {
            RamFlash e;
            e.image() = after_cut;
            if (cut2 >= 0) {
                {
                    fs::Fs g(e, kBase, kVolume, kWide);
                    e.cut_after(cut2);
                    boot(e, g);
                }
                const bool finished = e.ops() <= cut2;
                e.revive();
                if (finished) break;   // the boot ended before the second cut: later cut points add nothing
            }
            fs::Fs g(e, kBase, kVolume, kWide);
            const Boot b = boot(e, g);
            ++runs;
            resumed += b.mig.resumed;
            again += b.mig.outcome == mig::Outcome::Migrated;
            shrunk += b.mig.outcome == mig::Outcome::None;
            CHECK_EQ(int(b.err), int(fs::Err::Ok));
            CHECK_EQ(b.formatted, 0);   // never a format: every cut leaves the wide volume or the shrunk one
            CHECK_EQ(g.block_count(), kVolume);
            const Snapshot got = snapshot(g);
            const bool same = got == want;
            if (!same || g.block_count() != kVolume) {
                std::printf("  cut %d / %d: outcome=%u err=%s resumed=%d passes=%u above=%u blocks=%u\n", cut, cut2, unsigned(b.mig.outcome),
                            fs::err_name(b.mig.err), int(b.mig.resumed), b.mig.passes, unsigned(b.mig.above), unsigned(g.block_count()));
                for (const auto& [p, c] : want)
                    if (!got.count(p) || got.at(p) != c) std::printf("    %s: %s\n", p.c_str(), got.count(p) ? "differs" : "missing");
                for (const auto& [p, c] : got)
                    if (!want.count(p)) std::printf("    %s: extra\n", p.c_str());
            }
            CHECK_EQ(same, 1);
            rings_clean_up(e);
        }
    }
    std::printf("[migrate] power-cut sweep: %d operations, cut every %d%s: %u boots checked (%u moved again, %u of them "
                "settling leftovers; %u found the volume shrunk)\n",
                total, stride, second_stride ? " plus a second cut" : "", runs, again, resumed, shrunk);
    CHECK_EQ(again > 0, 1);
    CHECK_EQ(resumed > 0, 1);
    CHECK_EQ(shrunk > 0, 1);
}

void test_power_cut_sweep() {
    RamFlash d;
    make_wide(d);
    const Snapshot want = without(read_wide(d), {"/cards", "/bb"});
    // A cut at every operation takes about 16 s here (littlefs at -O0): K2_FULL_SWEEP=1 runs it, with a denser second
    // cut; the gate runs every 7th operation and a coarse second cut.
    const bool full = std::getenv("K2_FULL_SWEEP") != nullptr;
    sweep(d.image(), want, full ? 1 : 7, 0);
    sweep(d.image(), want, full ? 37 : 151, full ? 11 : 53);
}

// When the kept files do not fit 111 blocks with the margin, the transition captures and the log snapshot go too.
void test_tight_volume_drops_tcap_and_log() {
    RamFlash d;
    make_wide(d, 70, 517);
    const Snapshot before = read_wide(d);
    fs::Fs g(d, kBase, kVolume, kWide);
    const Boot b = boot(d, g);
    std::printf("[migrate] tight: outcome=%u used=%u above=%u passes=%u\n", unsigned(b.mig.outcome), unsigned(b.mig.used_blocks),
                unsigned(b.mig.above), b.mig.passes);
    CHECK_EQ(int(b.mig.outcome), int(mig::Outcome::Migrated));
    CHECK_EQ(b.mig.tight, 1);
    CHECK_EQ(g.block_count(), kVolume);
    CHECK_EQ(snapshot(g) == without(before, {"/cards", "/bb", "/tcap", "/log.txt"}), 1);
    CHECK_EQ(ring_range_blank(d), 1);
}

// Kept files that do not fit even then (an e-marker library full of quirk blocks): Deferred, the volume stays wide with
// every kept file (the cards and captures are gone) and every boot answers the same without writing.
void test_too_full_defers() {
    RamFlash d;
    make_wide(d, 140, 517);
    const Snapshot before = read_wide(d);
    for (int k = 0; k < 2; ++k) {
        fs::Fs g(d, kBase, kVolume, kWide);
        const std::uint64_t ops = d.progs + d.erases;
        const Boot b = boot(d, g);
        if (k == 0) std::printf("[migrate] too full: outcome=%u used=%u\n", unsigned(b.mig.outcome), unsigned(b.mig.used_blocks));
        CHECK_EQ(int(b.mig.outcome), int(mig::Outcome::Deferred));
        CHECK_EQ(int(b.err), int(fs::Err::Ok));
        CHECK_EQ(g.wide(), 1);
        CHECK_EQ(g.grown(), 0);   // the rings stay off (fs_flash.cpp init_rings asks grown())
        CHECK_EQ(snapshot(g) == without(before, {"/cards", "/bb", "/tcap", "/log.txt"}), 1);
        if (k == 1) CHECK_EQ(d.progs + d.erases, ops);
    }
}

// A blank unit formats straight into the ring layout; a unit already on it is left alone, its rings untouched.
void test_blank_and_current_units() {
    RamFlash d;
    {
        fs::Fs g(d, kBase, kVolume, kWide);
        const Boot b = boot(d, g);
        CHECK_EQ(b.formatted, 1);
        CHECK_EQ(int(b.mig.outcome), int(mig::Outcome::None));
        CHECK_EQ(g.block_count(), kVolume);
        CHECK_EQ(g.exists(fsgrow::kLegacyMarker), 1);
        CHECK_EQ(ring_range_blank(d), 1);
        CHECK_EQ(g.write_file("/usb.cfg", sv(content(8, 1))) == fs::Err::Ok, 1);
    }
    ringlog::Ring cr(d, flash::kCardRingBase, flash::kCardRingSectors, report_card::kRingKind, report_card::kRingReserve);
    cr.scan();
    const std::string rec = content(200, 77);
    CHECK_EQ(cr.append(1, sv(rec)), 1);
    const std::vector<std::uint8_t> ring_bytes(d.view(flash::kCardRingBase), d.view(flash::kCardRingBase) + flash::kSectorSize);
    fs::Fs g(d, kBase, kVolume, kWide);
    const Boot b = boot(d, g);
    CHECK_EQ(b.formatted, 0);
    CHECK_EQ(int(b.mig.outcome), int(mig::Outcome::None));
    CHECK_EQ(g.block_count(), kVolume);
    CHECK_EQ(std::vector<std::uint8_t>(d.view(flash::kCardRingBase), d.view(flash::kCardRingBase) + flash::kSectorSize) == ring_bytes, 1);
}

// The fence: under it littlefs writes nothing at or above it, and moves what it rewrites below it.
void test_fence_keeps_writes_below() {
    RamFlash d;
    fs::Fs f(d, kWide);
    CHECK_EQ(f.format() == fs::Err::Ok, 1);
    CHECK_EQ(f.mount() == fs::Err::Ok, 1);
    f.fence(kVolume);
    const std::vector<std::uint8_t> top(d.image().begin() + std::ptrdiff_t(kVolume) * fs::kBlockSize, d.image().end());
    for (unsigned i = 0; i < 40; ++i) {
        char p[16];
        std::snprintf(p, sizeof p, "/f%u", i % 8);
        CHECK_EQ(f.write_file(p, sv(content(3000, i))) == fs::Err::Ok, 1);
    }
    CHECK_EQ(std::vector<std::uint8_t>(d.image().begin() + std::ptrdiff_t(kVolume) * fs::kBlockSize, d.image().end()) == top, 1);
    CHECK_EQ(f.used_from(kVolume), 0u);
    CHECK_EQ(f.shrink() == fs::Err::Ok, 1);
    CHECK_EQ(f.block_count(), kWide);   // this Fs knows no grown size: it shrinks nothing
    fs::Fs g(d, kBase, kVolume, kWide);
    CHECK_EQ(g.mount() == fs::Err::Ok, 1);
    g.fence(0);
    for (unsigned i = 0; i < 100 && g.used_from(kVolume) == 0; ++i) {   // no fence: the allocator walks into the top
        char p[16];
        std::snprintf(p, sizeof p, "/g%u", i % 8);
        g.write_file(p, sv(content(3000, i)));
    }
    CHECK_EQ(g.used_from(kVolume) > 0, 1);
    CHECK_EQ(g.shrink() == fs::Err::Exists, 1);   // a block in use above 111: no shrink
    CHECK_EQ(g.block_count(), kWide);
}

}  // namespace

int main() {
    test_fence_keeps_writes_below();
    test_wide_volume_moves();
    test_unused_top_shrinks_at_once();
    test_tight_volume_drops_tcap_and_log();
    test_too_full_defers();
    test_blank_and_current_units();
    test_power_cut_sweep();
    return test_report("test_fs_migrate");
}
