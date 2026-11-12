// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Report card store (k2/chargertest/report_card_store.hpp) on the report-card ring log of a RAM flash that can lose power at
// any program or erase.
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "k2/chargertest/report_card_delta.hpp"
#include "k2/chargertest/report_card_store.hpp"
#include "ramflash.hpp"
#include "test_util.h"

using namespace k2;
namespace rc = k2::report_card;

// The card ring on a RAM flash; reboot() rescans it with a fresh ring and store (no RAM state survives). idle() runs the
// maintenance the main loop runs while nothing is busy, until it has nothing left to do.
struct Vol {
    k2test::RamFlash disk;
    std::unique_ptr<ringlog::Ring> ring;
    std::unique_ptr<rc::Store> s;
    unsigned reserve = rc::kRingReserve, sectors = flash::kCardRingSectors;
    explicit Vol(unsigned r = rc::kRingReserve, unsigned n = flash::kCardRingSectors) : reserve(r), sectors(n) { reboot(); }
    void reboot() {
        s.reset();
        ring = std::make_unique<ringlog::Ring>(disk, flash::kCardRingBase, sectors, rc::kRingKind, reserve);
        ring->scan();
        s = std::make_unique<rc::Store>(*ring);
    }
    unsigned idle() {
        unsigned n = 0;
        while (s->maintain(true)) ++n;
        return n;
    }
    rc::Err put(rc::Card& c) { idle(); return s->put(c); }
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
    vol.reboot();                                // the tombstone is on flash
    CHECK_EQ(all(*vol.s).size(), 1u);
    rc::Card a2 = card(0x1111);                  // the same charger again starts a new card
    vol.s->put(a2);
    CHECK_EQ(a2.runs, 1);
    CHECK_EQ(a2.card_no, 3);
    CHECK_EQ(all(*vol.s).size(), 2u);
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
    Vol vol;
    for (int round = 1; round <= 200; ++round) {     // many re-tests of three chargers: the ring goes round several times
        rc::Card c = card(0x100 + round % 3, 1 + round % 4);
        CHECK_EQ(int(vol.put(c)), int(rc::Err::Ok));
    }
    vol.reboot();
    const auto v = all(*vol.s);
    CHECK_EQ(v.size(), 3u);
    unsigned runs = 0;
    for (const auto& c : v) runs += c.runs;
    CHECK_EQ(runs, 200u);
}

// 40 cards (the target), then up to the capacity and beyond: the oldest makes room.
static void test_forty_cards_and_capacity() {
    Vol vol;
    for (unsigned i = 1; i <= 40; ++i) { rc::Card c = card(0x1000 + i); CHECK_EQ(int(vol.put(c)), int(rc::Err::Ok)); }
    vol.reboot();
    auto v = all(*vol.s);
    CHECK_EQ(v.size(), 40u);
    CHECK_EQ(v[0].id.fp, 0x1000u + 40);              // newest first
    CHECK_EQ(v[39].id.fp, 0x1001u);
    for (unsigned i = 41; i <= rc::kMaxCards + 3; ++i) { rc::Card c = card(0x1000 + i); CHECK_EQ(int(vol.put(c)), int(rc::Err::Ok)); }
    v = all(*vol.s);
    CHECK_EQ(v.size(), size_t(rc::kMaxCards));
    rc::Card g;
    CHECK_EQ(vol.s->find(0x1001, g), 0);             // the first three made room
    CHECK_EQ(vol.s->find(0x1003, g), 0);
    CHECK_EQ(vol.s->find(0x1004, g), 1);
    CHECK_EQ(vol.s->find(0x1000 + rc::kMaxCards + 3, g), 1);
    vol.reboot();
    CHECK_EQ(all(*vol.s).size(), size_t(rc::kMaxCards));
    CHECK_EQ(vol.s->find(0x1003, g), 0);             // the tombstones hold across a reboot
}

