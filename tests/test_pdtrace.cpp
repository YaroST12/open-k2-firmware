// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black-box ring: whole-record wrap, cursor reads, lost / reset / frozen flags, sample gating.
#include <vector>

#include "k2/pd/pdtrace.hpp"
#include "test_util.h"

using namespace k2::pdtrace;

namespace {
using R = Ring<600>;

void push_n(R& r, std::uint8_t tag, std::size_t payload, std::uint32_t ts = 0) {
    std::vector<std::uint8_t> p(payload, tag);
    r.push(Type::Mark, ts, p);
}
// Records in `out[0..n)`: the tag byte of each (first payload byte), or 0 for an empty payload.
std::vector<int> tags(const std::uint8_t* out, std::size_t n) {
    std::vector<int> t;
    for (std::size_t i = 0; i < n; i += out[i]) t.push_back(out[i] > kHeader ? out[i + kHeader] : 0);
    return t;
}
}  // namespace

int main() {
    {   // record framing: len, type, timestamp little endian, payload
        R r;
        const std::uint8_t pl[] = {0xAA, 0xBB};
        r.push(Type::Reg, 0x11223344, pl);
        std::uint8_t out[64];
        const auto rd = r.read(0, out);
        CHECK_EQ(rd.bytes, 8u);
        CHECK_EQ(out[0], 8); CHECK_EQ(out[1], int(Type::Reg));
        CHECK_EQ(out[2], 0x44); CHECK_EQ(out[5], 0x11);
        CHECK_EQ(out[6], 0xAA); CHECK_EQ(out[7], 0xBB);
        CHECK_EQ(rd.next, 8u); CHECK_EQ(rd.flags, 0);
    }
    {   // incremental cursor: the second read returns only what came after the first
        R r;
        push_n(r, 1, 4); push_n(r, 2, 4);
        std::uint8_t out[64];
        auto a = r.read(0, out);
        CHECK_EQ(tags(out, a.bytes).size(), 2u);
        push_n(r, 3, 4);
        auto b = r.read(a.next, out);
        CHECK_EQ(b.bytes, 10u);
        CHECK_EQ(tags(out, b.bytes)[0], 3);
        auto c = r.read(b.next, out);
        CHECK_EQ(c.bytes, 0u); CHECK_EQ(c.next, b.next);
    }
    {   // a small output buffer returns whole records only and the cursor continues exactly there
        R r;
        for (int i = 1; i <= 5; i++) push_n(r, i, 4);   // 10 bytes each
        std::uint8_t out[25];
        auto a = r.read(0, out);
        CHECK_EQ(a.bytes, 20u); CHECK_EQ(a.next, 20u);
        auto b = r.read(a.next, out);
        CHECK_EQ(tags(out, b.bytes)[0], 3);
    }
    {   // wrap: the oldest whole records are dropped, never a partial one
        R r;
        for (int i = 1; i <= 100; i++) push_n(r, std::uint8_t(i), 3 + i % 7);
        CHECK_EQ(r.head() - r.tail() <= 600, 1);
        std::uint8_t out[600];
        auto a = r.read(r.tail(), out);
        CHECK_EQ(a.flags, 0);
        const auto t = tags(out, a.bytes);
        CHECK_EQ(t.back(), 100);
        for (std::size_t i = 1; i < t.size(); i++) CHECK_EQ(t[i], t[i - 1] + 1);   // contiguous run, no gap
        CHECK_EQ(a.next, r.head());
    }
    {   // a cursor that fell behind the tail is moved forward and flagged Lost
        R r;
        for (int i = 0; i < 100; i++) push_n(r, 9, 10);
        std::uint8_t out[600];
        auto a = r.read(0, out);
        CHECK_EQ(a.flags & kFlagLost, kFlagLost);
        CHECK_EQ(a.bytes, r.head() - r.tail());
    }
    {   // a cursor beyond the head (device rebooted) restarts at the tail with the Reset flag
        R r;
        push_n(r, 1, 2);
        std::uint8_t out[64];
        auto a = r.read(5000, out);
        CHECK_EQ(a.flags & kFlagReset, kFlagReset);
        CHECK_EQ(a.bytes, 8u);
    }
    {   // clear keeps offsets monotonic; freeze stops recording and is reported
        R r;
        push_n(r, 1, 2); push_n(r, 2, 2);
        std::uint8_t out[64];
        auto a = r.read(0, out);
        r.clear();
        CHECK_EQ(r.read(a.next, out).bytes, 0u);
        r.freeze(true);
        CHECK_EQ(r.push(Type::Mark, 0, {}), 0);
        CHECK_EQ(r.missed(), 1u);
        CHECK_EQ(r.read(a.next, out).flags & kFlagFrozen, kFlagFrozen);
        r.freeze(false);
        CHECK_EQ(r.push(Type::Mark, 0, {}), 1);
    }
    {   // the largest record (7-object Rx frame) fits a read response beside its 9-byte header
        CHECK_EQ(kMaxRecord, 41u);
        CHECK_EQ(kReadHeader + kMaxRecord <= 58, 1);
    }
    {   // 50 ms period and 100 mV / 20 mA thresholds
        SampleGate g;
        CHECK_EQ(g.due(0, 5000, 0), 1);          // first sample always recorded
        CHECK_EQ(g.due(60, 5050, 10), 0);        // inside both thresholds
        CHECK_EQ(g.due(70, 5200, 0), 1);         // 200 mV step
        CHECK_EQ(g.due(100, 5900, 0), 0);        // big step but only 30 ms since the last record
        CHECK_EQ(g.due(130, 5900, 0), 1);
        CHECK_EQ(g.due(200, 5900, 25), 1);       // 25 mA step
        CHECK_EQ(g.due(300, 5900, 25), 0);
    }
    {   // cursor_for_last: oldest whole record within the byte budget
        R r;
        for (int i = 1; i <= 5; i++) push_n(r, i, 4);   // 10 bytes each
        std::uint8_t out[64];
        const auto c = r.cursor_for_last(25);            // room for 2 whole records
        CHECK_EQ(c, 30u);
        CHECK_EQ(tags(out, r.read(c, out).bytes).size(), 2u);
        CHECK_EQ(r.cursor_for_last(1000), 0u);
        CHECK_EQ(r.cursor_for_last(5), r.head());        // not even one fits: empty
    }
    {   // captures: pre-trigger copy, post-trigger append, cooldown, slot rotation, full slot
        R r;
        Captures<2, 100> c;
        for (int i = 1; i <= 8; i++) push_n(r, i, 4);   // 80 bytes of history
        CHECK_EQ(c.start(Trigger::HardReset, 1000, r, 50), 1);
        CHECK_EQ(c.slot(0).len, 50);                    // 5 whole records
        CHECK_EQ(c.slot(0).buf[kHeader], 4);            // the oldest kept is record 4
        const std::uint8_t rec9[10] = {10, 0, 0, 0, 0, 0, 9, 9, 9, 9};
        c.feed(1100, rec9);
        CHECK_EQ(c.slot(0).len, 60);
        CHECK_EQ(c.start(Trigger::HardReset, 1500, r, 50), 0);   // same trigger inside the cooldown
        CHECK_EQ(c.start(Trigger::Detach, 1500, r, 50), 1);      // another kind goes to slot 1
        CHECK_EQ(c.slot(1).trigger == Trigger::Detach, 1);
        c.feed(1600, rec9);                                      // slot 0's 500 ms window is over
        CHECK_EQ(c.slot(0).len, 60);
        CHECK_EQ(c.slot(0).filling, 0);
        CHECK_EQ(c.slot(1).len, 60);
        CHECK_EQ(c.start(Trigger::HardReset, 2600, r, 50), 1);   // both slots used: the older (0) is replaced
        CHECK_EQ(c.slot(0).seq, 3u);
        for (int i = 0; i < 6; i++) c.feed(2700, rec9);          // 50 + 10 * 5 = 100 fills it, the 6th is refused
        CHECK_EQ(c.slot(0).len, 100);
        CHECK_EQ(c.slot(0).filling, 0);
        c.clear();
        CHECK_EQ(c.slot(0).seq, 0u); CHECK_EQ(c.slot(1).len, 0);
    }
    {   // restored captures: filled from flash, flagged stored, replaced like any other slot
        R r;
        Captures<2, 100> c;
        const std::uint8_t a[10] = {10, 0, 0, 0, 0, 0, 1, 1, 1, 1};
        std::uint8_t big[101] = {};
        CHECK_EQ(c.restore(Trigger::Contract, big, 0), 0);         // does not fit a slot
        CHECK_EQ(c.restore(Trigger::Contract, a, 0), 1);
        CHECK_EQ(c.restore(Trigger::Detach, a, 4), 1);
        CHECK_EQ(c.slot(0).stored, 1);
        CHECK_EQ(c.slot(0).filling, 0);
        CHECK_EQ(c.slot(0).len, 10);
        CHECK_EQ(c.slot(1).pre, 4);
        CHECK_EQ(c.slot(0).seq < c.slot(1).seq, 1);               // restore the oldest first: the newest has the top seq
        c.feed(5000, a);                                           // stored slots are never appended to
        CHECK_EQ(c.slot(0).len, 10);
        for (int i = 1; i <= 3; i++) push_n(r, i, 4);
        CHECK_EQ(c.start(Trigger::HardReset, 9000, r, 50), 1);     // the older stored slot is replaced
        CHECK_EQ(c.slot(0).trigger == Trigger::HardReset, 1);
        CHECK_EQ(c.slot(0).stored, 0);
        CHECK_EQ(c.slot(1).stored, 1);
    }
    {   // dip detector: steps of more than 300 mV against the reading ~20 ms earlier
        DipDetector d;
        CHECK_EQ(d.update(0, 9000), 0);
        CHECK_EQ(d.update(5, 8800), 0);
        CHECK_EQ(d.update(10, 8600), 1);                // 400 mV below the reference of 9000
        CHECK_EQ(d.update(30, 8600), 0);                // reference moved: 8600 -> 8600 is stable
        CHECK_EQ(d.update(40, 12000), 0);               // a rise is no dip
    }
    {   // Collapse: the Baseus 70 W flood (3 frames 2.2 ms apart, again every 195 ms, a new MessageID and CRC each time, with a
        // Poll, an Irq and two Marks per frame) becomes the first frame in full, then one RxRepeat record
        using C = Collapse<2048>;
        struct Rig {
            Ring<2048> ring;
            C col;
            void put(Type t, std::uint32_t ts, std::vector<std::uint8_t> p) {
                col.record(ring, t, ts, p, [this](Type ty, std::uint32_t tt, std::span<const std::uint8_t> pl) { return ring.push(ty, tt, pl); });
            }
            void flush() { col.flush([this](Type ty, std::uint32_t tt, std::span<const std::uint8_t> pl) { return ring.push(ty, tt, pl); }); }
            std::vector<std::vector<std::uint8_t>> recs() {   // whole records as written
                std::vector<std::uint8_t> out(2048);
                const auto rd = ring.read(ring.tail(), out);
                std::vector<std::vector<std::uint8_t>> v;
                for (std::size_t i = 0; i < rd.bytes; i += out[i]) v.emplace_back(out.begin() + i, out.begin() + i + out[i]);
                return v;
            }
        };
        const auto caps = [](unsigned id, std::uint32_t crc, std::uint32_t pdo1 = 0x0801912C) {   // SOP, Source_Capabilities, 2 objects
            std::vector<std::uint8_t> p = {0xE0, std::uint8_t(0x01 | (2u << 6)), std::uint8_t(0x20 | (id << 1))};
            for (std::uint32_t o : {pdo1, std::uint32_t(0x0002D12C)}) for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(o >> (8 * i)));
            for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(crc >> (8 * i)));
            return p;
        };
        const std::vector<std::uint8_t> irq = {0x00, 0x00, 0x78}, ia = {0x04, 0x00, 0x00};
        const std::vector<std::uint8_t> mark_caps = {kMarkCaps, 0, 0, 0, 0}, mark_req = {kMarkAutoReq, 0, 1, 0, 0};
        const std::vector<std::uint8_t> poll = {0x40, 0x05, 0, 0};
        const auto cluster = [&](Rig& g, std::uint32_t ts, unsigned id, std::uint32_t crc) {
            g.put(Type::Poll, ts, poll);
            g.put(Type::Rx, ts + 1500, caps(id, crc));
            g.put(Type::Irq, ts + 2000, irq);
            g.put(Type::Mark, ts + 2010, mark_caps);
            g.put(Type::Mark, ts + 2020, mark_req);
        };
        Rig g;
        cluster(g, 1000, 3, 0xA62CCEBD);
        CHECK_EQ(g.recs().size(), 5u);                             // the first frame and its notes are recorded in full
        cluster(g, 3500, 3, 0xA62CCEBD);                           // the retries (same id, same CRC)
        cluster(g, 5700, 3, 0xA62CCEBD);
        cluster(g, 195000, 4, 0xB7D31EFA);                         // the periodic re-send: new id, new CRC
        cluster(g, 197200, 4, 0xB7D31EFA);
        CHECK_EQ(g.recs().size(), 5u);                             // nothing more written while the run is open
        CHECK_EQ(g.col.run(), 4);
        g.flush();                                                 // a read, a freeze or a capture start
        const auto v = g.recs();
        CHECK_EQ(v.size(), 6u);
        CHECK_EQ(v[5][1], int(Type::RxRepeat));
        CHECK_EQ(v[5][6] | v[5][7] << 8, 4);                       // count
        CHECK_EQ(v[5][8] | v[5][9] << 8, 193);                     // span from the first repeat (5000 us) to the last (198700 us), in ms
        const std::uint32_t ts = v[5][2] | v[5][3] << 8 | v[5][4] << 16 | std::uint32_t(v[5][5]) << 24;
        CHECK_EQ(ts, 5000u);                                       // stamped with the first repeat
        g.flush();
        CHECK_EQ(g.recs().size(), 6u);                             // flushing twice writes nothing more
    }
    {   // Collapse: anything that is not a repeat ends the run and the next frame is recorded in full
        using C = Collapse<2048>;
        struct Rig {
            Ring<2048> ring;
            C col;
            void put(Type t, std::uint32_t ts, std::vector<std::uint8_t> p) {
                col.record(ring, t, ts, p, [this](Type ty, std::uint32_t tt, std::span<const std::uint8_t> pl) { return ring.push(ty, tt, pl); });
            }
            std::vector<int> types() {
                std::vector<std::uint8_t> out(2048);
                const auto rd = ring.read(ring.tail(), out);
                std::vector<int> v;
                for (std::size_t i = 0; i < rd.bytes; i += out[i]) v.push_back(out[i + 1]);
                return v;
            }
        };
        const auto caps = [](std::uint32_t pdo1) {
            std::vector<std::uint8_t> p = {0xE0, 0x41, 0x10};
            for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(pdo1 >> (8 * i)));
            p.insert(p.end(), 4, 0x55);
            return p;
        };
        const std::vector<std::uint8_t> one = {0x01, 0x02, 0x03, 0x04};
        const auto T = [](Type t) { return int(t); };
        {   // different objects: a changed list is a new frame
            Rig g;
            g.put(Type::Rx, 10, caps(0x0801912C));
            g.put(Type::Rx, 20, caps(0x0801912C));
            g.put(Type::Rx, 30, caps(0x0002D12C));
            CHECK_EQ(g.types().size(), 3u);                        // Rx, RxRepeat, Rx
            CHECK_EQ(g.types()[1], T(Type::RxRepeat));
        }
        {   // an Irq carrying INTERRUPTB (GoodCRC sent), a Tx or a Reset between two identical frames: the second is in full
            for (Type other : {Type::Tx, Type::Reset, Type::Vbus, Type::Sink, Type::Cc}) {
                Rig g;
                g.put(Type::Rx, 10, caps(0x0801912C));
                g.put(Type::Rx, 20, caps(0x0801912C));
                g.put(other, 30, one);
                g.put(Type::Rx, 40, caps(0x0801912C));
                CHECK_EQ(g.types().size(), 4u);                    // Rx, RxRepeat, other, Rx
                CHECK_EQ(g.types()[3], T(Type::Rx));
            }
            Rig g;
            g.put(Type::Rx, 10, caps(0x0801912C));
            g.put(Type::Irq, 15, {0x00, 0x01, 0x00});             // INTERRUPTB
            g.put(Type::Rx, 20, caps(0x0801912C));
            CHECK_EQ(g.types().size(), 3u);
            CHECK_EQ(g.types()[2], T(Type::Rx));
        }
        {   // a different Mark or Irq inside a run ends it
            Rig g;
            g.put(Type::Rx, 10, caps(0x0801912C));
            g.put(Type::Mark, 11, {kMarkAutoReq, 0x00, 0x01, 0, 0});     // not queued
            g.put(Type::Rx, 20, caps(0x0801912C));
            g.put(Type::Mark, 21, {kMarkAutoReq, 0x00, 0x01, 0, 0});     // the same again: dropped
            g.put(Type::Mark, 22, {kMarkAutoReq, 0x00, 0x00, 0, 0});     // Request queued: kept, the run is written first
            CHECK_EQ(g.types().size(), 4u);                        // Rx, Mark, RxRepeat, Mark
            CHECK_EQ(g.types()[2], T(Type::RxRepeat));
            CHECK_EQ(g.types()[3], T(Type::Mark));
        }
        {   // a non-caps message is never collapsed, a Poll before one stays
            Rig g;
            std::vector<std::uint8_t> goodcrc = {0xE0, 0x41, 0x00, 0, 0, 0, 0};
            g.put(Type::Poll, 5, {1, 0, 0, 0});
            g.put(Type::Rx, 10, goodcrc);
            g.put(Type::Poll, 15, {1, 0, 0, 0});
            g.put(Type::Rx, 20, goodcrc);
            CHECK_EQ(g.types().size(), 4u);
        }
        {   // a frozen ring takes nothing and the collapser never holds records back
            Rig g;
            g.put(Type::Rx, 10, caps(0x0801912C));
            g.ring.freeze(true);
            g.put(Type::Rx, 20, caps(0x0801912C));
            g.put(Type::Poll, 25, {1, 0, 0, 0});
            CHECK_EQ(g.types().size(), 1u);
            CHECK_EQ(g.ring.missed(), 2u);
            CHECK_EQ(g.col.run(), 0);
        }
        {   // the anchor leaves the ring: the frame is written in full again
            using C2 = Collapse<600>;
            Ring<600> ring;
            C2 col;
            const auto w = [&](Type ty, std::uint32_t tt, std::span<const std::uint8_t> pl) { return ring.push(ty, tt, pl); };
            col.record(ring, Type::Rx, 1, caps(0x0801912C), w);
            for (int i = 0; i < 30; i++) col.record(ring, Type::Vbus, 2 + i, std::vector<std::uint8_t>(20, 7), w);   // pushes the Rx out
            col.record(ring, Type::Rx, 99, caps(0x0801912C), w);
            std::vector<std::uint8_t> out(600);
            const auto rd = ring.read(ring.tail(), out);
            std::size_t last_type = 0;
            for (std::size_t i = 0; i < rd.bytes; i += out[i]) last_type = out[i + 1];
            CHECK_EQ(last_type, std::size_t(Type::Rx));
        }
    }
    return test_report("pdtrace");
}
