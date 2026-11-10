// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Report card store (k2/chargertest/report_card_store.hpp) on a fake flash that can lose power at any half-word.
#include <cstring>
#include <memory>
#include <vector>

#include "k2/chargertest/report_card_delta.hpp"
#include "k2/chargertest/report_card_store.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;
namespace rc = k2::report_card;

// One littlefs volume on a RAM disk; reboot() remounts it with a fresh store (no RAM state survives).
struct Vol {
    k2test::RamDisk disk{96};
    std::unique_ptr<fs::Fs> fs;
    std::unique_ptr<rc::Store> s;
    Vol() { reboot(); }
    void reboot() {
        s.reset();
        fs = std::make_unique<fs::Fs>(disk, 96);
        bool formatted = false;
        fs->mount_or_format(formatted);
        s = std::make_unique<rc::Store>(*fs);
    }
};
using Store = rc::Store;

static rc::Card card(uint32_t fp, int pass = 1, int fail = 0, unsigned max_mv = 9000) {
    rc::Card c;
    c.id.fp = fp; c.fw = 0xABCD; c.uptime_s = fp;
    for (int i = 0; i < pass; ++i) c.group[i] = {rc::GV::Pass, uint16_t(max_mv), 0, 0};
    for (int i = 0; i < fail; ++i) c.group[6 + i] = {rc::GV::Fail, 0, 3, 13};
    return c;
}
static std::vector<rc::Card> all(Store& s) {
    std::array<rc::Card, rc::kMaxCards + 2> buf;
    const size_t n = s.list(buf);
    return {buf.begin(), buf.begin() + n};
}

static void test_empty_and_first_put() {
    Vol v; Store& s = *v.s;
    CHECK_EQ(all(s).size(), 0u);
    rc::Card c = card(0x1111);
    CHECK_EQ(int(s.put(c)), int(rc::Err::Ok));
    CHECK_EQ(c.card_no, 1); CHECK_EQ(c.runs, 1);
    rc::Card g;
    CHECK_EQ(s.find(0x1111, g), 1);
    CHECK_EQ(g == c, 1);
    CHECK_EQ(s.get(1, g), 1);
    CHECK_EQ(s.get(2, g), 0);
    rc::Card none;
    CHECK_EQ(int(s.put(none)), int(rc::Err::Invalid));   // fp 0 = no identity
    v.reboot();                                          // the card is on flash, not in RAM
    CHECK_EQ(v.s->find(0x1111, g), 1);
    CHECK_EQ(g == c, 1);
}

static void test_retest_replaces_and_keeps_name() {
    Vol vol; Store& s = *vol.s;
    rc::Card a = card(0x1111), b = card(0x2222);
    s.put(a); s.put(b);
    CHECK_EQ(int(s.rename(1, "Desk Anker")), int(rc::Err::Ok));
    rc::Card a2 = card(0x1111, 2, 1);
    s.put(a2);
    CHECK_EQ(a2.card_no, 1); CHECK_EQ(a2.runs, 2);
    CHECK_STR(a2.name, "Desk Anker");
    const auto v = all(s);
    CHECK_EQ(v.size(), 2u);
    CHECK_EQ(v[0].id.fp, 0x1111u);      // newest first
    CHECK_EQ(v[1].id.fp, 0x2222u);
    CHECK_EQ(v[0].counts().fail, 1u);
    CHECK_EQ(int(s.rename(1, "bad/name")), int(rc::Err::Invalid));
    CHECK_EQ(int(s.rename(9, "Nope")), int(rc::Err::NotFound));
}

static void test_rename_keeps_order() {
    Vol vol; Store& s = *vol.s;
    rc::Card a = card(0x1111), b = card(0x2222);
    s.put(a); s.put(b);
    CHECK_EQ(int(s.rename(1, "Old one")), int(rc::Err::Ok));
    std::array<uint16_t, 4> n{};
    CHECK_EQ(s.numbers(n), 2u);
    CHECK_EQ(n[0], 2); CHECK_EQ(n[1], 1);
}

