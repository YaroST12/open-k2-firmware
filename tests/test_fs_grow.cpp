// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for growing the littlefs volume from 90 to 145 blocks (k2::fs::Fs::grow, k2/core/fs_grow.hpp): a volume
// mounts at either size, grows only on proof, survives a power cut at any program or erase of the grow, and a fresh
// format takes the grown size only when the run is proven blank.
#include <array>
#include <cstring>
#include <string>

#include "k2/core/fs.hpp"
#include "k2/core/fs_grow.hpp"
#include "k2/drivers/flash.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;
using k2test::RamDisk;

namespace {

constexpr std::uint32_t kBase = flash::kFsBlocks;        // 90
constexpr std::uint32_t kGrown = flash::kFsGrownBlocks;  // 145

std::span<const std::uint8_t> sv(const std::string& s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }

std::string slurp(fs::Fs& f, const char* path) {
    std::array<std::uint8_t, 4096> buf{};
    std::size_t n = 0;
    return f.read_file(path, buf, n) == fs::Err::Ok ? std::string(reinterpret_cast<char*>(buf.data()), n) : std::string("<missing>");
}

// A grown volume mounts again at its grown size, files intact; the base size still mounts as before.
void test_fs_mounts_both_sizes_and_grows() {
    RamDisk d(kGrown);
    {
        fs::Fs f(d, kBase, kGrown);
        bool fm = false;
        CHECK_EQ(f.mount_or_format(fm) == fs::Err::Ok, 1);
        CHECK_EQ(f.block_count(), kBase);
        CHECK_EQ(f.grown(), 0);
        CHECK_EQ(f.write_file("/a", sv(std::string(3000, 'a'))) == fs::Err::Ok, 1);
        f.unmount();
        CHECK_EQ(f.mount() == fs::Err::Ok, 1);
        CHECK_EQ(f.block_count(), kBase);
        CHECK_EQ(f.grow() == fs::Err::Ok, 1);
        CHECK_EQ(f.block_count(), kGrown);
        CHECK_EQ(f.grown(), 1);
        CHECK_EQ(f.grow() == fs::Err::Ok, 1);   // already grown: nothing to do
    }
    fs::Fs g(d, kBase, kGrown);
    CHECK_EQ(g.mount() == fs::Err::Ok, 1);
    CHECK_EQ(g.block_count(), kGrown);
    CHECK_EQ(g.grown(), 1);
    CHECK_COND(slurp(g, "/a") == std::string(3000, 'a'));
}

// A superblock of any other size (the old 96-block layout, a damaged count) does not mount: the caller formats.
void test_fs_refuses_a_foreign_block_count() {
    RamDisk d(kGrown);
    {
        fs::Fs f(d, 96);
        bool fm = false;
        CHECK_EQ(f.mount_or_format(fm) == fs::Err::Ok, 1);
    }
    fs::Fs g(d, kBase, kGrown);
    CHECK_EQ(g.mount() == fs::Err::Ok, 0);
    CHECK_EQ(g.ready(), 0);
    fs::Fs h(d, kBase);   // no grown size configured: only the base size mounts
    CHECK_EQ(h.mount() == fs::Err::Ok, 0);
}

// The grown volume has room for the new blocks: about 55 more 2 KB files than the base volume.
void test_grown_volume_uses_the_new_blocks() {
    RamDisk d(kGrown);
    fs::Fs f(d, kBase, kGrown);
    CHECK_EQ(f.format(kGrown) == fs::Err::Ok, 1);
    CHECK_EQ(f.mount() == fs::Err::Ok, 1);
    CHECK_EQ(f.block_count(), kGrown);
    unsigned n = 0;
    for (; n < 200; ++n) {
        char p[16];
        std::snprintf(p, sizeof p, "/f%u", n);
        if (f.write_file(p, sv(std::string(2048, char('a' + n % 26)))) != fs::Err::Ok) break;
    }
    CHECK_EQ(n > 120, 1);
    CHECK_EQ(n < kGrown, 1);
}

// ---- The boot sequence (k2/core/fs_grow.hpp) ------------------------------------------------------------------------

// The RAM disk behind the flash guard: a program or erase of a run block (90..144) before the run is open is refused
// and counted, like flash::store_program/store_erase refuse it on the device.
struct GuardedDisk final : fs::BlockDevice {
    RamDisk ram{kGrown};
    bool open = false;
    int violations = 0;
    int read(std::uint32_t b, std::uint32_t o, void* buf, std::uint32_t n) override { return ram.read(b, o, buf, n); }
    int prog(std::uint32_t b, std::uint32_t o, const void* buf, std::uint32_t n) override {
        if (b >= kBase && !open) { ++violations; return fs::kIoFail; }
        return ram.prog(b, o, buf, n);
    }
    int erase(std::uint32_t b) override {
        if (b >= kBase && !open) { ++violations; return fs::kIoFail; }
        return ram.erase(b);
    }
    // The flash byte at a device address of the run.
    std::uint8_t& at(std::uint32_t addr) {
        const std::uint32_t block = kBase + (addr - flash::kFsRunBase) / flash::kSectorSize;
        return ram.image()[std::size_t(block) * fs::kBlockSize + addr % flash::kSectorSize];
    }
};

// The sequence's environment over the guarded disk: the blank check reads the run through the block map.
struct Env {
    GuardedDisk& d;
    int opens = 0;
    bool blank(std::uint32_t sector) {
        for (std::uint32_t a = sector; a < sector + flash::kSectorSize; a++)
            if (d.at(a) != 0xFF) return false;
        return true;
    }
    void open_run() { d.open = true; ++opens; }
};
static_assert(fsgrow::Env<Env>);

// A 90-block volume as the device has it today: the legacy wipe done, a few files. `churn` rewrites of a small root
// file fill the superblock pair to different levels, so the grow's commit appends, compacts or splits it.
void make_base_volume(GuardedDisk& d, bool legacy_done = true, int churn = 0) {
    fs::Fs f(d, kBase, kGrown);
    CHECK_EQ(f.format() == fs::Err::Ok, 1);
    CHECK_EQ(f.mount() == fs::Err::Ok, 1);
    for (int i = 0; i < churn; ++i) f.write_file("/rd.rec", sv(std::string(10, char('0' + i % 10))));
    if (legacy_done) f.write_file(fsgrow::kLegacyMarker, sv("1"));
    CHECK_EQ(f.write_file("/big", sv(std::string(3000, 'b'))) == fs::Err::Ok, 1);
    CHECK_EQ(f.write_file("/usb.cfg", sv(std::string("K2US\x01\xfe\0\0", 8))) == fs::Err::Ok, 1);
    f.unmount();
}

void check_files(fs::Fs& f) {
    CHECK_COND(slurp(f, "/big") == std::string(3000, 'b'));
    CHECK_COND(slurp(f, "/usb.cfg") == std::string("K2US\x01\xfe\0\0", 8));
}

void test_blank_run_grows() {
    GuardedDisk d;
    make_base_volume(d);
    fs::Fs f(d, kBase, kGrown);
    Env env{d};
    bool formatted = true;
    CHECK_EQ(fsgrow::mount(f, env, formatted) == fs::Err::Ok, 1);
    CHECK_EQ(formatted, 0);
    CHECK_EQ(f.block_count(), kBase);
    CHECK_EQ(d.open, 0);                                  // a 90-block volume does not open the run
    const fsgrow::Result r = fsgrow::grow(f, env);
    CHECK_EQ(int(r.outcome), int(fsgrow::Outcome::Grown));
    CHECK_EQ(f.block_count(), kGrown);
    CHECK_EQ(f.exists(fsgrow::kIntent), 0);               // the intent is gone once the superblock says 145
    check_files(f);
    // The new blocks take data: more than the 90-block volume could hold.
    unsigned n = 0;
    for (; n < 200; ++n) {
        char p[16];
        std::snprintf(p, sizeof p, "/f%u", n);
        if (f.write_file(p, sv(std::string(2048, 'x'))) != fs::Err::Ok) break;
    }
    CHECK_EQ(n > kBase, 1);
    CHECK_EQ(d.violations, 0);
    // Booting again: the grown volume mounts at 145 and opens the run itself; grow() has nothing to do.
    fs::Fs g(d, kBase, kGrown);
    Env env2{d};
    d.open = false;
    CHECK_EQ(fsgrow::mount(g, env2, formatted) == fs::Err::Ok, 1);
    CHECK_EQ(formatted, 0);
    CHECK_EQ(g.block_count(), kGrown);
    CHECK_EQ(d.open, 1);
    CHECK_EQ(int(fsgrow::grow(g, env2).outcome), int(fsgrow::Outcome::AlreadyGrown));
    check_files(g);
    CHECK_EQ(d.violations, 0);
}

// One page of the run holds data that is not ours: no grow, the page and the run stay closed, the reason names it.
void test_data_in_the_run_blocks_the_grow() {
    for (std::uint32_t page : {flash::kFsRunBase, 0x080F0000u, flash::kFsRunEnd - flash::kSectorSize}) {
        GuardedDisk d;
        make_base_volume(d);
        d.at(page + 6) = 0x5A;
        fs::Fs f(d, kBase, kGrown);
        Env env{d};
        bool formatted = true;
        CHECK_EQ(fsgrow::mount(f, env, formatted) == fs::Err::Ok, 1);
        const fsgrow::Result r = fsgrow::grow(f, env);
        CHECK_EQ(int(r.outcome), int(fsgrow::Outcome::NotBlank));
        CHECK_EQ(r.addr, page);
        CHECK_EQ(f.block_count(), kBase);
        CHECK_EQ(env.opens, 0);
        CHECK_EQ(f.exists(fsgrow::kIntent), 0);
        CHECK_EQ(d.at(page + 6), 0x5A);
        check_files(f);
    }
}

// Before the legacy wipe has run (marker /.legacy missing) the run may still hold our old raw stores: no grow yet.
void test_no_grow_before_the_legacy_wipe() {
    GuardedDisk d;
    make_base_volume(d, false);
    fs::Fs f(d, kBase, kGrown);
    Env env{d};
    bool formatted = true;
    fsgrow::mount(f, env, formatted);
    CHECK_EQ(int(fsgrow::grow(f, env).outcome), int(fsgrow::Outcome::LegacyPending));
    CHECK_EQ(f.block_count(), kBase);
    CHECK_EQ(env.opens, 0);
}

// A fresh format (nothing mounts) takes 145 blocks only on a blank run, and then needs no legacy wipe: a blank run holds
// no old store. Data in the run: 90 blocks, the data untouched.
void test_fresh_format_size_follows_the_proof() {
    {
        GuardedDisk d;
        fs::Fs f(d, kBase, kGrown);
        Env env{d};
        bool formatted = false;
        CHECK_EQ(fsgrow::mount(f, env, formatted) == fs::Err::Ok, 1);
        CHECK_EQ(formatted, 1);
        CHECK_EQ(f.block_count(), kGrown);
        CHECK_EQ(d.open, 1);
        CHECK_EQ(f.exists(fsgrow::kLegacyMarker), 1);
        CHECK_EQ(d.violations, 0);
    }
    {
        GuardedDisk d;
        d.at(0x080E6000) = 'K';
        fs::Fs f(d, kBase, kGrown);
        Env env{d};
        bool formatted = false;
        CHECK_EQ(fsgrow::mount(f, env, formatted) == fs::Err::Ok, 1);
        CHECK_EQ(formatted, 1);
        CHECK_EQ(f.block_count(), kBase);
        CHECK_EQ(d.open, 0);
        CHECK_EQ(f.exists(fsgrow::kLegacyMarker), 0);     // the legacy import and wipe still have their turn
        CHECK_EQ(d.at(0x080E6000), 'K');
        CHECK_EQ(d.violations, 0);
    }
}

// A grow cut short after its intent was committed may leave our own half-written blocks in the run: the intent says the
// run was proven blank before, so the next boot grows without asking the run again. Without the intent the same bytes
// block the grow.
void test_intent_resumes_over_our_own_leftovers() {
    GuardedDisk d;
    make_base_volume(d);
    {
        fs::Fs f(d, kBase, kGrown);
        f.mount();
        f.write_file(fsgrow::kIntent, sv("1"));
        f.unmount();
    }
    d.at(0x080E8000) = 0x00;
    fs::Fs f(d, kBase, kGrown);
    Env env{d};
    bool formatted = true;
    fsgrow::mount(f, env, formatted);
    CHECK_EQ(int(fsgrow::grow(f, env).outcome), int(fsgrow::Outcome::Grown));
    CHECK_EQ(f.block_count(), kGrown);
    CHECK_EQ(f.exists(fsgrow::kIntent), 0);
    check_files(f);
}

// A power cut at every program and erase of the grow: the next boot mounts 90 or 145 blocks (never a format), every
// file is intact, and the grow then completes.
int power_cut_sweep(int churn, bool* compacted) {
    GuardedDisk ref;
    make_base_volume(ref, true, churn);
    int total = 0;
    {
        GuardedDisk d;
        d.ram.image() = ref.ram.image();
        fs::Fs f(d, kBase, kGrown);
        Env env{d};
        bool formatted = true;
        fsgrow::mount(f, env, formatted);
        d.ram.reset_counters();
        CHECK_EQ(int(fsgrow::grow(f, env).outcome), int(fsgrow::Outcome::Grown));
        total = d.ram.ops_done();
        if (compacted) *compacted = d.ram.erases > 0;
    }
    CHECK_EQ(total > 2, 1);
    int at_base = 0, at_grown = 0;
    for (int cut = 0; cut <= total; ++cut) {
        GuardedDisk d;
        d.ram.image() = ref.ram.image();
        {
            fs::Fs f(d, kBase, kGrown);
            Env env{d};
            bool formatted = true;
            fsgrow::mount(f, env, formatted);
            d.ram.cut_after(cut);
            fsgrow::grow(f, env);
        }
        d.ram.revive();
        d.open = false;                                   // a reboot closes the run again
        fs::Fs g(d, kBase, kGrown);
        Env env{d};
        bool formatted = true;
        CHECK_EQ(fsgrow::mount(g, env, formatted) == fs::Err::Ok, 1);
        CHECK_EQ(formatted, 0);
        CHECK_EQ(g.block_count() == kBase || g.block_count() == kGrown, 1);
        (g.block_count() == kBase ? at_base : at_grown)++;
        check_files(g);
        const fsgrow::Outcome o = fsgrow::grow(g, env).outcome;
        CHECK_EQ(o == fsgrow::Outcome::Grown || o == fsgrow::Outcome::AlreadyGrown, 1);
        CHECK_EQ(g.block_count(), kGrown);
        CHECK_EQ(g.exists(fsgrow::kIntent), 0);
        check_files(g);
        CHECK_EQ(d.violations, 0);
    }
    CHECK_EQ(at_base > 0, 1);
    CHECK_EQ(at_grown > 0, 1);
    return total;
}

void test_power_cut_sweep_during_the_grow() {
    int points = 0, levels = 0, compactions = 0;
    for (int churn = 0; churn <= 160; ++churn, ++levels) {
        bool compacted = false;
        points += power_cut_sweep(churn, &compacted) + 1;
        compactions += compacted;
    }
    std::printf("[grow] power-cut sweeps over %d fill levels of the superblock pair: %d cut points, %d levels compact the pair "
                "during the grow\n", levels, points, compactions);
    CHECK_EQ(compactions > 0, 1);   // the sweep covers a grow whose commits erase and rewrite the pair, not only appends
}

}  // namespace

int main() {
    test_fs_mounts_both_sizes_and_grows();
    test_fs_refuses_a_foreign_block_count();
    test_grown_volume_uses_the_new_blocks();
    test_blank_run_grows();
    test_data_in_the_run_blocks_the_grow();
    test_no_grow_before_the_legacy_wipe();
    test_fresh_format_size_follows_the_proof();
    test_intent_resumes_over_our_own_leftovers();
    test_power_cut_sweep_during_the_grow();
    return test_report("test_fs_grow");
}
