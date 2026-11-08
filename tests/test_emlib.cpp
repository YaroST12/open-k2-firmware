// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the e-marker library (k2::emlib): one file per cable on littlefs, over a RAM disk that
// models NOR flash and can lose power at any program or erase.
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "k2/pd/emlib.hpp"
#include "k2/pd/emname.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;

namespace {

constexpr std::uint32_t kBlocks = 96;

struct Vol {
    k2test::RamDisk d{kBlocks};
    fs::Fs f{d, kBlocks};
    Vol() { bool fm; f.mount_or_format(fm); }
};

emrec::Record cable(unsigned n, std::uint32_t cab = 0x00000C43) {
    const std::array<std::uint32_t, 5> v = {0xFF00A041, 0x18002B1Du + 0, 0, 0x40510000u + n, cab};
    return emrec::make(v, 2, "x");
}

std::vector<std::uint16_t> seqs(const emlib::Library& l) {
    std::array<std::uint16_t, emlib::kCapacity + 1> s{};
    const unsigned n = l.list(s);
    return {s.begin(), s.begin() + n};
}

}  // namespace

static void test_name() {
    // ID header VID; cable VDO 1: bits 2:0 speed, 6:5 current (2 = 5 A), 10:9 voltage (0 = 20 V, 3 = 50 V)
    auto name = [](std::uint32_t vid, std::uint32_t cab, unsigned ord = 1) {
        const std::array<std::uint32_t, 5> v = {0xFF00A041, 0x18000000u | vid, 0, 0, cab};
        return std::string(emname::make(emrec::make(v, 2), ord).data());
    };
    CHECK_STR(name(0x05AC, 4 | (2u << 5) | (3u << 9)).c_str(), "Apple-USB4G4-240W");
    CHECK_STR(name(0x05AC, 3 | (2u << 5) | (3u << 9)).c_str(), "Apple-USB4G3-240W");
    CHECK_STR(name(0x1037, 0).c_str(), "Anker-USB2-60W");
    CHECK_STR(name(0x1037, 1 | (2u << 5)).c_str(), "Anker-USB3.2G1-100W");
    CHECK_STR(name(0x1037, 2 | (2u << 5) | (2u << 9)).c_str(), "Anker-USB3.2G2-200W");
    CHECK_STR(name(0x8087, 3 | (2u << 5)).c_str(), "Intel-TBT3-100W");
    CHECK_STR(name(0x2B1D, 1 | (1u << 5)).c_str(), "0x2B1D-USB3.2G1-60W");
    CHECK_STR(name(0x05AC, 0, 2).c_str(), "Apple-USB2-60W-2");
    CHECK_STR(name(0x05AC, 0, 13).c_str(), "Apple-USB2-60W-13");
    // always fits 24 characters; a long manufacturer is cut, not the version or wattage
    const std::string longest = name(0x31CA, 2 | (2u << 5) | (3u << 9), 9999);   // Leadtrend
    CHECK_EQ(longest.size() <= 24, 1);
    CHECK_EQ(longest.find("USB3.2G2-240W-9999") != std::string::npos, 1);
}

static void test_short_name_predicate() {
    // The predicate form sees the names already handed out through a callback, no array needed.
    std::vector<emname::Short83> taken;
    auto is_taken = [](const void* c, const emname::Short83& n) {
        for (const auto& u : *static_cast<const std::vector<emname::Short83>*>(c)) if (u == n) return true;
        return false;
    };
    for (const char* n : {"Anker-USB3.2G1-100W", "Anker-USB3.2G1-100W-2", "Anker-USB3.2G1-100W-3"})
        taken.push_back(emname::short_name(n, is_taken, &taken));
    CHECK_STR(std::string(taken[0].data(), 11).c_str(), "ANKER-~1JSN");
    CHECK_STR(std::string(taken[1].data(), 11).c_str(), "ANKER-~2JSN");
    CHECK_STR(std::string(taken[2].data(), 11).c_str(), "ANKER-~3JSN");
}