static void test_remove() {
    Vol vol; Store& s = *vol.s;
    rc::Card a = card(0x1111), b = card(0x2222);
    s.put(a); s.put(b);
    CHECK_EQ(int(s.remove(1)), int(rc::Err::Ok));
    CHECK_EQ(all(s).size(), 1u);
    CHECK_EQ(int(s.remove(1)), int(rc::Err::NotFound));
    rc::Card a2 = card(0x1111);                  // the same charger again starts a new card
    s.put(a2);
    CHECK_EQ(a2.runs, 1);
    CHECK_EQ(a2.card_no, 3);
    CHECK_EQ(all(s).size(), 2u);
}

static void test_numbers_are_newest_first() {
    Vol vol; Store& s = *vol.s;
    std::array<uint16_t, rc::kMaxCards> n{};
    CHECK_EQ(s.numbers(n), 0u);
    rc::Card a = card(0x1111), b = card(0x2222), c = card(0x3333);
    s.put(a); s.put(b); s.put(c);
    s.remove(2);
    CHECK_EQ(s.numbers(n), 2u);
    CHECK_EQ(n[0], 3);
    CHECK_EQ(n[1], 1);
    std::array<uint16_t, 1> one{};
    CHECK_EQ(s.numbers(one), 1u);                 // clipped to the buffer
    CHECK_EQ(one[0], 3);
}

static void test_retests_keep_run_counts() {
    Vol vol; Store& s = *vol.s;
    for (int round = 1; round <= 60; ++round) {      // many re-tests of three chargers
        rc::Card c = card(0x100 + round % 3, 1 + round % 4);
        CHECK_EQ(int(s.put(c)), int(rc::Err::Ok));
    }
    const auto v = all(s);
    CHECK_EQ(v.size(), 3u);
    unsigned runs = 0;
    for (const auto& c : v) runs += c.runs;
    CHECK_EQ(runs, 60u);
}

// 40 cards (the target), then up to the capacity and beyond: the oldest makes room.
static void test_forty_cards_and_capacity() {
    Vol vol;
    for (unsigned i = 1; i <= 40; ++i) { rc::Card c = card(0x1000 + i); CHECK_EQ(int(vol.s->put(c)), int(rc::Err::Ok)); }
    vol.reboot();
    auto v = all(*vol.s);
    CHECK_EQ(v.size(), 40u);
    CHECK_EQ(v[0].id.fp, 0x1000u + 40);              // newest first
    CHECK_EQ(v[39].id.fp, 0x1001u);
    const unsigned used40 = vol.fs->used_blocks();
    for (unsigned i = 41; i <= rc::kMaxCards + 3; ++i) { rc::Card c = card(0x1000 + i); CHECK_EQ(int(vol.s->put(c)), int(rc::Err::Ok)); }
    v = all(*vol.s);
    CHECK_EQ(v.size(), size_t(rc::kMaxCards));
    rc::Card g;
    CHECK_EQ(vol.s->find(0x1001, g), 0);             // the first three made room
    CHECK_EQ(vol.s->find(0x1003, g), 0);
    CHECK_EQ(vol.s->find(0x1004, g), 1);
    CHECK_EQ(vol.s->find(0x1000 + rc::kMaxCards + 3, g), 1);
    CHECK_EQ(vol.fs->used_blocks() < 96u, 1);
    (void)used40;
}

// V3 cards (158 bytes, format byte 3) coexist with v4 cards (176 bytes, format byte 4) on flash.
// The Store's ensure() and read() functions accept both file sizes.
static void test_v3_v4_file_sizes_are_both_accepted() {
    CHECK_EQ(rc::kFileSize, 228u);   // v9: 220 + 8; v8 cards are 212
    CHECK_EQ(rc::kFileSizeV7, 210u);
    CHECK_EQ(rc::kFileSizeV6, 204u);
    CHECK_EQ(rc::kFileSizeV5, 192u); CHECK_EQ(rc::kFileSizeV4, 176u); CHECK_EQ(rc::kFileSizeV3, 158u);
}

