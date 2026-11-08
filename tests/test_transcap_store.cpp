// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Transition capture store (k2/pd/transcap_store.hpp) on a RAM disk that can lose power at any operation.
#include <memory>
#include <vector>

#include "k2/pd/transcap_store.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::transcap;
using Rec = Recorder<1920, 64>;

struct Vol {
    k2test::RamDisk disk{96};
    std::unique_ptr<fs::Fs> fs;
    std::unique_ptr<Store> s;
    Vol() { reboot(); }
    void reboot() {
        s.reset();
        fs = std::make_unique<fs::Fs>(disk, 96);
        bool fm = false;
        fs->mount_or_format(fm);
        s = std::make_unique<Store>(*fs);
    }
};

static Rec trace(unsigned salt, std::size_t n = 1200) {
    Rec r;
    r.arm();
    std::uint32_t t = 5'000'000 + salt;
    unsigned seed = salt + 1;
    for (std::size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        t += 1000 + ((seed >> 16) % 9 == 0 ? 130 : 0);
        r.push({t, std::uint16_t(i < n / 2 ? 3300 + (seed >> 20) % 3 : 3300 - (i - n / 2) * 4), std::uint8_t(90 + (seed >> 24) % 2), 7});
    }
    r.mark(5'000'000 + 2000, Mark::TxRequest, 2, 3300);
    r.mark(5'000'000 + 2600, Mark::Accept, 2);
    r.trigger(t, Reason::ShutOff);
    return r;
}

static std::vector<std::uint8_t> read_all(Store& s, std::uint32_t seq) {
    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 100> b;
    for (std::uint32_t off = 0;; off += 100) {
        std::size_t n = 0;
        if (s.read(seq, off, b, n) != fs::Err::Ok || n == 0) break;
        out.insert(out.end(), b.begin(), b.begin() + n);
    }
    return out;
}

static void test_meta_pack_roundtrip() {
    Meta m;
    m.seq = 0xA1B2C3D4; m.run_id = 77; m.step = 513; m.reason = Reason::NoPsRdy;
    m.thumb.vmax_mv = 20000; m.thumb.accept_col = 33; m.thumb.trig_col = 90;
    for (std::size_t c = 0; c < kThumbCols; c++) { m.thumb.lo[c] = std::uint8_t(c); m.thumb.hi[c] = std::uint8_t(255 - c); }
    std::array<std::uint8_t, kMetaBytes> b;
    pack_meta(m, b);
    Meta g;
    CHECK_EQ(unpack_meta(b, g), true);
    CHECK_EQ(g.seq, 0xA1B2C3D4u); CHECK_EQ(g.run_id, 77); CHECK_EQ(g.step, 513);
    CHECK_EQ(int(g.reason), int(Reason::NoPsRdy));
    CHECK_EQ(g.thumb.vmax_mv, 20000); CHECK_EQ(g.thumb.accept_col, 33); CHECK_EQ(g.thumb.trig_col, 90);
    CHECK_EQ(g.thumb.lo[5], 5); CHECK_EQ(g.thumb.hi[5], 250);
    b[0] = 'X';
    CHECK_EQ(unpack_meta(b, g), false);
    CHECK_EQ(unpack_meta({b.data(), 10}, g), false);
}

static void test_save_list_find_and_read_back() {
    Vol v;
    const Rec r = trace(1);
    std::uint32_t seq = 0;
    CHECK_EQ(int(v.s->save(r, 42, 7, seq)), int(fs::Err::Ok));
    CHECK_EQ(seq, 1);
    std::array<Meta, 8> l;
    CHECK_EQ(v.s->list(l), 1);
    CHECK_EQ(l[0].run_id, 42); CHECK_EQ(l[0].step, 7); CHECK_EQ(int(l[0].reason), int(Reason::ShutOff));
    Meta f;
    CHECK_EQ(v.s->find(42, 7, f), true);
    CHECK_EQ(v.s->find(42, 8, f), false);
    CHECK_EQ(v.s->find(43, 7, f), false);
    // The stored stream decodes to the same capture, also after a reboot.
    v.reboot();
    const auto bytes = read_all(*v.s, seq);
    std::size_t sz = 0;
    CHECK_EQ(int(v.s->size(seq, sz)), int(fs::Err::Ok));
    CHECK_EQ(sz, bytes.size());
    std::vector<Sample> sm(1920);
    std::vector<Marker> mk(64);
    Decoded d;
    CHECK_EQ(decode(bytes, sm, mk, d), true);
    CHECK_EQ(d.n_samples, 1200);
    CHECK_EQ(sm[600].vbus_mv, r.sample(600).vbus_mv);
    CHECK_EQ(sm[1199].t_us, r.sample(1199).t_us);
    CHECK_EQ(int(d.reason), int(Reason::ShutOff));
}