static void test_short_name() {
    std::vector<emname::Short83> taken;
    auto sn = [&](const char* n) {
        const emname::Short83 s = emname::short_name(n, taken);
        taken.push_back(s);
        return std::string(s.data(), s.size());
    };
    CHECK_STR(sn("Desk USB4").c_str(), "DESKUSB4JSN");
    CHECK_STR(sn("a b").c_str(), "AB      JSN");
    CHECK_STR(sn("Anker-USB3.2G1-100W").c_str(), "ANKER-~1JSN");
    CHECK_STR(sn("Anker-USB3.2G1-100W-2").c_str(), "ANKER-~2JSN");     // same cut base: next number
    CHECK_STR(sn("desk usb4").c_str(), "DESKUS~1JSN");                 // same short name as the first: numbered
    CHECK_STR(sn("!!!").c_str(), "CABLE   JSN");
    CHECK_STR(sn("!!!").c_str(), "CABLE~1 JSN");
    for (int i = 0; i < 150; i++) sn("Apple-USB4G3-240W");             // many collisions stay unique
    for (std::size_t i = 0; i < taken.size(); i++)
        for (std::size_t j = i + 1; j < taken.size(); j++) CHECK_EQ(taken[i] == taken[j] ? 1 : 0, 0);
}

static void test_sanitize() {
    std::array<char, emlib::kNameMax + 1> o;
    CHECK_EQ(emlib::sanitize("  My cable_1.5-m!?  ", o) ? 1 : 0, 1);
    CHECK_STR(o.data(), "My cable_1.5-m");
    CHECK_EQ(emlib::sanitize("\xC3\xA9\t#", o) ? 1 : 0, 0);                  // nothing valid left
    CHECK_EQ(emlib::sanitize("0123456789012345678901234567890", o) ? 1 : 0, 1);
    CHECK_EQ(std::strlen(o.data()), emlib::kNameMax);
    CHECK_EQ(emlib::sanitize("abcdefghijklmnopqrstuvw  x", o) ? 1 : 0, 1);      // cut at 24, trailing space trimmed
    CHECK_STR(o.data(), "abcdefghijklmnopqrstuvw");
    CHECK_EQ(emlib::name_valid("ok-name_1.0") ? 1 : 0, 1);
    CHECK_EQ(emlib::name_valid("") ? 1 : 0, 0);
    CHECK_EQ(emlib::name_valid(" lead") ? 1 : 0, 0);
    CHECK_EQ(emlib::name_valid("a/b") ? 1 : 0, 0);
    CHECK_EQ(emlib::name_valid("0123456789012345678901234") ? 1 : 0, 0);        // 25
}

static void test_layout() {
    CHECK_EQ(emlib::kFileSize, std::size_t(98));
    CHECK_EQ(emlib::kOffRec + emrec::kRecordSize, emlib::kFileSize);
    // the whole library at capacity fits the volume many times over (inline files, ~one 2 KB block per 20)
    CHECK_EQ(emlib::kCapacity * emlib::kFileSize < 96u * fs::kBlockSize / 2, 1);
}

static void test_save_list_get() {
    Vol v;
    emlib::Library l(v.f);
    CHECK_EQ(l.count(), 0u);
    const emlib::Result r = l.save(cable(1));
    CHECK_EQ(int(r.err), int(emlib::Err::Ok));
    CHECK_EQ(r.seq, 1);
    CHECK_EQ(r.existed ? 1 : 0, 0);
    emlib::Entry e;
    CHECK_EQ(l.get(1, e) ? 1 : 0, 1);
    CHECK_STR(e.name.data(), "0x2B1D-USB4G3-200W");
    CHECK_STR(reinterpret_cast<const char*>(e.rec.data()), "0x2B1D-USB4G3-200W");
    CHECK_EQ(emrec::valid(e.rec) ? 1 : 0, 1);
    CHECK_EQ(emrec::vdo(e.rec, 3), 0x40510001u);
    CHECK_EQ(l.save(cable(2)).seq, 2);
    CHECK_EQ(l.save(cable(3)).seq, 3);
    CHECK_EQ(seqs(l).size(), 3u);
    CHECK_EQ(seqs(l)[2], 3);
    CHECK_EQ(l.get(9, e) ? 1 : 0, 0);
    emlib::Library l2(v.f);                                // state lives in the volume only
    CHECK_EQ(l2.count(), 3u);
    CHECK_EQ(l2.save(cable(4)).seq, 4);
    CHECK_EQ(v.f.exists("/emk/0004.emk") ? 1 : 0, 1);
}