// A v5 card file (192 bytes: magic, stamp, a 184 byte record with the 178 byte payload) written by the firmware before the UFCS
// modes: it lists and loads (no modes, format raised to the current one), and the next run of that charger rewrites it as v6.
static void test_v5_file_loads_and_resaves_as_v6() {
    Vol v;
    rc::Card c = card(0x5151, 2, 1, 20000);
    c.card_no = 7; c.runs = 3;
    rc::set_name(c, "Old five");
    std::array<std::uint8_t, rc::kMaxRecord> rec{};
    rc::encode_card(rec, c);
    std::array<std::uint8_t, 8 + 184> file{};
    std::memcpy(file.data(), rc::kFileMagic.data(), 4);
    rc::detail::put32(&file[4], 9);
    file[8] = rec[0]; file[9] = rec[1]; file[10] = 178; file[11] = 0;
    std::memcpy(&file[12], &rec[4], 178);
    file[12 + 33] = 5;                                            // the format byte of a v5 payload
    rc::detail::put16(&file[12 + 178], k2::bbstore::crc16(0xFFFF, {&file[8], 4 + 178}));
    char path[rc::Store::kPathLen];
    rc::Store::make_path(0x5151, path);
    CHECK_EQ(int(v.fs->mkdir(rc::kDir)), int(fs::Err::Ok));
    CHECK_EQ(int(v.fs->write_file(path, file)), int(fs::Err::Ok));
    v.reboot();
    CHECK_EQ(all(*v.s).size(), 1u);
    rc::Card g;
    CHECK_EQ(v.s->find(0x5151, g), 1);
    CHECK_EQ(g.card_no, 7); CHECK_EQ(g.runs, 3); CHECK_STR(g.name, "Old five");
    CHECK_EQ(g.group[0].max_mv, 20000u);
    CHECK_EQ(g.format, unsigned(rc::kFormat));                    // migrated in memory
    CHECK_EQ(rc::ufcs_modes(g), 0u);
    CHECK_EQ(g.times, 1u);
    // A fresh run of the same charger carries modes and is stored as a current file.
    rc::Card n = card(0x5151, 2, 0, 20000);
    n.ufcs[0] = {50, 200, 3000};
    CHECK_EQ(int(v.s->put(n)), int(rc::Err::Ok));
    CHECK_EQ(n.card_no, 7); CHECK_EQ(n.runs, 4);
    std::array<std::uint8_t, rc::kFileSize> back{};
    std::size_t got = 0;
    CHECK_EQ(int(v.fs->read_file(path, back, got)), int(fs::Err::Ok));
    CHECK_EQ(got, rc::kFileSize);
    CHECK_EQ(back[12 + 33], unsigned(rc::kFormat));
    v.reboot();
    CHECK_EQ(v.s->find(0x5151, g), 1);
    CHECK_EQ(rc::ufcs_modes(g), 1u);
    CHECK_EQ(g.ufcs[0].hi_dv, 200u); CHECK_EQ(g.ufcs[0].ma, 3000u);
}

