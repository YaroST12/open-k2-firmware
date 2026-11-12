// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD black box push mode over the HID port (k2/core/trace_hid.hpp): the 64 byte report layout, whole-record
// packing, the explicit lost-records report on a ring wrap, the report sequence number, the keepalive timeout
// and the stop. The reader here is written independently of the encoder (own report parser, own record walk)
// so a shared mistake cannot hide.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "k2/pd/pdtrace.hpp"
#include "k2/core/trace_hid.hpp"
#include "test_util.h"

using namespace k2::pdtrace;
namespace th = k2::trace_hid;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Report = std::array<std::uint8_t, 64>;

struct Parsed {
    bool ok = false;
    std::uint8_t kind = 0, seq = 0;
    Bytes payload;
};

// One report: A7 kind seq len payload(len), zero padding behind it. `ok` is false for a broken frame.
Parsed parse(const Report& r) {
    Parsed p;
    if (r[0] != 0xA7) return p;
    const std::size_t n = r[3];
    if (4 + n > 64) return p;
    for (std::size_t i = 4 + n; i < 64; i++) if (r[i] != 0) return p;   // the pad is zero
    p.ok = true;
    p.kind = r[1];
    p.seq = r[2];
    p.payload.assign(r.begin() + 4, r.begin() + 4 + n);
    return p;
}

std::uint32_t u32(const Bytes& b, std::size_t at) {
    return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | std::uint32_t(b[at + 3]) << 24;
}

using R = Ring<600>;

// A record of `size` bytes whose first payload byte is `tag` (the record's number).
void push_rec(R& r, std::uint8_t tag, std::size_t size = 10) {
    Bytes p(size - 6, tag);
    r.push(Type::Mark, tag, p);
}

// Reports until the session has nothing more to say, at time `now`.
std::vector<Parsed> drain(th::Session& s, const R& r, std::uint32_t now, int* bad = nullptr) {
    std::vector<Parsed> out;
    Report f{};
    int guard = 0;
    while (s.next(r, now, f) && guard++ < 10000) {
        const Parsed p = parse(f);
        if (!p.ok && bad) (*bad)++;
        out.push_back(p);
    }
    return out;
}

// Record tags (first payload byte) in the Data reports.
std::vector<int> data_tags(const std::vector<Parsed>& v) {
    std::vector<int> t;
    for (const auto& p : v) {
        if (p.kind != 2) continue;
        for (std::size_t i = 4; i < p.payload.size(); i += p.payload[i]) t.push_back(p.payload[i + 6]);
    }
    return t;
}

}  // namespace