static void test_duplicate_and_invalid() {
    Vol v;
    emlib::Library l(v.f);
    CHECK_EQ(l.save(cable(1)).seq, 1);
    v.d.reset_counters();
    const emlib::Result again = l.save(cable(1));
    CHECK_EQ(int(again.err), int(emlib::Err::Ok));
    CHECK_EQ(again.existed ? 1 : 0, 1);
    CHECK_EQ(again.seq, 1);
    CHECK_EQ(v.d.ops_done(), 0);                            // nothing written
    CHECK_EQ(l.count(), 1u);
    emrec::Record bad = cable(2);
    bad[10] ^= 0xFF;
    CHECK_EQ(int(l.save(bad).err), int(emlib::Err::Invalid));
    CHECK_EQ(l.count(), 1u);
}

static void test_delete() {
    Vol v;
    emlib::Library l(v.f);
    for (unsigned i = 1; i <= 4; i++) l.save(cable(i));
    CHECK_EQ(int(l.remove(2)), int(emlib::Err::Ok));
    CHECK_EQ(l.count(), 3u);
    emlib::Entry e;
    CHECK_EQ(l.get(2, e) ? 1 : 0, 0);
    CHECK_EQ(int(l.remove(2)), int(emlib::Err::NotFound));
    CHECK_EQ(int(l.remove(77)), int(emlib::Err::NotFound));
    CHECK_EQ(seqs(l)[1], 3);
    const emlib::Result r = l.save(cable(2));
    CHECK_EQ(r.existed ? 1 : 0, 0);
    CHECK_EQ(r.seq, 2);                                     // the lowest free number is reused
}

static void test_rename() {
    Vol v;
    {
        emlib::Library l(v.f);
        for (unsigned i = 1; i <= 3; i++) l.save(cable(i));
        CHECK_EQ(int(l.rename(2, "Desk USB4")), int(emlib::Err::Ok));
        CHECK_EQ(int(l.rename(2, "Desk USB4")), int(emlib::Err::Ok));
        CHECK_EQ(int(l.rename(1, "DESK usb4")), int(emlib::Err::Exists));
        CHECK_EQ(int(l.rename(1, "bad/name")), int(emlib::Err::Invalid));
        CHECK_EQ(int(l.rename(77, "x")), int(emlib::Err::NotFound));
        CHECK_EQ(int(l.rename(3, "this name is exactly 24c")), int(emlib::Err::Ok));
    }
    emlib::Library l(v.f);
    emlib::Entry e;
    CHECK_EQ(l.get(2, e) ? 1 : 0, 1);
    CHECK_STR(e.name.data(), "Desk USB4");
    CHECK_EQ(int(e.version), 2);
    CHECK_EQ(emrec::vdo(e.rec, 3), 0x40510002u);
    CHECK_EQ(emrec::valid(e.rec) ? 1 : 0, 1);
    CHECK_EQ(l.get(3, e) ? 1 : 0, 1);
    CHECK_STR(e.name.data(), "this name is exactly 24c");
    CHECK_EQ(l.count(), 3u);
    CHECK_EQ(int(l.rename(2, "other")), int(emlib::Err::Ok));
    CHECK_EQ(int(l.rename(1, "Desk USB4")), int(emlib::Err::Ok));
    for (int round = 0; round < 100; round++) {            // churn leaves renamed cables alone
        const auto s = seqs(l);
        l.remove(s.back());
        CHECK_EQ(int(l.save(cable(7000 + round)).err), int(emlib::Err::Ok));
    }
    CHECK_EQ(l.get(1, e) ? 1 : 0, 1);
    CHECK_STR(e.name.data(), "Desk USB4");
}

static void test_fill_to_capacity() {
    Vol v;
    emlib::Library l(v.f);
    for (unsigned i = 1; i <= emlib::kCapacity; i++) CHECK_EQ(int(l.save(cable(i)).err), int(emlib::Err::Ok));
    CHECK_EQ(l.count(), unsigned(emlib::kCapacity));
    CHECK_EQ(int(l.save(cable(5000)).err), int(emlib::Err::Full));
    CHECK_EQ(int(l.remove(40)), int(emlib::Err::Ok));
    const emlib::Result r = l.save(cable(5000));
    CHECK_EQ(int(r.err), int(emlib::Err::Ok));
    CHECK_EQ(l.count(), unsigned(emlib::kCapacity));
    emlib::Entry e;
    CHECK_EQ(l.get(41, e) ? 1 : 0, 1);
    CHECK_EQ(emrec::vdo(e.rec, 3), 0x40510000u + 41);
    CHECK_EQ(l.get(r.seq, e) ? 1 : 0, 1);
    CHECK_EQ(emrec::vdo(e.rec, 3), 0x40510000u + 5000);
    for (int round = 0; round < 200; round++) {            // churn at capacity stays consistent
        const auto s = seqs(l);
        CHECK_EQ(int(l.remove(s[(round * 37) % s.size()])), int(emlib::Err::Ok));
        CHECK_EQ(int(l.save(cable(6000 + round)).err), int(emlib::Err::Ok));
    }
    CHECK_EQ(seqs(l).size(), std::size_t(emlib::kCapacity));
}