// A card record as an older firmware framed it: the stamp, then the record with a `payload` byte payload and format byte
// `format` (its CRC right after the payload), zero-padded to the record size. The ring keeps such a card until its next
// save rewrites it in the current format.
static void write_old_card(Vol& v, const rc::Card& c, unsigned payload, unsigned format, std::uint32_t stamp) {
    std::array<std::uint8_t, rc::kMaxRecord> rec{};
    rc::encode_card(rec, c);
    std::array<std::uint8_t, rc::kRecBytes> b{};
    rc::detail::put32(&b[0], stamp);
    std::uint8_t* f = &b[4];
    f[0] = rec[0]; f[1] = rec[1]; f[2] = std::uint8_t(payload); f[3] = 0;
    std::memcpy(&f[4], &rec[4], payload);
    f[4 + 33] = std::uint8_t(format);
    rc::detail::put16(&f[4 + payload], ringlog::crc16(0xFFFF, {f, 4 + payload}));
    CHECK_EQ(v.ring->append(rc::kRecCard, b), 1);
}
// The format byte of the newest record of fp in the ring.
static unsigned stored_format(Vol& v, std::uint32_t fp) {
    unsigned fmt = 0;
    v.ring->each([&](const ringlog::Record& r) {
        if (r.type == rc::kRecCard && rc::detail::get32(r.data + 4 + 4) == fp) fmt = r.data[4 + 4 + 33];
        return true;
    });
    return fmt;
}

// A v5 card (the 178 byte payload of the firmware before the UFCS modes): it lists and loads (no modes, format raised to
// the current one), and the next run of that charger stores it in the current format.
static void test_v5_file_loads_and_resaves_as_v6() {
    Vol v;
    rc::Card c = card(0x5151, 2, 1, 20000);
    c.card_no = 7; c.runs = 3;
    rc::set_name(c, "Old five");
    write_old_card(v, c, 178, 5, 9);
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
    CHECK_EQ(stored_format(v, 0x5151), unsigned(rc::kFormat));
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
    Vol vol;
    rc::Card a = card(0x1111);
    vol.put(a);
    for (int i = 0; i < 300; ++i) { rc::Card r = card(0x1111); CHECK_EQ(int(vol.put(r)), int(rc::Err::Ok)); }
    rc::Card g;
    CHECK_EQ(vol.s->find(0x1111, g), 1);
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
    write_old_card(v, c, 190, 6, 11);
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
    CHECK_EQ(stored_format(v, 0x6161), unsigned(rc::kFormat));
}

// Every card format a firmware ever framed loads from a ring record: v3 (144 byte payload) up to v10 (250).
static void test_every_older_file_size_loads() {
    struct Old { unsigned payload, format, file; };
    constexpr Old olds[] = {{144, 3, 158}, {162, 4, 176}, {178, 5, 192}, {190, 6, 204}, {196, 7, 210}, {198, 8, 212}, {214, 9, 228}, {250, 10, 264}};
    Vol v;
    std::uint32_t stamp = 1;
    for (const Old& o : olds) {
        rc::Card c = card(0xA000 + o.format, 2, 1, 15000);
        c.card_no = std::uint16_t(o.format); c.runs = 2;
        rc::set_name(c, "Old card");
        write_old_card(v, c, o.payload, o.format, stamp++);
    }
    v.reboot();
    const auto got = all(*v.s);
    CHECK_EQ(got.size(), std::size(olds));
    for (const Old& o : olds) {
        rc::Card g;
        const bool found = v.s->find(0xA000 + o.format, g);
        if (!found) std::printf("  format %u (%u B file) not listed\n", o.format, o.file);
        CHECK_EQ(found, 1);
        CHECK_EQ(g.card_no, o.format); CHECK_EQ(g.runs, 2); CHECK_STR(g.name, "Old card");
        CHECK_EQ(g.group[0].max_mv, 15000u);
    }
    CHECK_EQ(got.front().id.fp, 0xA000u + 10);   // stamp order: the newest file first
    CHECK_EQ(got.back().id.fp, 0xA000u + 3);
}