// Re-running a charger with exactly the results the stored card already holds stores no second copy: the stored card (its rows,
// its boot and uptime) stays, its run count and its repeat counter grow, and the newest repeat's time is recorded. put() hands
// back that merged card, so the delta against the previous card is empty.
static void test_identical_run_is_counted_not_stored() {
    Vol vol; Store& s = *vol.s;
    rc::Card a = card(0x1111, 2, 1);
    a.boots = 4; a.uptime_s = 100;
    rc::set_name(a, "Desk Anker");
    CHECK_EQ(int(s.put(a)), int(rc::Err::Ok));
    CHECK_EQ(a.runs, 1); CHECK_EQ(a.times, 1u);
    rc::Card b = card(0x1111, 2, 1);                // the same results, a later boot and uptime
    b.boots = 5; b.uptime_s = 700;
    CHECK_EQ(int(s.put(b)), int(rc::Err::Ok));
    CHECK_EQ(b.card_no, 1); CHECK_EQ(b.runs, 2); CHECK_EQ(b.times, 2u);
    CHECK_EQ(b.boots, 4u); CHECK_EQ(b.uptime_s, 100u);                  // the run that was stored keeps its own time ...
    CHECK_EQ(b.last_boots, 5u); CHECK_EQ(b.last_uptime_s, 700u);        // ... and the repeat is the newest
    CHECK_STR(b.name, "Desk Anker");
    rc::Card g;
    CHECK_EQ(s.find(0x1111, g), 1);
    CHECK_EQ(g == b, 1);
    CHECK_EQ(all(s).size(), 1u);
    rc::DeltaLine dl[4];
    CHECK_EQ(rc::delta(a, b, dl), 0u);                                  // no change since the previous run
    rc::Card c = card(0x1111, 2, 1);
    c.boots = 5; c.uptime_s = 900;
    CHECK_EQ(int(s.put(c)), int(rc::Err::Ok));
    CHECK_EQ(c.runs, 3); CHECK_EQ(c.times, 3u); CHECK_EQ(c.last_uptime_s, 900u);
    vol.reboot();                                                       // the counters are on flash
    CHECK_EQ(vol.s->find(0x1111, g), 1);
    CHECK_EQ(g.runs, 3); CHECK_EQ(g.times, 3u); CHECK_EQ(g.last_boots, 5u); CHECK_EQ(g.last_uptime_s, 900u);
    CHECK_EQ(g.boots, 4u); CHECK_EQ(g.uptime_s, 100u);
}

// A repeat is a save: the charger moves to the front of the newest-first order like any re-test.
static void test_repeat_moves_the_card_to_the_front() {
    Vol vol; Store& s = *vol.s;
    rc::Card a = card(0x1111), b = card(0x2222);
    s.put(a); s.put(b);
    rc::Card a2 = card(0x1111);
    s.put(a2);
    CHECK_EQ(a2.times, 2u);
    const auto v = all(s);
    CHECK_EQ(v.size(), 2u);
    CHECK_EQ(v[0].id.fp, 0x1111u);
    CHECK_EQ(v[1].id.fp, 0x2222u);
}

// A run that differs in any stored row is a new run: its card replaces the stored one and the repeat counter starts over.
static void test_different_run_is_stored_and_resets_the_counter() {
    Vol vol; Store& s = *vol.s;
    rc::Card a = card(0x1111, 2, 1);
    s.put(a);
    rc::Card r = card(0x1111, 2, 1); s.put(r);
    CHECK_EQ(r.times, 2u);
    rc::Card d = card(0x1111, 2, 1);
    d.group[6].reason = 12;                                             // one reason differs
    d.boots = 8; d.uptime_s = 40;
    CHECK_EQ(int(s.put(d)), int(rc::Err::Ok));
    CHECK_EQ(d.runs, 3); CHECK_EQ(d.times, 1u);
    CHECK_EQ(d.boots, 8u); CHECK_EQ(d.uptime_s, 40u);                   // the new run is the stored card
    rc::Card g;
    CHECK_EQ(s.find(0x1111, g), 1);
    CHECK_EQ(g.group[6].reason, 12);
    CHECK_EQ(g.times, 1u);
    rc::DeltaLine dl[4];
    rc::Card e = card(0x1111, 2, 1);
    e.group[6].verdict = rc::GV::Pass;                                  // a verdict flips: stored, and the delta says so
    CHECK_EQ(int(s.put(e)), int(rc::Err::Ok));
    CHECK_EQ(e.times, 1u); CHECK_EQ(e.runs, 4);
    CHECK_EQ(rc::delta(g, e, dl), 1u);
}