int main() {
    {   // geometry: 56 ring bytes fit a Data report, so one largest record always fits
        CHECK_EQ(th::kReportBytes, 64u);
        CHECK_EQ(th::kPayloadMax, 60u);
        CHECK_EQ(th::kChunk, 56u);
        CHECK_EQ(th::kChunk >= kMaxRecord, true);
    }
    {   // idle: nothing is sent until start()
        R r;
        push_rec(r, 1);
        th::Session s;
        CHECK_EQ(s.active(), false);
        Report f{};
        CHECK_EQ(s.next(r, 0, f), false);
    }
    {   // round trip from the oldest: Start, then whole records in as few reports as possible, then silence
        R r;
        for (std::uint8_t i = 1; i <= 8; i++) push_rec(r, i);   // 8 x 10 B = 80 B
        th::Session s;
        s.start(r, true, 100, 3000);
        CHECK_EQ(s.active(), true);
        int bad = 0;
        const auto v = drain(s, r, 100, &bad);
        CHECK_EQ(bad, 0);
        CHECK_EQ(v.size(), 3u);                 // Start, Data (5 records = 50 B), Data (3 records = 30 B)
        CHECK_EQ(v[0].kind, 1);
        CHECK_EQ(v[0].payload.size(), 13u);
        CHECK_EQ(v[0].payload[0], 1);           // from the oldest
        CHECK_EQ(u32(v[0].payload, 1), 0u);     // cursor
        CHECK_EQ(u32(v[0].payload, 5), 80u);    // head
        CHECK_EQ(u32(v[0].payload, 9), 0u);     // tail
        CHECK_EQ(v[1].kind, 2);
        CHECK_EQ(u32(v[1].payload, 0), 0u);
        CHECK_EQ(v[1].payload.size(), 4u + 50u);
        CHECK_EQ(v[2].kind, 2);
        CHECK_EQ(u32(v[2].payload, 0), 50u);    // chains onto the first Data report
        CHECK_EQ(v[2].payload.size(), 4u + 30u);
        const auto tags = data_tags(v);
        CHECK_EQ(tags.size(), 8u);
        for (std::size_t i = 0; i < tags.size(); i++) CHECK_EQ(tags[i], int(i + 1));
        CHECK_EQ(v[0].seq, 0);                  // one counter over every report
        CHECK_EQ(v[1].seq, 1);
        CHECK_EQ(v[2].seq, 2);
        std::uint8_t ring_bytes[128];
        const auto rd = r.read(0, ring_bytes);
        CHECK_EQ(rd.bytes, 80u);
        CHECK_EQ(memcmp(&v[1].payload[4], ring_bytes, 50), 0);       // byte for byte what the ring holds
        CHECK_EQ(memcmp(&v[2].payload[4], ring_bytes + 50, 30), 0);
        CHECK_EQ(drain(s, r, 100).size(), 0u);  // nothing new, nothing sent
        push_rec(r, 9);
        const auto more = drain(s, r, 100);
        CHECK_EQ(more.size(), 1u);
        CHECK_EQ(more[0].seq, 3);
        CHECK_EQ(data_tags(more)[0], 9);
        CHECK_EQ(s.stats().records, 9u);
        CHECK_EQ(s.stats().lost, 0u);
        CHECK_EQ(s.stats().reports, 4u);
    }
    {   // default start is the current head: old records are not sent
        R r;
        for (std::uint8_t i = 1; i <= 5; i++) push_rec(r, i);
        th::Session s;
        s.start(r, false, 0, 3000);
        auto first = drain(s, r, 0);
        CHECK_EQ(first.size(), 1u);
        CHECK_EQ(first[0].kind, 1);
        CHECK_EQ(first[0].payload[0], 0);
        push_rec(r, 6);
        const auto tags = data_tags(drain(s, r, 0));
        CHECK_EQ(tags.size(), 1u);
        CHECK_EQ(tags[0], 6);
    }
    {   // largest records: one per report (two do not fit 56 bytes), never split
        R r;
        for (std::uint8_t i = 1; i <= 4; i++) push_rec(r, i, kMaxRecord);
        th::Session s;
        s.start(r, true, 0, 3000);
        const auto v = drain(s, r, 0);
        CHECK_EQ(v.size(), 1u + 4u);
        for (std::size_t i = 1; i < v.size(); i++) CHECK_EQ(v[i].payload.size(), 4u + kMaxRecord);
        const auto tags = data_tags(v);
        CHECK_EQ(tags.size(), 4u);
    }
    {   // records of 28 B: two per report (56 B exactly)
        R r;
        for (std::uint8_t i = 1; i <= 4; i++) push_rec(r, i, 28);
        th::Session s;
        s.start(r, true, 0, 3000);
        const auto v = drain(s, r, 0);
        CHECK_EQ(v.size(), 1u + 2u);
        CHECK_EQ(v[1].payload.size(), 4u + 56u);
    }
    {   // a ring that wrapped past the cursor: one explicit Lost report with the exact counts, then the survivors
        R r;
        th::Session s;
        for (std::uint8_t i = 1; i <= 3; i++) push_rec(r, i);
        s.start(r, true, 0, 3000);
        const auto first = drain(s, r, 0);
        CHECK_EQ(first.size(), 2u);                       // Start + 3 records
        for (std::uint8_t i = 4; i <= 90; i++) push_rec(r, i);   // 870 B into 600: wraps, never read
        const auto v = drain(s, r, 0);
        CHECK_EQ(v[0].kind, 3);                           // Lost first
        CHECK_EQ(v[0].payload.size(), 9u);
        const std::uint32_t lost = u32(v[0].payload, 0);
        CHECK_EQ(lost > 0, true);
        CHECK_EQ(u32(v[0].payload, 4) % 10, 0u);          // whole 10 B records
        CHECK_EQ(u32(v[0].payload, 4), lost * 10);
        CHECK_EQ(v[0].payload[8], 0);
        // every record is either sent or counted lost: 90 pushed, 3 sent before the wrap
        std::size_t got = data_tags(v).size();
        CHECK_EQ(3u + lost + got, 90u);
        CHECK_EQ(s.stats().lost, lost);
        CHECK_EQ(s.stats().records, 3u + got);
        // the survivors are the newest ones, in order
        const auto tags = data_tags(v);
        CHECK_EQ(tags.back(), 90);
        for (std::size_t i = 1; i < tags.size(); i++) CHECK_EQ(tags[i], tags[i - 1] + 1);
    }
    {   // the report sequence number wraps at 256 and counts every report, Data or not
        Ring<8192> big;
        th::Session s;
        s.start(big, false, 0, 3000);
        Report f{};
        CHECK_EQ(s.next(big, 0, f), true);                 // Start, seq 0
        for (int i = 0; i < 300; i++) {
            Bytes p(35, std::uint8_t(i));
            big.push(Type::Rx, i, p);                      // one 41 B record per report
            CHECK_EQ(s.next(big, 0, f), true);
            CHECK_EQ(parse(f).seq, std::uint8_t((i + 1) & 0xFF));
        }
        CHECK_EQ(s.next(big, 0, f), false);
    }
    {   // keepalive timeout: after the silence an End report (reason timeout) is sent once, then the session is off
        R r;
        push_rec(r, 1);
        th::Session s;
        s.start(r, true, 1000, 500);
        CHECK_EQ(drain(s, r, 1100).size(), 2u);            // Start + Data
        CHECK_EQ(s.active(), true);
        s.keepalive(1400);                                 // the deadline moves to 1900
        CHECK_EQ(drain(s, r, 1800).size(), 0u);
        CHECK_EQ(s.active(), true);
        const auto end = drain(s, r, 1901);
        CHECK_EQ(end.size(), 1u);
        CHECK_EQ(end[0].kind, th::kKindEnd);
        CHECK_EQ(end[0].payload.size(), 1u);
        CHECK_EQ(end[0].payload[0], th::kEndTimeout);
        CHECK_EQ(s.active(), false);
        push_rec(r, 2);
        CHECK_EQ(drain(s, r, 2000).size(), 0u);            // off: records are not pushed
    }
    {   // expired() lets the firmware end a session whose host stopped reading (no room for the End report)
        R r;
        th::Session s;
        CHECK_EQ(s.expired(0), false);                      // off is not expired
        s.start(r, true, 100, 500);
        CHECK_EQ(s.expired(600), false);
        CHECK_EQ(s.expired(601), true);
        s.keepalive(500);
        CHECK_EQ(s.expired(601), false);
    }
    {   // the deadline survives the 32 bit millisecond wrap
        R r;
        th::Session s;
        s.start(r, false, 0xFFFFFF00u, 1000);
        s.keepalive(0xFFFFFF80u);
        drain(s, r, 0x00000100u);                          // 0x180 ms after the keepalive: alive
        CHECK_EQ(s.active(), true);
        drain(s, r, 0xFFFFFF80u + 1001u);
        CHECK_EQ(s.active(), false);
    }
    {   // stop ends the session at once and silently; a restart begins a fresh stream
        R r;
        push_rec(r, 1);
        th::Session s;
        s.start(r, true, 0, 3000);
        s.stop();
        CHECK_EQ(s.active(), false);
        CHECK_EQ(drain(s, r, 0).size(), 0u);
        s.start(r, true, 0, 3000);
        const auto v = drain(s, r, 0);
        CHECK_EQ(v.size(), 2u);
        CHECK_EQ(v[0].seq, 0);                             // the counter restarts with the session
    }
    {   // the timeout is clamped to the supported span
        CHECK_EQ(th::clamp_timeout_ms(0), th::kTimeoutDefaultMs);
        CHECK_EQ(th::clamp_timeout_ms(10), th::kTimeoutMinMs);
        CHECK_EQ(th::clamp_timeout_ms(5000), 5000u);
        CHECK_EQ(th::clamp_timeout_ms(65535), th::kTimeoutMaxMs);
    }
    return test_report("test_trace_hid");
}