// Power cut at every program/erase of a save (new card, re-test, rename, delete): after a reboot the store
// lists a consistent set, the old state or the new one, and keeps accepting saves.
static void test_power_cut_everywhere() {
    for (int fill : {0, 1, 5, 20, 64}) {
        for (int op = 0; op < 4; ++op) {
            int total = 1;
            for (int cut = 0; cut < total + 1; ++cut) {
                Vol vol;
                for (int i = 0; i < fill; ++i) { rc::Card c = card(0x10 + i, 1 + i % 3); vol.put(c); }
                const auto before = all(*vol.s);
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

// Live card records in the ring (the latest per fingerprint, not deleted), counted from the raw records.
static unsigned live_records(Vol& v) {
    std::vector<std::uint32_t> fps;
    v.ring->each([&](const ringlog::Record& r) {
        const std::uint32_t fp = r.type == rc::kRecCard ? rc::detail::get32(r.data + 8) : rc::detail::get32(r.data);
        auto it = std::find(fps.begin(), fps.end(), fp);
        if (r.type == rc::kRecGone) { if (it != fps.end()) fps.erase(it); }
        else if (it == fps.end()) fps.push_back(fp);
        return true;
    });
    return unsigned(fps.size());
}

// More live cards than kMaxCards (a power cut between a save of a new charger and the tombstone of the oldest, or records
// written raw). The newest 64 by stamp are listed whatever the record order: here the stamps fall with the order, so an
// index that kept the first 64 records would keep the oldest cards. Reading never writes anything.
static constexpr unsigned kOver = rc::kMaxCards + 5;
static void write_over_cap(Vol& v) {
    for (unsigned i = 0; i < kOver; ++i) {
        rc::Card c = card(0x10000000u + i, 1 + i % 3);
        c.card_no = std::uint16_t(i + 1); c.runs = 1;
        write_old_card(v, c, 250, 10, 1000 - i);       // the first record is the newest card
    }
}
static void test_over_cap_lists_the_newest_by_stamp() {
    Vol v;
    write_over_cap(v);
    v.reboot();
    const int ops0 = v.disk.ops();
    const auto got = all(*v.s);
    CHECK_EQ(got.size(), std::size_t(rc::kMaxCards));
    if (!got.empty()) {
        CHECK_EQ(got.front().id.fp, 0x10000000u);                          // the newest card is listed first
        CHECK_EQ(got.back().id.fp, 0x10000000u + rc::kMaxCards - 1);       // the 5 oldest are not listed
    }
    rc::Card g;
    CHECK_EQ(v.s->find(0x10000000u, g), 1);
    CHECK_EQ(v.s->find(0x10000000u + kOver - 1, g), 0);
    CHECK_EQ(v.disk.ops(), ops0);                                          // listing writes nothing
    CHECK_EQ(live_records(v), kOver);
}

// A save of a new charger with more live cards than the cap: the oldest listed card gets its tombstone, and the cards the
// index left out are not carried forward when their sector goes, so the ring converges to kMaxCards live cards.
static void test_save_over_cap_ends_at_the_cap() {
    Vol v;
    write_over_cap(v);
    v.reboot();
    rc::Card n = card(0x7E57, 2);
    CHECK_EQ(int(v.put(n)), int(rc::Err::Ok));
    for (int k = 0; k < 120; ++k) { rc::Card r = card(0x10000000u + k % 8, 1 + k % 5); CHECK_EQ(int(v.put(r)), int(rc::Err::Ok)); }
    v.idle();
    CHECK_EQ(live_records(v), rc::kMaxCards);
    v.reboot();
    const auto got = all(*v.s);
    CHECK_EQ(got.size(), std::size_t(rc::kMaxCards));
    rc::Card g;
    CHECK_EQ(v.s->find(0x7E57u, g), 1);
    CHECK_EQ(v.s->find(0x10000000u, g), 1);                             // the newest old card stays
    CHECK_EQ(v.s->find(0x10000000u + rc::kMaxCards - 1, g), 0);         // the lowest listed stamp made room
    // every listed number is unique and leads back to its own card
    for (const auto& c : got) {
        unsigned same = 0;
        for (const auto& d : got) same += d.card_no == c.card_no;
        CHECK_EQ(same, 1u);
    }
}

// Two files with the same card number (ids were handed out again while older files were invisible). The newer card
// keeps the number; the older one shows the next free number (one above the highest in use), from the index alone:
// nothing is written until that card's own next save, which stores the new number.
static void test_duplicate_numbers_newer_keeps_it() {
    Vol v;
    rc::Card a = card(0xA1, 1); a.card_no = 5; a.runs = 1; rc::set_name(a, "Older five");
    rc::Card b = card(0xB2, 2); b.card_no = 5; b.runs = 1; rc::set_name(b, "Newer five");
    rc::Card c = card(0xC3, 3); c.card_no = 7; c.runs = 1;
    write_old_card(v, a, 250, 10, 10);
    write_old_card(v, b, 250, 10, 20);
    write_old_card(v, c, 250, 10, 15);
    v.reboot();
    const int ops0 = v.disk.ops();
    rc::Card g;
    CHECK_EQ(v.s->get(5, g), 1);
    CHECK_EQ(g.id.fp, 0xB2u);
    CHECK_EQ(v.s->get(8, g), 1);
    CHECK_EQ(g.id.fp, 0xA1u);
    CHECK_EQ(g.card_no, 8);                                 // the loaded card carries the number it is shown with
    CHECK_STR(g.name, "Older five");
    const auto got = all(*v.s);
    CHECK_EQ(got.size(), 3u);
    for (const auto& x : got) if (x.id.fp == 0xA1) CHECK_EQ(x.card_no, 8);
    std::array<std::uint16_t, 4> nums{};
    CHECK_EQ(v.s->numbers(nums), 3u);
    CHECK_EQ(nums[0], 5); CHECK_EQ(nums[1], 7); CHECK_EQ(nums[2], 8);
    CHECK_EQ(v.disk.ops(), ops0);                            // no write at indexing
    v.reboot();                                              // stable across reboots before any save
    CHECK_EQ(v.s->get(8, g), 1);
    CHECK_EQ(g.id.fp, 0xA1u);
    // A re-test of the older card stores its new number; it is now the newest, and keeps 8.
    rc::Card r = card(0xA1, 4);
    CHECK_EQ(int(v.s->put(r)), int(rc::Err::Ok));
    CHECK_EQ(r.card_no, 8);
    v.reboot();
    CHECK_EQ(v.s->get(8, g), 1);
    CHECK_EQ(g.id.fp, 0xA1u);
    CHECK_EQ(g.runs, 2);
    CHECK_EQ(v.s->get(5, g), 1);
    CHECK_EQ(g.id.fp, 0xB2u);
}

// The case that made the duplicates on hardware: a v8 card file an older build could not read kept its number 5 while
// the number went to a new v10 card. The invisible card's stamp can sit above the v10 card's (stamps were handed out
// from the cards the build could see), so the stamp alone would move the number the user has seen away from the v10
// card. A current-format file keeps its number over an older-format one; the stamp decides only between equals.
static void test_duplicate_number_stays_with_the_current_format() {
    Vol v;
    rc::Card old8 = card(0xA1, 1); old8.card_no = 5; old8.runs = 1; rc::set_name(old8, "Invisible v8");
    rc::Card cur = card(0xB2, 2); cur.card_no = 5; cur.runs = 1; rc::set_name(cur, "Current v10");
    rc::Card c = card(0xC3, 3); c.card_no = 7; c.runs = 1;
    write_old_card(v, old8, 198, 8, 30);                    // the higher stamp
    write_old_card(v, cur, 250, 10, 20);
    write_old_card(v, c, 250, 10, 15);
    v.reboot();
    const int ops0 = v.disk.ops();
    rc::Card g;
    CHECK_EQ(v.s->get(5, g), 1);
    CHECK_EQ(g.id.fp, 0xB2u);
    CHECK_EQ(v.s->get(8, g), 1);
    CHECK_EQ(g.id.fp, 0xA1u);
    CHECK_STR(g.name, "Invisible v8");
    std::array<std::uint16_t, 4> nums{};
    CHECK_EQ(v.s->numbers(nums), 3u);
    CHECK_EQ(nums[0], 8); CHECK_EQ(nums[1], 5); CHECK_EQ(nums[2], 7);   // the list order stays by stamp
    CHECK_EQ(v.disk.ops(), ops0);
}

// Many rotations of the ring with 64 chargers: maintenance carries the live cards forward, so all 64 survive every wrap,
// with their names (a rename appended long ago is carried as the card it is) and their order.
static void test_gc_keeps_every_card_across_rotations() {
    Vol v;
    for (unsigned i = 0; i < rc::kMaxCards; ++i) { rc::Card c = card(0x2000 + i, 1 + i % 4); CHECK_EQ(int(v.put(c)), int(rc::Err::Ok)); }
    CHECK_EQ(int(v.s->rename(1, "First one")), int(rc::Err::Ok));
    const std::uint64_t erases0 = v.disk.erases;
    for (int k = 0; k < 400; ++k) {                       // re-tests of eight chargers only: the other cards must be carried
        rc::Card c = card(0x2000 + 10 + k % 8, 1 + k % 5, k % 2);
        CHECK_EQ(int(v.put(c)), int(rc::Err::Ok));
    }
    CHECK_EQ(v.disk.erases > erases0 + 2 * flash::kCardRingSectors, 1);   // the ring went round more than twice
    v.reboot();
    const auto got = all(*v.s);
    CHECK_EQ(got.size(), std::size_t(rc::kMaxCards));
    rc::Card g;
    for (unsigned i = 0; i < rc::kMaxCards; ++i) CHECK_EQ(v.s->find(0x2000 + i, g), 1);
    CHECK_EQ(v.s->get(1, g), 1);
    CHECK_EQ(g.id.fp, 0x2000u);
    CHECK_STR(g.name, "First one");
    for (const auto& c : got) {                          // numbers stay unique
        unsigned same = 0;
        for (const auto& d : got) same += d.card_no == c.card_no;
        CHECK_EQ(same, 1u);
    }
}

// A deleted card stays deleted through any number of wraps: its tombstone may be erased only with or after the records
// it kills, and the dead card is never carried forward.
static void test_tombstone_survives_gc() {
    Vol v;
    for (unsigned i = 0; i < 20; ++i) { rc::Card c = card(0x3000 + i); CHECK_EQ(int(v.put(c)), int(rc::Err::Ok)); }
    CHECK_EQ(int(v.s->remove(3)), int(rc::Err::Ok));      // card 3 = 0x3002
    for (int k = 0; k < 300; ++k) {
        rc::Card c = card(0x3000 + 10 + k % 4, 1 + k % 5);
        CHECK_EQ(int(v.put(c)), int(rc::Err::Ok));
        if (k % 37 == 0) {
            v.reboot();
            rc::Card g;
            CHECK_EQ(v.s->find(0x3002, g), 0);
            CHECK_EQ(v.s->count(), 19u);
        }
    }
    v.reboot();
    rc::Card g;
    CHECK_EQ(v.s->find(0x3002, g), 0);
    CHECK_EQ(v.s->find(0x3001, g), 1);
    CHECK_EQ(v.s->count(), 19u);
    CHECK_EQ(live_records(v), 19u);
}

// No erase without maintenance: a put on a ring with no blank sector left fails with Full and erases nothing; the next
// idle maintenance makes room and the save then goes through.
static void test_full_ring_refuses_without_erase() {
    Vol v;
    int k = 0;
    rc::Err e = rc::Err::Ok;
    const std::uint64_t erases0 = v.disk.erases;
    for (; k < 500 && e == rc::Err::Ok; ++k) { rc::Card c = card(0x4000 + k % 6, 1 + k % 5); e = v.s->put(c); }
    CHECK_EQ(int(e), int(rc::Err::Full));
    CHECK_EQ(v.disk.erases, erases0);                     // put never erases
    CHECK_EQ(k > int(7 * (flash::kCardRingSectors - rc::kRingReserve)) - 2, 1);   // every sector but the reserve was used
    CHECK_EQ(v.s->needs_maintenance(), 1);
    CHECK_EQ(v.s->maintain(false), 0);                    // busy: no erase
    CHECK_EQ(v.disk.erases, erases0);
    CHECK_EQ(v.idle() > 0u, 1);
    CHECK_EQ(v.s->needs_maintenance(), 0);
    rc::Card c = card(0x4000, 3);
    CHECK_EQ(int(v.s->put(c)), int(rc::Err::Ok));
    v.reboot();
    CHECK_EQ(v.s->count(), 6u);
}

// Power cut at every program and erase of a maintenance pass over a full ring of 64 cards: after a reboot every card is
// still listed once, and the next maintenance finishes the job.
static void test_power_cut_during_maintenance() {
    for (int cut = 0;; ++cut) {
        Vol v;
        for (unsigned i = 0; i < rc::kMaxCards; ++i) { rc::Card c = card(0x5000 + i, 1 + i % 4); v.put(c); }
        for (int k = 0;; ++k) {                           // re-tests until no blank sector is left
            rc::Card c = card(0x5000 + k % 3, 1 + k % 5);
            if (v.s->put(c) != rc::Err::Ok) break;
        }
        v.disk.cut_after(cut);
        v.idle();
        const bool lost = v.disk.dead();
        v.disk.revive();
        v.reboot();
        CHECK_EQ(v.s->count(), std::size_t(rc::kMaxCards));
        rc::Card g;
        for (unsigned i = 0; i < rc::kMaxCards; ++i) CHECK_EQ(v.s->find(0x5000 + i, g), 1);
        v.idle();
        CHECK_EQ(v.s->needs_maintenance(), 0);
        rc::Card n = card(0x5001, 5);
        CHECK_EQ(int(v.s->put(n)), int(rc::Err::Ok));
        v.reboot();
        CHECK_EQ(v.s->count(), std::size_t(rc::kMaxCards));
        if (!lost) break;
    }
}

// A long random mix of new chargers, re-tests, renames and deletes over a pool larger than the cap, with maintenance
// sometimes skipped (the ring runs full and the store must pick a sector other than the oldest to give back) and reboots
// between: the store lists exactly the cards of a plain model, newest first. A tombstone carried forward out of a middle
// sector keeps a deleted card deleted.
static void test_random_ops_match_a_model() {
    struct M { std::uint32_t fp, stamp; };
    std::vector<M> model;
    std::uint32_t top = 0, rng = 12345;
    const auto rnd = [&](std::uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; };
    const auto at = [&](std::uint32_t fp) { for (std::size_t i = 0; i < model.size(); ++i) if (model[i].fp == fp) return int(i); return -1; };
    Vol v;
    for (int step = 0; step < 3000; ++step) {
        const std::uint32_t fp = 0x9000 + rnd(90);
        const unsigned op = rnd(10);
        if (rnd(4) != 0) v.idle();
        if (op < 7) {
            rc::Card c = card(fp, 1 + int(rnd(6)), int(rnd(2)), 5000 + rnd(9) * 1000);
            if (v.s->put(c) != rc::Err::Ok) continue;          // ring full until the next maintenance
            if (const int i = at(fp); i >= 0) model[std::size_t(i)].stamp = ++top;
            else {
                if (model.size() == rc::kMaxCards) {
                    std::size_t lo = 0;
                    for (std::size_t i = 1; i < model.size(); ++i) if (model[i].stamp < model[lo].stamp) lo = i;
                    model.erase(model.begin() + long(lo));
                }
                model.push_back({fp, ++top});
            }
        } else if (op < 9) {
            rc::Card g;
            if (!v.s->find(fp, g)) continue;
            const rc::Err e = op == 7 ? v.s->remove(g.card_no) : v.s->rename(g.card_no, "Bench");
            if (e != rc::Err::Ok) continue;
            if (op == 7) model.erase(model.begin() + at(fp));
        } else {
            v.reboot();
        }
        if (step % 50 == 0 || op == 9) {
            std::vector<M> want = model;
            std::sort(want.begin(), want.end(), [](const M& a, const M& b) { return a.stamp > b.stamp; });
            const auto got = all(*v.s);
            CHECK_EQ(got.size(), want.size());
            for (std::size_t i = 0; i < got.size() && i < want.size(); ++i) CHECK_EQ(got[i].id.fp, want[i].fp);
        }
    }
    v.idle();
    v.reboot();
    CHECK_EQ(v.s->count(), model.size());
    rc::Card g;
    for (const M& m : model) CHECK_EQ(v.s->find(m.fp, g), 1);
}

// A deleted card's tombstone sits in a middle sector, its card record in the oldest one. A power cut in a maintenance
// pass of the full ring leaves too little room to carry the oldest sector forward, so the store gives back the middle
// sector instead and must carry the tombstone with it: the card stays deleted, every other card stays listed.
static void test_tombstone_carried_out_of_a_middle_sector() {
    int middle = 0;
    for (int cut = 0;; ++cut) {
        // The geometry this case was built on (14 sectors, a reserve of 2): one cut then uses up the slack and reaches the
        // middle-sector path of victim(). The shipped ring (22 sectors, reserve 4) needs three such cuts in a row; the
        // path is the same code either way.
        Vol v(2, 14);                                   // no maintenance until the ring is full: put() straight to the store
        for (unsigned i = 0; i < 7; ++i) { rc::Card c = card(0x6000 + i); v.s->put(c); }       // sector 0: A..G
        rc::Card h = card(0x6007);
        v.s->put(h);                                                                           // sector 1: H
        rc::Card g;
        CHECK_EQ(v.s->find(0x6000, g), 1);
        CHECK_EQ(int(v.s->remove(g.card_no)), int(rc::Err::Ok));                               // A's tombstone, sector 1
        for (int k = 0; k < 6; ++k) { rc::Card c = card(0x6007, 1 + k % 6, 0, 5000); v.s->put(c); }   // H re-tests, sector 1
        for (unsigned i = 8; i < 64; ++i) { rc::Card c = card(0x6100 + i); v.s->put(c); }
        h = card(0x6007, 3, 1);
        v.s->put(h);                                                     // sector 1 now carries the tombstone only
        for (unsigned k = 0;; ++k) {                                     // re-tests spread over the sectors, to the reserve
            rc::Card c = card(0x6108 + (k * 7 + k / 8) % 56, 1 + int(k % 6), 1);
            if (v.s->put(c) != rc::Err::Ok) break;
        }
        CHECK_EQ(v.ring->blank(), v.reserve);
        const std::uint32_t tail = v.ring->tail();
        v.disk.cut_after(cut);
        v.idle();
        const bool lost = v.disk.dead();
        v.disk.revive();
        v.reboot();
        CHECK_EQ(v.s->find(0x6000, g), 0);
        CHECK_EQ(v.s->count(), 63u);
        v.s->maintain(true);
        if (v.ring->state(1) == ringlog::Sec::Blank && v.ring->tail() == tail) ++middle;   // gave back sector 1 first
        v.reboot();
        CHECK_EQ(v.s->find(0x6000, g), 0);
        v.idle();
        v.reboot();
        CHECK_EQ(v.s->find(0x6000, g), 0);
        CHECK_EQ(v.s->count(), 63u);
        CHECK_EQ(v.s->needs_maintenance(), 0);
        if (!lost) break;
    }
    CHECK_EQ(middle > 0, 1);
}

int main() {
    test_empty_and_first_put();
    test_retest_replaces_and_keeps_name();
    test_rename_keeps_order();
    test_remove();
    test_numbers_are_newest_first();
    test_retests_keep_run_counts();
    test_forty_cards_and_capacity();
    test_v5_file_loads_and_resaves_as_v6();
    test_identical_run_is_counted_not_stored();
    test_repeat_moves_the_card_to_the_front();
    test_different_run_is_stored_and_resets_the_counter();
    test_noise_within_the_stored_resolution_is_a_repeat();
    test_repeat_keeps_the_name_and_learns_the_identity();
    test_repeat_counters_saturate();
    test_v6_file_loads_unchanged_and_takes_a_repeat();
    test_power_cut_everywhere();
    test_every_older_file_size_loads();
    test_over_cap_lists_the_newest_by_stamp();
    test_save_over_cap_ends_at_the_cap();
    test_duplicate_numbers_newer_keeps_it();
    test_duplicate_number_stays_with_the_current_format();
    test_gc_keeps_every_card_across_rotations();
    test_tombstone_survives_gc();
    test_full_ring_refuses_without_erase();
    test_power_cut_during_maintenance();
    test_random_ops_match_a_model();
    test_tombstone_carried_out_of_a_middle_sector();
    return test_report("test_report_card_store");
}