// Noise below the card's resolution (100 mV): one stored step of difference in a measured maximum is the same run, two steps are not.
static void test_noise_within_the_stored_resolution_is_a_repeat() {
    Vol vol; Store& s = *vol.s;
    rc::Card a = card(0x1111, 2, 0, 9000);
    s.put(a);
    rc::Card n1 = card(0x1111, 2, 0, 9100);                             // one step up
    s.put(n1);
    CHECK_EQ(n1.times, 2u); CHECK_EQ(n1.group[0].max_mv, 9000u);        // the stored card keeps its own reading
    rc::Card n2 = card(0x1111, 2, 0, 8900);                             // one step down from the STORED value, not from the last run
    s.put(n2);
    CHECK_EQ(n2.times, 3u);
    rc::Card far = card(0x1111, 2, 0, 9200);                            // two steps from the stored 9.0 V: a real change
    s.put(far);
    CHECK_EQ(far.times, 1u); CHECK_EQ(far.group[0].max_mv, 9200u);
    // The repeat chain cannot drift: 9.2 stored, then 9.3 (one step) and 9.4 (two steps from 9.2) is new again.
    rc::Card up = card(0x1111, 2, 0, 9300); s.put(up);
    CHECK_EQ(up.times, 2u);
    rc::Card up2 = card(0x1111, 2, 0, 9400); s.put(up2);
    CHECK_EQ(up2.times, 1u);
}

// What the first run learned and the repeat did not (name, identity) stays; what only the repeat learned is added.
static void test_repeat_keeps_the_name_and_learns_the_identity() {
    Vol vol; Store& s = *vol.s;
    rc::Card a = card(0x1111);
    s.put(a);
    CHECK_EQ(int(s.rename(1, "Mine")), int(rc::Err::Ok));
    rc::Card b = card(0x1111);
    b.id.vid = 0x2717; b.id.pid = 0x0102; b.id.pdp_w = 65;              // the repeat's probe answered
    s.put(b);
    CHECK_EQ(b.times, 2u); CHECK_STR(b.name, "Mine"); CHECK_EQ(b.name_auto, 0);
    CHECK_EQ(b.id.vid, 0x2717); CHECK_EQ(b.id.pid, 0x0102); CHECK_EQ(b.id.pdp_w, 65);
    rc::Card g;
    CHECK_EQ(s.find(0x1111, g), 1);
    CHECK_EQ(g == b, 1);
}

// The counters saturate instead of wrapping.
static void test_repeat_counters_saturate() {
    Vol vol; Store& s = *vol.s;
    rc::Card a = card(0x1111);
    s.put(a);
    for (int i = 0; i < 300; ++i) { rc::Card r = card(0x1111); CHECK_EQ(int(s.put(r)), int(rc::Err::Ok)); }
    rc::Card g;
    CHECK_EQ(s.find(0x1111, g), 1);
    CHECK_EQ(g.times, 255u);
    CHECK_EQ(g.runs, 301u);
}

// A v6 card file (204 bytes: its CRC where the repeat block now starts) loads unchanged, as a card run once, and an identical
// run on top of it is a repeat that rewrites the file as v7 (210 bytes).
static void test_v6_file_loads_unchanged_and_takes_a_repeat() {
    Vol v;
    rc::Card c = card(0x6161, 2, 1, 20000);
    c.card_no = 5; c.runs = 4; c.boots = 3; c.uptime_s = 1234;
    c.ufcs[0] = {50, 200, 3000};
    rc::set_name(c, "Old six");
    std::array<std::uint8_t, rc::kMaxRecord> rec{};
    rc::encode_card(rec, c);
    std::array<std::uint8_t, 8 + 4 + 190 + 2> file{};
    std::memcpy(file.data(), rc::kFileMagic.data(), 4);
    rc::detail::put32(&file[4], 11);
    file[8] = rec[0]; file[9] = rec[1]; file[10] = 190; file[11] = 0;
    std::memcpy(&file[12], &rec[4], 190);
    file[12 + 33] = 6;                                            // the format byte of a v6 payload
    rc::detail::put16(&file[12 + 190], k2::bbstore::crc16(0xFFFF, {&file[8], 4 + 190}));
    char path[rc::Store::kPathLen];
    rc::Store::make_path(0x6161, path);
    CHECK_EQ(int(v.fs->mkdir(rc::kDir)), int(fs::Err::Ok));
    CHECK_EQ(int(v.fs->write_file(path, file)), int(fs::Err::Ok));
    v.reboot();
    rc::Card g;
    CHECK_EQ(v.s->find(0x6161, g), 1);
    CHECK_EQ(g.card_no, 5); CHECK_EQ(g.runs, 4); CHECK_STR(g.name, "Old six");
    CHECK_EQ(g.boots, 3u); CHECK_EQ(g.uptime_s, 1234u);
    CHECK_EQ(g.group[0].max_mv, 20000u); CHECK_EQ(int(g.group[6].verdict), int(rc::GV::Fail));
    CHECK_EQ(rc::ufcs_modes(g), 1u); CHECK_EQ(g.ufcs[0].ma, 3000u);
    CHECK_EQ(g.times, 1u); CHECK_EQ(g.last_boots, 0u); CHECK_EQ(g.last_uptime_s, 0u);   // the old CRC is not a repeat block
    CHECK_EQ(g.format, unsigned(rc::kFormat));
    // An identical run (same rows, same UFCS modes) on top of it is the second time.
    rc::Card n = card(0x6161, 2, 1, 20000);
    n.ufcs[0] = {50, 200, 3000}; n.boots = 6; n.uptime_s = 50;
    CHECK_EQ(int(v.s->put(n)), int(rc::Err::Ok));
    CHECK_EQ(n.times, 2u); CHECK_EQ(n.runs, 5); CHECK_EQ(n.last_boots, 6u); CHECK_EQ(n.boots, 3u);
    std::array<std::uint8_t, rc::kFileSize> back{};
    std::size_t got = 0;
    CHECK_EQ(int(v.fs->read_file(path, back, got)), int(fs::Err::Ok));
    CHECK_EQ(got, rc::kFileSize);
    CHECK_EQ(back[12 + 33], unsigned(rc::kFormat));
    CHECK_EQ(back[12 + 190], 2u);
}