static void test_oldest_dropped_beyond_the_bound() {
    Vol v;
    std::uint32_t seq = 0;
    for (unsigned i = 0; i < kMaxStored + 3; i++) CHECK_EQ(int(v.s->save(trace(i), 100 + i, std::uint16_t(i), seq)), int(fs::Err::Ok));
    std::array<Meta, 16> l;
    const std::size_t n = v.s->list(l);
    CHECK_EQ(n, kMaxStored);
    CHECK_EQ(l[0].seq, kMaxStored + 3);           // newest first
    CHECK_EQ(l[n - 1].seq, 4);                    // seqs 1..3 are gone
    CHECK_EQ(read_all(*v.s, 1).size(), 0u);       // their data files too
    CHECK_EQ(read_all(*v.s, 4).size() > 0, true);
}

static void test_clear() {
    Vol v;
    std::uint32_t seq = 0;
    v.s->save(trace(1), 1, 1, seq);
    v.s->save(trace(2), 1, 2, seq);
    v.s->clear();
    std::array<Meta, 8> l;
    CHECK_EQ(v.s->list(l), 0);
}

static void test_sequence_continues_after_reboot() {
    Vol v;
    std::uint32_t seq = 0;
    v.s->save(trace(1), 1, 1, seq);
    v.reboot();
    v.s->save(trace(2), 1, 2, seq);
    CHECK_EQ(seq, 2);
}

// Power cut at every program/erase of a save: the store is consistent afterwards, holds the old captures
// and either the new one complete or not at all; the next save works.
static void test_power_cut_sweep() {
    // A reference run, to count the operations of one save onto a volume that already holds two captures.
    int total = 0;
    {
        Vol v;
        std::uint32_t seq = 0;
        v.s->save(trace(1), 1, 1, seq);
        v.s->save(trace(2), 1, 2, seq);
        v.disk.reset_counters();
        v.s->save(trace(3), 1, 3, seq);
        total = v.disk.ops_done();
    }
    CHECK_EQ(total > 4, true);
    int complete = 0, absent = 0;
    for (int cut = 0; cut <= total; cut += 1) {
        Vol v;
        std::uint32_t seq = 0;
        v.s->save(trace(1), 1, 1, seq);
        v.s->save(trace(2), 1, 2, seq);
        v.disk.cut_after(cut);
        v.s->save(trace(3), 1, 3, seq);
        v.disk.revive();
        v.reboot();
        std::array<Meta, 16> l;
        const std::size_t n = v.s->list(l);
        bool have3 = false;
        for (std::size_t i = 0; i < n; i++) if (l[i].step == 3) have3 = true;
        CHECK_EQ(n >= 2 && n <= 3, true);
        if (have3) {
            // a listed capture always has a whole, decodable data file
            const auto bytes = read_all(*v.s, l[0].seq);
            std::vector<Sample> sm(1920);
            std::vector<Marker> mk(64);
            Decoded d;
            CHECK_EQ(decode(bytes, sm, mk, d), true);
            ++complete;
        } else ++absent;
        std::uint32_t s2 = 0;
        CHECK_EQ(int(v.s->save(trace(4), 1, 4, s2)), int(fs::Err::Ok));   // the volume is still usable
        std::array<Meta, 16> l2;
        CHECK_EQ(v.s->list(l2) <= kMaxStored, true);
    }
    CHECK_EQ(complete > 0 && absent > 0, true);
}

int main() {
    test_meta_pack_roundtrip();
    test_save_list_find_and_read_back();
    test_oldest_dropped_beyond_the_bound();
    test_clear();
    test_sequence_continues_after_reboot();
    test_power_cut_sweep();
    return test_report("test_transcap_store");
}
