// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for k2::fs (littlefs over a NOR-semantics RAM disk): mount/format, whole-file round trips,
// atomic replace, append, listing, tmp cleanup, and a power-cut sweep over every program/erase of a write.
#include <array>
#include <cstring>
#include <string>

#include "k2/core/fs.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;
using k2test::RamDisk;

namespace {

constexpr std::uint32_t kBlocks = 96;

std::span<const std::uint8_t> sv(const std::string& s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }

std::string slurp(fs::Fs& f, const char* path, fs::Err* e = nullptr) {
    std::array<std::uint8_t, 4096> buf{};
    std::size_t n = 0;
    const fs::Err r = f.read_file(path, buf, n);
    if (e) *e = r;
    return r == fs::Err::Ok ? std::string(reinterpret_cast<char*>(buf.data()), n) : std::string();
}

void test_blank_disk_formats() {
    RamDisk d(kBlocks);
    fs::Fs f(d, kBlocks);
    CHECK_EQ(f.mount() == fs::Err::Ok, 0);
    bool formatted = false;
    CHECK_EQ(f.mount_or_format(formatted) == fs::Err::Ok, 1);
    CHECK_EQ(formatted, 1);
    CHECK_EQ(f.ready(), 1);
    f.unmount();
    CHECK_EQ(f.mount_or_format(formatted) == fs::Err::Ok, 1);
    CHECK_EQ(formatted, 0);
}

void test_round_trip_and_replace() {
    RamDisk d(kBlocks);
    fs::Fs f(d, kBlocks);
    bool fm;
    f.mount_or_format(fm);
    CHECK_EQ(f.mkdir("/cards") == fs::Err::Ok, 1);
    CHECK_EQ(f.mkdir("/cards") == fs::Err::Ok, 1);   // idempotent
    CHECK_EQ(f.write_file("/cards/a.json", sv("{\"a\":1}")) == fs::Err::Ok, 1);
    CHECK_STR(slurp(f, "/cards/a.json").c_str(), "{\"a\":1}");
    CHECK_EQ(f.write_file("/cards/a.json", sv("{\"a\":22}")) == fs::Err::Ok, 1);
    CHECK_STR(slurp(f, "/cards/a.json").c_str(), "{\"a\":22}");
    fs::Err e;
    slurp(f, "/cards/none.json", &e);
    CHECK_EQ(e == fs::Err::NotFound, 1);
    std::size_t n = 0;
    CHECK_EQ(f.size_of("/cards/a.json", n) == fs::Err::Ok, 1);
    CHECK_EQ(n, 8);
    CHECK_EQ(f.remove("/cards/a.json") == fs::Err::Ok, 1);
    CHECK_EQ(f.exists("/cards/a.json"), 0);
    // no ".tmp" leaks after a successful write
    int count = 0;
    f.list("/cards", [&](const fs::Info&) { ++count; return true; });
    CHECK_EQ(count, 0);
}

void test_too_big_reports_size() {
    RamDisk d(kBlocks);
    fs::Fs f(d, kBlocks);
    bool fm;
    f.mount_or_format(fm);
    std::string big(1000, 'x');
    CHECK_EQ(f.write_file("/big", sv(big)) == fs::Err::Ok, 1);
    std::array<std::uint8_t, 100> small{};
    std::size_t n = 0;
    CHECK_EQ(f.read_file("/big", small, n) == fs::Err::TooBig, 1);
    CHECK_EQ(n, 1000);
    CHECK_STR(slurp(f, "/big").c_str(), big.c_str());   // a 1000 byte file spans several blocks
}

void test_append_and_list() {
    RamDisk d(kBlocks);
    fs::Fs f(d, kBlocks);
    bool fm;
    f.mount_or_format(fm);
    f.mkdir("/jr");
    for (int i = 0; i < 5; ++i) CHECK_EQ(f.append_file("/jr/log.dat", sv("ab")) == fs::Err::Ok, 1);
    CHECK_STR(slurp(f, "/jr/log.dat").c_str(), "ababababab");
    f.write_file("/jr/b", sv("1"));
    f.write_file("/jr/a", sv("22"));
    std::string names;
    f.list("/jr", [&](const fs::Info& i) { names += i.name; names += ':' + std::to_string(i.size) + ' '; return true; });
    CHECK_STR(names.c_str(), "a:2 b:1 log.dat:10 ");   // sorted by name
    int seen = 0;
    f.list("/jr", [&](const fs::Info&) { return ++seen < 2; });
    CHECK_EQ(seen, 2);                                  // the callback stops the walk
}

void test_remount_keeps_files() {
    RamDisk d(kBlocks);
    {
        fs::Fs f(d, kBlocks);
        bool fm;
        f.mount_or_format(fm);
        f.write_file("/prefs.json", sv("{\"v\":1}"));
    }
    fs::Fs g(d, kBlocks);
    CHECK_EQ(g.mount() == fs::Err::Ok, 1);
    CHECK_STR(slurp(g, "/prefs.json").c_str(), "{\"v\":1}");
}

void test_clean_tmp() {
    RamDisk d(kBlocks);
    fs::Fs f(d, kBlocks);
    bool fm;
    f.mount_or_format(fm);
    f.mkdir("/cards");
    f.write_file("/cards/x.json.tmp", sv("half"));   // what an interrupted write_file leaves
    f.write_file("/cards/y.json", sv("ok"));
    CHECK_EQ(f.clean_tmp("/cards") == fs::Err::Ok, 1);
    CHECK_EQ(f.exists("/cards/x.json.tmp"), 0);
    CHECK_EQ(f.exists("/cards/y.json"), 1);
}

// Power cut at every program/erase of an atomic replace: the file is the old content or the new one,
// the image always mounts, and a leftover .tmp never shows in a listing.
void test_power_cut_sweep_atomic_replace() {
    const std::string oldv(180, 'o'), newv(200, 'n');
    int total = 0;
    {   // how many operations does the replace take?
        RamDisk d(kBlocks);
        fs::Fs f(d, kBlocks);
        bool fm;
        f.mount_or_format(fm);
        f.write_file("/p.json", sv(oldv));
        d.reset_counters();
        f.write_file("/p.json", sv(newv));
        total = d.ops_done();
    }
    CHECK_EQ(total > 2, 1);
    for (int cut = 0; cut <= total; ++cut) {
        RamDisk d(kBlocks);
        {
            fs::Fs f(d, kBlocks);
            bool fm;
            f.mount_or_format(fm);
            f.write_file("/p.json", sv(oldv));
            d.cut_after(cut);
            f.write_file("/p.json", sv(newv));
        }
        d.revive();
        fs::Fs g(d, kBlocks);
        bool fm;
        CHECK_EQ(g.mount_or_format(fm) == fs::Err::Ok, 1);
        CHECK_EQ(fm, 0);                                  // never needs a reformat
        const std::string got = slurp(g, "/p.json");
        const bool ok = got == oldv || got == newv;
        CHECK_EQ(ok, 1);
        if (cut == total) CHECK_EQ(got == newv, 1);       // no cut: the new content landed
        g.clean_tmp("/");
        // the file system stays writable after the cut
        CHECK_EQ(g.write_file("/q.json", sv("after")) == fs::Err::Ok, 1);
    }
}

// A torn append leaves the old content (a littlefs commit is all or nothing).
void test_power_cut_sweep_append() {
    int total = 0;
    {
        RamDisk d(kBlocks);
        fs::Fs f(d, kBlocks);
        bool fm;
        f.mount_or_format(fm);
        f.append_file("/l.dat", sv("0123456789"));
        d.reset_counters();
        f.append_file("/l.dat", sv("ABCDEF"));
        total = d.ops_done();
    }
    for (int cut = 0; cut <= total; ++cut) {
        RamDisk d(kBlocks);
        {
            fs::Fs f(d, kBlocks);
            bool fm;
            f.mount_or_format(fm);
            f.append_file("/l.dat", sv("0123456789"));
            d.cut_after(cut);
            f.append_file("/l.dat", sv("ABCDEF"));
        }
        d.revive();
        fs::Fs g(d, kBlocks);
        CHECK_EQ(g.mount() == fs::Err::Ok, 1);
        const std::string got = slurp(g, "/l.dat");
        const bool ok = got == "0123456789" || got == "0123456789ABCDEF";
        CHECK_EQ(ok, 1);
    }
}

void test_full_disk_reports_no_space_and_recovers() {
    RamDisk d(kBlocks);
    fs::Fs f(d, kBlocks);
    bool fm;
    f.mount_or_format(fm);
    std::string blob(1900, 'z');
    fs::Err last = fs::Err::Ok;
    int i = 0;
    for (; i < 200 && last == fs::Err::Ok; ++i) last = f.write_file(("/f" + std::to_string(i)).c_str(), sv(blob));
    CHECK_EQ(last == fs::Err::NoSpace, 1);
    CHECK_EQ(i > 60, 1);
    f.remove("/f0");
    CHECK_EQ(f.write_file("/again", sv(blob)) == fs::Err::Ok, 1);
}

void test_wear_levelling_spreads_erases() {
    RamDisk d(kBlocks);
    fs::Fs f(d, kBlocks);
    bool fm;
    f.mount_or_format(fm);
    f.write_file("/prefs.json", sv("{}"));
    d.reset_counters();
    for (int i = 0; i < 300; ++i) f.write_file("/prefs.json", sv(i & 1 ? "{\"a\":1}" : "{\"a\":2}"));
    CHECK_EQ(slurp(f, "/prefs.json").size() > 0, 1);
    CHECK_EQ(d.erases < 300, 1);   // small inline files: metadata log, not an erase per write
}

// write_chunks: a file larger than the stack buffer it is built in, produced by offset (the fill callback sees every byte once
// and in order), replaced atomically like write_file: a power cut at any program / erase leaves the old file or the new one.
std::uint8_t pattern(std::size_t i) { return std::uint8_t(i * 7 + (i >> 8)); }
void fill_pattern(void*, std::size_t off, std::span<std::uint8_t> out) {
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = pattern(off + i);
}

bool matches_pattern(fs::Fs& f, const char* path, std::size_t n) {
    std::array<std::uint8_t, 4096> buf{};
    std::size_t got = 0;
    if (f.read_file(path, buf, got) != fs::Err::Ok || got != n) return false;
    for (std::size_t i = 0; i < n; ++i) if (buf[i] != pattern(i)) return false;
    return true;
}

void test_write_chunks_round_trip() {
    RamDisk d(kBlocks);
    fs::Fs f(d, kBlocks);
    bool fm;
    f.mount_or_format(fm);
    CHECK_EQ(f.mkdir("/jr") == fs::Err::Ok, 1);
    for (const std::size_t n : {std::size_t(0), std::size_t(1), std::size_t(63), std::size_t(64), std::size_t(65), std::size_t(300), std::size_t(1810)}) {
        CHECK_EQ(f.write_chunks("/jr/x.bin", n, fill_pattern, nullptr) == fs::Err::Ok, 1);
        CHECK_EQ(matches_pattern(f, "/jr/x.bin", n), 1);
    }
    std::size_t sz = 0;
    CHECK_EQ(f.size_of("/jr/x.bin", sz) == fs::Err::Ok && sz == 1810, 1);
    // The fill callback is asked for contiguous ranges, in order, covering exactly [0, total).
    struct Seen { std::size_t next = 0; bool ordered = true; } seen;
    CHECK_EQ(f.write_chunks("/jr/y.bin", 500, [](void* c, std::size_t off, std::span<std::uint8_t> out) {
        auto& s = *static_cast<Seen*>(c);
        if (off != s.next) s.ordered = false;
        s.next = off + out.size();
        for (auto& b : out) b = 0;
    }, &seen) == fs::Err::Ok, 1);
    CHECK_EQ(seen.ordered && seen.next == 500, 1);
    f.write_chunks("/jr/x.bin", 40, fill_pattern, nullptr);
    CHECK_EQ(f.write_chunks("/nodir/x.bin", 10, fill_pattern, nullptr) != fs::Err::Ok, 1);
    CHECK_EQ(f.exists("/nodir/x.bin"), 0);
}

void test_write_chunks_power_cut_sweep() {
    int total = 0;
    {
        RamDisk d(kBlocks);
        fs::Fs f(d, kBlocks);
        bool fm;
        f.mount_or_format(fm);
        f.mkdir("/jr");
        f.write_chunks("/jr/s.bin", 700, fill_pattern, nullptr);
        d.reset_counters();
        f.write_chunks("/jr/s.bin", 1500, fill_pattern, nullptr);
        total = d.ops_done();
    }
    CHECK_EQ(total > 4, 1);
    for (int cut = 0; cut <= total; ++cut) {
        RamDisk d(kBlocks);
        {
            fs::Fs f(d, kBlocks);
            bool fm;
            f.mount_or_format(fm);
            f.mkdir("/jr");
            f.write_chunks("/jr/s.bin", 700, fill_pattern, nullptr);
            d.cut_after(cut);
            f.write_chunks("/jr/s.bin", 1500, fill_pattern, nullptr);
        }
        d.revive();
        fs::Fs g(d, kBlocks);
        CHECK_EQ(g.mount() == fs::Err::Ok, 1);
        CHECK_EQ(matches_pattern(g, "/jr/s.bin", 700) || matches_pattern(g, "/jr/s.bin", 1500), 1);
    }
}

}  // namespace

int main() {
    test_blank_disk_formats();
    test_round_trip_and_replace();
    test_too_big_reports_size();
    test_append_and_list();
    test_remount_keeps_files();
    test_clean_tmp();
    test_power_cut_sweep_atomic_replace();
    test_power_cut_sweep_append();
    test_full_disk_reports_no_space_and_recovers();
    test_wear_levelling_spreads_erases();
    test_write_chunks_round_trip();
    test_write_chunks_power_cut_sweep();
    return test_report("test_fs");
}