static void test_foreign_files_ignored() {
    Vol v;
    emlib::Library l(v.f);
    l.save(cable(1));
    v.f.write_file("/emk/readme.txt", std::span<const std::uint8_t>());
    const std::array<std::uint8_t, 5> junk{1, 2, 3, 4, 5};
    v.f.write_file("/emk/0007.emk", junk);                 // wrong size: not listed
    CHECK_EQ(l.count(), 1u);
    emlib::Entry e;
    CHECK_EQ(l.get(7, e) ? 1 : 0, 0);
}

namespace {
emk_quirk::Quirks oppo_quirks() {
    emk_quirk::Quirks q;
    const std::array<std::uint32_t, 2> w = {0xAABBCCDDu, 0x11223344u};
    emk_quirk::add_row(q, emk_quirk::svdm_row(0x22D9, 31, emk_quirk::Kind::Ack, w));
    q.unmatched = emk_quirk::Unmatched::Nak;
    return q;
}
}  // namespace

// K2E2: the 98-byte K2E1 head, then the quirk block. A cable without quirks stays a K2E1 file.
static void test_quirks_roundtrip() {
    Vol v;
    emlib::Library l(v.f);
    const emk_quirk::Quirks q = oppo_quirks();
    const emlib::Result r = l.save(cable(1), q);
    CHECK_EQ(int(r.err), int(emlib::Err::Ok));
    CHECK_EQ(r.seq, 1);
    std::size_t sz = 0;
    CHECK_EQ(int(v.f.size_of("/emk/0001.emk", sz)), int(fs::Err::Ok));
    CHECK_EQ(sz, emlib::kFileSize + emk_quirk::block_size(1));
    emlib::Entry e;
    CHECK_EQ(l.get(1, e) ? 1 : 0, 1);
    CHECK_STR(e.name.data(), "0x2B1D-USB4G3-200W");
    CHECK_EQ(emrec::vdo(e.rec, 3), 0x40510001u);
    emk_quirk::Quirks back;
    CHECK_EQ(l.get_quirks(1, back) ? 1 : 0, 1);
    CHECK_EQ(back == q ? 1 : 0, 1);
    CHECK_EQ(l.count(), 1u);

    // no quirks: the old file form, and get_quirks answers an empty table
    CHECK_EQ(l.save(cable(2)).seq, 2);
    CHECK_EQ(int(v.f.size_of("/emk/0002.emk", sz)), int(fs::Err::Ok));
    CHECK_EQ(sz, emlib::kFileSize);
    CHECK_EQ(l.get_quirks(2, back) ? 1 : 0, 1);
    CHECK_EQ(back.empty() ? 1 : 0, 1);
    CHECK_EQ(l.get_quirks(9, back) ? 1 : 0, 0);
}

static void test_quirks_dedup_rename_migration() {
    Vol v;
    emlib::Library l(v.f);
    const emk_quirk::Quirks q = oppo_quirks();
    // a plain cable and the same cable with quirks are different clones
    CHECK_EQ(l.save(cable(1)).seq, 1);
    const emlib::Result with = l.save(cable(1), q);
    CHECK_EQ(with.seq, 2);
    CHECK_EQ(with.existed ? 1 : 0, 0);
    emlib::Entry e;
    CHECK_EQ(l.get(2, e) ? 1 : 0, 1);
    CHECK_STR(e.name.data(), "0x2B1D-USB4G3-200W-2");
    // the same pair again: nothing written
    v.d.reset_counters();
    const emlib::Result again = l.save(cable(1), q);
    CHECK_EQ(again.existed ? 1 : 0, 1);
    CHECK_EQ(again.seq, 2);
    CHECK_EQ(v.d.ops_done(), 0);
    // different quirks: a third clone
    emk_quirk::Quirks q2 = q;
    q2.flags = emk_quirk::kQMsgIdEcho;
    CHECK_EQ(l.save(cable(1), q2).seq, 3);

    // a rename keeps the quirks
    CHECK_EQ(int(l.rename(2, "oppo clone")), int(emlib::Err::Ok));
    emk_quirk::Quirks back;
    CHECK_EQ(l.get_quirks(2, back) ? 1 : 0, 1);
    CHECK_EQ(back == q ? 1 : 0, 1);
    CHECK_EQ(l.get(2, e) ? 1 : 0, 1);
    CHECK_STR(e.name.data(), "oppo clone");
    CHECK_EQ(e.version, 2);

    // a K2E1 file written by an older build loads as an entry with empty quirks
    CHECK_EQ(l.remove(1) == emlib::Err::Ok ? 1 : 0, 1);
    CHECK_EQ(l.save(cable(5)).seq, 1);
    CHECK_EQ(l.get_quirks(1, back) ? 1 : 0, 1);
    CHECK_EQ(back.empty() ? 1 : 0, 1);
}