// Power cut at every program/erase of a save (new card, re-test, rename, delete): after a reboot the store
// lists a consistent set, the old state or the new one, and keeps accepting saves.
static void test_power_cut_everywhere() {
    for (int fill : {0, 1, 5, 20}) {
        for (int op = 0; op < 4; ++op) {
            int total = 1;
            for (int cut = 0; cut < total + 1; ++cut) {
                Vol vol;
                for (int i = 0; i < fill; ++i) { rc::Card c = card(0x10 + i, 1 + i % 3); vol.s->put(c); }
                const auto before = all(*vol.s);
                vol.disk.reset_counters();
                vol.disk.cut_after(cut);
                rc::Card n = card(op == 0 ? 0x9000 : 0x10, 5);
                if (op <= 1) vol.s->put(n);
                else if (op == 2) vol.s->rename(1, "Renamed");
                else vol.s->remove(1);
                const bool lost = vol.disk.dead();
                if (!lost) total = cut;           // the sweep passed the end of the operation
                vol.disk.revive();
                vol.reboot();
                const auto after = all(*vol.s);
                CHECK_EQ(int(after.size()) >= int(before.size()) - (op == 3 ? 1 : 0), 1);
                CHECK_EQ(after.size() <= before.size() + 1, 1);
                for (const auto& c : after) {      // every listed card decodes, with its own number
                    rc::Card g;
                    CHECK_EQ(vol.s->get(c.card_no, g), 1);
                    CHECK_EQ(g.id.fp == c.id.fp, 1);
                }
                rc::Card m = card(0x7777);
                CHECK_EQ(int(vol.s->put(m)), int(rc::Err::Ok));
                if (!lost) break;
            }
        }
    }
}

int main() {
    test_empty_and_first_put();
    test_retest_replaces_and_keeps_name();
    test_rename_keeps_order();
    test_remove();
    test_numbers_are_newest_first();
    test_retests_keep_run_counts();
    test_forty_cards_and_capacity();
    test_v3_v4_file_sizes_are_both_accepted();
    test_v5_file_loads_and_resaves_as_v6();
    test_identical_run_is_counted_not_stored();
    test_repeat_moves_the_card_to_the_front();
    test_different_run_is_stored_and_resets_the_counter();
    test_noise_within_the_stored_resolution_is_a_repeat();
    test_repeat_keeps_the_name_and_learns_the_identity();
    test_repeat_counters_saturate();
    test_v6_file_loads_unchanged_and_takes_a_repeat();
    test_power_cut_everywhere();
    return test_report("test_report_card_store");
}
