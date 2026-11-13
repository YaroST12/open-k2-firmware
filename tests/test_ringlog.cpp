// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the append-only ring log on raw flash (k2/core/ringlog.hpp): records survive a rescan in order, a full
// ring refuses an append without erasing, the reserve is kept, the oldest sector goes on wrap, a torn record or sector
// header is skipped and never fatal, and a power cut at any program or erase leaves a readable ring.
#include <cstdio>
#include <string>
#include <vector>

#include "k2/core/ringlog.hpp"
#include "ramflash.hpp"
#include "test_util.h"

using namespace k2;
using k2test::RamFlash;

namespace {

constexpr std::uint32_t kBase = flash::kFsRunBase;   // any sectors of the run
constexpr std::uint8_t kKind = 'T';

std::vector<std::uint8_t> payload(unsigned n, unsigned salt) {
    std::vector<std::uint8_t> v(n);
    for (unsigned i = 0; i < n; ++i) v[i] = std::uint8_t(i * 13 + salt * 7 + 1);
    return v;
}

struct Seen {
    std::vector<std::uint32_t> salts;   // the type byte carries the salt the payload was made with
    std::vector<std::uint16_t> lens;
    std::vector<std::uint32_t> seqs;
    bool bad = false;
};

Seen read_all(ringlog::Ring& r) {
    Seen s;
    r.each([&](const ringlog::Record& rec) {
        s.salts.push_back(rec.type);
        s.lens.push_back(rec.len);
        s.seqs.push_back(rec.seq);
        const auto want = payload(rec.len, rec.type);
        // an empty record has no bytes to compare (and an empty vector's data() is null, which memcmp must not get)
        if (rec.len && std::memcmp(rec.data, want.data(), rec.len) != 0) s.bad = true;
        return true;
    });
    return s;
}

bool add(ringlog::Ring& r, unsigned salt, unsigned len, bool reserve = false) {
    const auto p = payload(len, salt);
    // split in an even head and the rest, as the stores do (a header span, then the body)
    const std::size_t a = len >= 4 ? 4 : 0;
    return r.append(std::uint8_t(salt), {p.data(), a}, {p.data() + a, len - a}, reserve);
}

void test_append_and_rescan() {
    RamFlash f;
    {
        ringlog::Ring r(f, kBase, 4, kKind);
        r.scan();
        CHECK_EQ(r.blank(), 4u);
        CHECK_EQ(add(r, 1, 100), 1);
        CHECK_EQ(add(r, 2, 7), 1);     // odd length: padded
        CHECK_EQ(add(r, 3, 0), 1);     // empty record
        CHECK_EQ(r.blank(), 3u);
        CHECK_EQ(r.live(), 1u);
    }
    ringlog::Ring r(f, kBase, 4, kKind);
    r.scan();
    const Seen s = read_all(r);
    CHECK_EQ(s.salts.size(), 3u);
    CHECK_EQ(s.bad, 0);
    CHECK_EQ(s.salts[0], 1u); CHECK_EQ(s.salts[1], 2u); CHECK_EQ(s.salts[2], 3u);
    CHECK_EQ(s.lens[1], 7u);
    CHECK_COND(s.seqs[0] < s.seqs[1] && s.seqs[1] < s.seqs[2]);
    CHECK_EQ(add(r, 4, 10), 1);    // appends continue behind the old ones
    CHECK_EQ(read_all(r).salts.back(), 4u);
}

void test_records_fill_sectors_in_order() {
    RamFlash f;
    ringlog::Ring r(f, kBase, 4, kKind);
    r.scan();
    const unsigned per = ringlog::per_sector(480);
    CHECK_EQ(per, 4u);
    unsigned n = 0;
    while (add(r, n + 1, 480)) ++n;
    CHECK_EQ(n, 4 * per);          // no reserve: every sector is used
    CHECK_EQ(r.blank(), 0u);
    const auto e0 = f.erases;
    CHECK_EQ(add(r, 99, 480), 0);  // full: refused, nothing erased
    CHECK_EQ(f.erases, e0);
    ringlog::Ring q(f, kBase, 4, kKind);
    q.scan();
    const Seen s = read_all(q);
    CHECK_EQ(s.salts.size(), n);
    for (unsigned i = 0; i < n; ++i) CHECK_EQ(s.salts[i], i + 1);
    CHECK_EQ(s.bad, 0);
}

void test_reserve_is_kept_for_the_caller() {
    RamFlash f;
    ringlog::Ring r(f, kBase, 3, kKind, 1);
    r.scan();
    unsigned n = 0;
    while (add(r, n + 1, 1000)) ++n;
    CHECK_EQ(n, 2u * ringlog::per_sector(1000));   // the last blank sector is not taken
    CHECK_EQ(r.blank(), 1u);
    CHECK_EQ(add(r, 50, 1000, true), 1);           // the caller's own use of the reserve (carrying records forward)
    CHECK_EQ(r.blank(), 0u);
}

void test_wrap_erases_the_oldest_sector() {
    RamFlash f;
    ringlog::Ring r(f, kBase, 3, kKind);
    r.scan();
    unsigned salt = 1;
    for (int round = 0; round < 7; ++round) {
        while (add(r, salt, 900)) ++salt;
        const std::uint32_t t = r.tail();
        CHECK_COND(t != 0);
        CHECK_EQ(r.erase(t), 1);
        CHECK_EQ(r.blank(), 1u);
    }
    ringlog::Ring q(f, kBase, 3, kKind);
    q.scan();
    const Seen s = read_all(q);
    CHECK_EQ(s.salts.size(), 2u * ringlog::per_sector(900));   // two full sectors survive, the newest
    for (std::size_t i = 1; i < s.salts.size(); ++i) CHECK_EQ(s.salts[i], s.salts[i - 1] + 1);
    CHECK_EQ(s.salts.back(), salt - 1);
    CHECK_EQ(s.bad, 0);
}

// A record torn by a power cut is skipped by its length; the records around it are intact and the ring goes on.
void test_torn_record_is_skipped() {
    RamFlash f;
    ringlog::Ring r(f, kBase, 2, kKind);
    r.scan();
    CHECK_EQ(add(r, 1, 300), 1);
    f.cut_after(1);                  // header written, the payload torn half way
    CHECK_EQ(add(r, 2, 300), 0);
    f.revive();
    ringlog::Ring q(f, kBase, 2, kKind);
    q.scan();
    CHECK_EQ(read_all(q).salts.size(), 1u);
    CHECK_EQ(add(q, 3, 300), 1);
    ringlog::Ring z(f, kBase, 2, kKind);
    z.scan();
    const Seen s = read_all(z);
    CHECK_EQ(s.salts.size(), 2u);
    CHECK_EQ(s.salts[0], 1u); CHECK_EQ(s.salts[1], 3u);
    CHECK_EQ(s.bad, 0);
}

// A sector whose header was torn (or that holds anything not ours) is Dirty: never appended to, erased by maintenance.
void test_dirty_sectors() {
    RamFlash f;
    ringlog::Ring r(f, kBase, 3, kKind);
    r.scan();
    f.cut_after(0);                  // the very first program (the sector header) is torn
    CHECK_EQ(add(r, 1, 100), 0);
    f.revive();
    std::uint8_t junk[4] = {1, 2, 3, 4};
    f.program(kBase + 2 * flash::kSectorSize + 100, junk);   // a stray write in another sector
    ringlog::Ring q(f, kBase, 3, kKind);
    q.scan();
    CHECK_EQ(q.dirty(), 2u);
    CHECK_EQ(q.blank(), 1u);
    CHECK_EQ(add(q, 2, 100), 1);
    const std::uint32_t d = q.first_dirty();
    CHECK_COND(d != 0);
    CHECK_EQ(q.erase(d), 1);
    CHECK_EQ(q.dirty(), 1u);
    // another kind's sector is not ours either
    ringlog::Ring other(f, kBase, 3, 'X');
    other.scan();
    CHECK_EQ(other.live(), 0u);
    CHECK_EQ(other.dirty(), 2u);
}

// Sectors on the skip mask are neither read nor written (the migration's view of blocks the old volume still uses).
void test_skip_mask() {
    RamFlash f;
    std::uint8_t junk[4] = {1, 2, 3, 4};
    f.program(kBase + 100, junk);
    ringlog::Ring r(f, kBase, 3, kKind);
    r.scan(0b001);
    CHECK_EQ(r.dirty(), 0u);
    CHECK_EQ(r.blank(), 2u);
    while (add(r, 1, 1500)) {}
    CHECK_COND(f.view(kBase)[100] == 1);   // untouched
}

// Bytes in the free part of a sector that are not blank (an erase cut short can leave them): the sector is closed and the
// record goes to the next one.
void test_non_blank_tail_closes_the_sector() {
    RamFlash f;
    ringlog::Ring r(f, kBase, 2, kKind);
    r.scan();
    CHECK_EQ(add(r, 1, 100), 1);
    std::uint8_t junk[2] = {0, 0};
    f.program(kBase + 1500, junk);
    ringlog::Ring q(f, kBase, 2, kKind);
    q.scan();
    CHECK_EQ(add(q, 2, 1600), 1);
    CHECK_EQ(q.blank(), 0u);
    const Seen s = read_all(q);
    CHECK_EQ(s.salts.size(), 2u);
    CHECK_EQ(s.bad, 0);
}

// Power cut at every program and erase of a long sequence of appends with wraps: after any cut the ring rescans, shows a
// gap-free run of the records written (the newest maybe lost, never a torn one), and takes appends again.
void test_power_cut_sweep() {
    auto run = [](RamFlash& f, int& done) {
        ringlog::Ring r(f, kBase, 3, kKind);
        r.scan();
        for (unsigned salt = 1; salt <= 30; ++salt) {
            if (r.blank() == 0) {
                if (!r.erase(r.tail())) return false;
            }
            if (!add(r, salt, 400 + salt * 10)) return false;
            ++done;
        }
        return true;
    };
    RamFlash ref;
    int n = 0;
    CHECK_EQ(run(ref, n), 1);
    const int total = ref.ops();
    int failures = 0;
    for (int cut = 0; cut < total; ++cut) {
        RamFlash f;
        f.cut_after(cut);
        int done = 0;
        run(f, done);
        f.revive();
        ringlog::Ring r(f, kBase, 3, kKind);
        r.scan();
        const Seen s = read_all(r);
        bool ok = !s.bad;
        for (std::size_t i = 1; i < s.salts.size(); ++i) ok = ok && s.salts[i] == s.salts[i - 1] + 1;
        if (!s.salts.empty()) ok = ok && s.salts.back() <= unsigned(done + 1) && s.salts.back() >= unsigned(done);
        if (done > 0 && s.salts.empty()) ok = false;
        // the ring goes on: maintenance, then an append
        if (r.first_dirty()) r.erase(r.first_dirty());
        if (r.blank() == 0) r.erase(r.tail());
        ok = ok && add(r, 77, 300);
        if (!ok) {
            ++failures;
            if (failures < 5) std::printf("cut %d: done=%d seen=%zu last=%u\n", cut, done, s.salts.size(), s.salts.empty() ? 0 : s.salts.back());
        }
    }
    CHECK_EQ(failures, 0);
}

}  // namespace

int main() {
    test_append_and_rescan();
    test_records_fill_sectors_in_order();
    test_reserve_is_kept_for_the_caller();
    test_wrap_erases_the_oldest_sector();
    test_torn_record_is_skipped();
    test_dirty_sectors();
    test_skip_mask();
    test_non_blank_tail_closes_the_sector();
    test_power_cut_sweep();
    return test_report("test_ringlog");
}