static void test_quirks_corrupt_block() {
    Vol v;
    emlib::Library l(v.f);
    l.save(cable(1), oppo_quirks());
    std::array<std::uint8_t, 256> b{};
    std::size_t n = 0;
    CHECK_EQ(int(v.f.read_file("/emk/0001.emk", b, n)), int(fs::Err::Ok));
    // a bad row (SOP mask 0) in a well-sized file: the cable still loads, the quirks do not
    b[emlib::kFileSize + emk_quirk::kBlockHeader] = 0;
    v.f.write_file("/emk/0001.emk", std::span<const std::uint8_t>(b.data(), n));
    emlib::Entry e;
    emk_quirk::Quirks q;
    CHECK_EQ(l.get(1, e) ? 1 : 0, 1);
    CHECK_EQ(l.get_quirks(1, q) ? 1 : 0, 0);
    // a K2E2 file whose size matches no block is not listed
    const std::array<std::uint8_t, emlib::kFileSize + 5> odd{};
    v.f.write_file("/emk/0007.emk", odd);
    CHECK_EQ(l.count(), 1u);
}

// Power lost after every operation of a save, delete or rename: no earlier cable is ever lost.
static void test_power_loss() {
    Vol base;
    {
        emlib::Library l(base.f);
        for (unsigned i = 1; i <= 12; i++) l.save(cable(i));
    }
    for (int scenario = 0; scenario < 3; scenario++) {
        int total = 0;
        for (int cut = 0; cut < 400; cut++) {
            k2test::RamDisk d = base.d;
            d.revive();
            std::vector<std::uint16_t> before;
            {
                fs::Fs f(d, kBlocks);
                f.mount();
                emlib::Library l(f);
                before = seqs(l);
                d.reset_counters();
                d.cut_after(cut);
                if (scenario == 0) l.save(cable(9000));
                else if (scenario == 1) l.remove(before[5]);
                else l.rename(before[5], "renamed");
            }
            const bool finished = !d.dead();
            d.revive();
            fs::Fs g(d, kBlocks);
            CHECK_EQ(g.mount() == fs::Err::Ok ? 1 : 0, 1);
            g.clean_tmp("/emk");
            emlib::Library after(g);
            const auto now = seqs(after);
            for (std::uint16_t s : before) {
                if (scenario == 1 && s == before[5]) continue;
                emlib::Entry e;
                if (!after.get(s, e)) { CHECK_EQ(cut, -1); break; }
                if (emrec::vdo(e.rec, 3) != 0x40510000u + s) { CHECK_EQ(cut, -2); break; }
            }
            if (scenario == 2) {
                emlib::Entry e;
                CHECK_EQ(after.get(before[5], e) ? 1 : 0, 1);
                CHECK_EQ(std::strcmp(e.name.data(), "renamed") == 0 || std::strncmp(e.name.data(), "0x2B1D", 6) == 0, 1);
                CHECK_EQ(now.size(), before.size());
            }
            CHECK_EQ(now.size() <= before.size() + 1, 1);
            CHECK_EQ(int(after.save(cable(9500)).err), int(emlib::Err::Ok));
            total = cut;
            if (finished) break;
        }
        CHECK_EQ(total >= 1, 1);
    }
}

int main() {
    test_name();
    test_short_name();
    test_short_name_predicate();
    test_sanitize();
    test_layout();
    test_save_list_get();
    test_duplicate_and_invalid();
    test_delete();
    test_fill_to_capacity();
    test_foreign_files_ignored();
    test_rename();
    test_quirks_roundtrip();
    test_quirks_dedup_rename_migration();
    test_quirks_corrupt_block();
    test_power_loss();
    return test_report("test_emlib");
}
