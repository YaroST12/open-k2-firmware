// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The multiplexed debug stream (k2/core/dbg_stream.hpp): framing round trip per kind, the boot header, change-only and windowed
// power samples, priorities and lost accounting when the FIFO saturates, and the per-pass cost bound. The reader is written
// independently of the encoder (own CRC, own parser, own payload offsets) so a shared mistake cannot hide.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "k2/core/binlog.hpp"
#include "k2/core/dbg_stream.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/chargertest/run_events.hpp"
#include "test_util.h"

using namespace k2;
using Bytes = std::vector<std::uint8_t>;

namespace {

std::uint16_t crc_ref(const std::uint8_t* p, std::size_t n) {
    std::uint16_t c = 0xFFFF;
    for (std::size_t i = 0; i < n; i++) {
        c ^= std::uint16_t(p[i] << 8);
        for (int b = 0; b < 8; b++) c = std::uint16_t(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

struct Frame { std::uint8_t kind; Bytes pl; };

std::vector<Frame> parse(const Bytes& s, int* bad = nullptr) {
    std::vector<Frame> out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != 0xA6) { i++; continue; }
        if (i + 3 > s.size()) break;
        const std::size_t n = s[i + 2];
        if (i + 3 + n + 2 > s.size()) break;
        const std::uint16_t want = std::uint16_t(s[i + 3 + n] | s[i + 4 + n] << 8);
        if (crc_ref(&s[i + 1], 2 + n) == want) {
            out.push_back({s[i + 1], Bytes(s.begin() + i + 3, s.begin() + i + 3 + n)});
            i += 5 + n;
        } else {
            if (bad) (*bad)++;
            i++;
        }
    }
    return out;
}

std::uint32_t u32(const Bytes& b, std::size_t at) {
    return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | std::uint32_t(b[at + 3]) << 24;
}
std::uint16_t u16(const Bytes& b, std::size_t at) { return std::uint16_t(b[at] | b[at + 1] << 8); }

enum : std::uint8_t { Start = 1, Data = 2, Lost = 3, Hello = 4, Text = 5, Log = 6, Power = 7, Run = 8, Gap = 9, Beat = 10 };

struct Fake final : dbg::Probe {
    std::uint32_t now = 1000;
    dbg::PowerSample ps;
    bool have_power = true;
    int power_calls = 0;
    std::string text = "K2B1 boot header text, long enough to need three chunks of up to fifty-six bytes each, so 0123456789";
    std::uint32_t now_ms() override { return now; }
    bool power(dbg::PowerSample& s) override { power_calls++; s = ps; return have_power; }
    void beat(dbg::BeatInfo& b) override { b.stack_free = 4000; b.msp_free = 3000; b.page = 2; b.usb_dropped = 7; }
    std::size_t boot_len() override { return text.size(); }
    void boot_read(std::size_t off, std::span<std::uint8_t> out) override {
        for (std::size_t i = 0; i < out.size(); i++) out[i] = std::uint8_t(text[off + i]);
    }
    std::string_view hash() override { return "abc1234-dirty"; }
};

using TraceRing = pdtrace::Ring<600>;
using LogRing = BinLog<128>;
using RunQ = runev::Queue<runev::kQueueEvents>;

struct Rig {
    TraceRing tr;
    LogRing lg;
    RunQ rq;
    Fake fake;
    dbg::Mux mux;
    Bytes wire;

    void start(std::uint8_t mask, std::uint16_t hz = 20, bool history = false) {
        mux.start(mask, hz, history, tr, lg, rq, fake.now);
        wire.clear();
    }
    // One main-loop pass (1 ms later) with `room` free FIFO bytes; the frames sent.
    unsigned pass(std::size_t room = 100000) {
        fake.now++;
        return mux.service(tr, lg, rq, fake, room, [&](std::span<const std::uint8_t> f) { wire.insert(wire.end(), f.begin(), f.end()); });
    }
    // Passes with a large FIFO until nothing is left to say (power and Beat keepalives take the clock past 1 s, so the
    // loop stops on a quiet pass).
    void drain(unsigned max = 200) {
        for (unsigned i = 0; i < max; i++)
            if (pass() == 0) return;
    }
    std::vector<Frame> frames(int* bad = nullptr) const { return parse(wire, bad); }
    std::vector<Frame> of(std::uint8_t kind) const {
        std::vector<Frame> v;
        for (auto& f : frames())
            if (f.kind == kind) v.push_back(f);
        return v;
    }
};

void log_push(LogRing& l, std::uint32_t ms, std::uint16_t id, std::uint32_t sig, ...) {
    std::va_list ap;
    va_start(ap, sig);
    l.push(ms, id, sig, ap);
    va_end(ap);
}

void push_rec(TraceRing& r, std::uint8_t tag, std::size_t size = 10) {
    Bytes p(size - pdtrace::kHeader, tag);
    r.push(pdtrace::Type::Mark, tag, p);
}

// ---- Hello, boot text, Beat ----

void test_hello_then_boot_text_then_beat() {
    Rig r;
    r.start(dbg::kAll, 50);
    r.drain();
    const auto fr = r.frames();
    CHECK_EQ(fr.size() >= 5u, true);
    CHECK_EQ(fr[0].kind, Hello);
    CHECK_EQ(fr[0].pl[0], dbg::kVersion);
    CHECK_EQ(fr[0].pl[1], dbg::kAll);
    CHECK_EQ(u16(fr[0].pl, 2), 50u);
    CHECK_EQ(u32(fr[0].pl, 4), 1001u);                                   // the K2 clock at the first pass
    CHECK_EQ(fr[0].pl[8], 0u);
    CHECK_STR(std::string(fr[0].pl.begin() + 9, fr[0].pl.end()).c_str(), "abc1234-dirty");
    std::string text;
    std::size_t i = 1;
    for (; i < fr.size() && fr[i].kind == Text; i++) {
        CHECK_EQ(fr[i].pl[4], 1u);                                       // tag: boot info
        text.append(fr[i].pl.begin() + 5, fr[i].pl.end());
    }
    CHECK_STR(text.c_str(), r.fake.text.c_str());
    CHECK_EQ(i >= 3u, true);                                             // more than one chunk
    bool beat = false;
    for (; i < fr.size(); i++) {
        CHECK_EQ(fr[i].kind == Hello || fr[i].kind == Text, false);      // nothing but data kinds after the header
        if (fr[i].kind == Beat) {
            beat = true;
            CHECK_EQ(fr[i].pl.size(), 50u);
            CHECK_EQ(u16(fr[i].pl, 4), 4000u);
            CHECK_EQ(u16(fr[i].pl, 6), 3000u);
            CHECK_EQ(fr[i].pl[8], 2u);
            CHECK_EQ(u32(fr[i].pl, 10), 7u);
        }
    }
    CHECK_EQ(beat, true);
}

void test_header_goes_out_before_everything_even_in_a_tiny_fifo() {
    Rig r;
    r.rq.push({5, runev::Code::Stage, 1, 0, 0, 0});
    r.start(dbg::kAll, 20, true);
    for (int i = 0; i < 40; i++) r.pass(dbg::kFrameMax);                 // room for exactly one worst-case frame per pass
    const auto fr = r.frames();
    CHECK_EQ(fr[0].kind, Hello);
    std::size_t k = 1;
    while (k < fr.size() && fr[k].kind == Text) k++;
    for (std::size_t j = k; j < fr.size(); j++) CHECK_EQ(fr[j].kind == Text, false);
    CHECK_EQ(r.of(Run).size(), 1u);
}

void test_mask_selects_kinds() {
    Rig r;
    r.rq.push({5, runev::Code::Stage, 1, 0, 0, 0});
    push_rec(r.tr, 1);
    log_push(r.lg, 10, 3, 0);
    r.start(dbg::kRun, 20, true);
    r.drain();
    CHECK_EQ(r.of(Run).size(), 1u);
    CHECK_EQ(r.of(Data).size() + r.of(Start).size() + r.of(Log).size() + r.of(Power).size(), 0u);
    r.start(dbg::kLog | dbg::kTrace, 20, true);
    r.drain();
    CHECK_EQ(r.of(Run).size() + r.of(Power).size(), 0u);
    CHECK_EQ(r.of(Log).size(), 1u);
    CHECK_EQ(r.of(Data).size(), 1u);
    CHECK_EQ(u16(r.of(Hello)[0].pl, 2), 0u);                             // no power kind: no rate announced
}

// ---- Run events ----

void test_run_event_round_trip_and_gap() {
    Rig r;
    r.start(dbg::kRun);
    r.rq.push({1234, runev::Code::RowEnd, 7, 3, 9, 5000});
    r.rq.push({1300, runev::Code::Attach, 1, 0, 0, 0});
    r.drain();
    auto run = r.of(Run);
    CHECK_EQ(run.size(), 2u);
    CHECK_EQ(run[0].pl.size(), 12u);
    CHECK_EQ(u32(run[0].pl, 0), 1234u);
    CHECK_EQ(run[0].pl[4], unsigned(runev::Code::RowEnd));
    CHECK_EQ(run[0].pl[5], 7u);
    CHECK_EQ(u16(run[0].pl, 6), 3u);
    CHECK_EQ(u16(run[0].pl, 8), 9u);
    CHECK_EQ(u16(run[0].pl, 10), 5000u);
    CHECK_EQ(run[1].pl[4], unsigned(runev::Code::Attach));
    // 20 events while the host cannot take anything: the queue holds kQueueEvents, the rest are reported, in order
    constexpr unsigned kQ = runev::kQueueEvents, kLost = 20 - kQ;
    r.wire.clear();
    for (std::uint16_t i = 0; i < 20; i++) r.rq.push({2000u + i, runev::Code::Stage, std::uint8_t(i), 0, 0, 0});
    r.drain();
    const auto fr = r.frames();
    CHECK_EQ(fr[0].kind, Gap);
    CHECK_EQ(fr[0].pl[0], 3u);                                           // the run source
    CHECK_EQ(u32(fr[0].pl, 1), kLost);
    CHECK_EQ(u32(fr[0].pl, 5), kLost * 12u);
    CHECK_EQ(r.of(Run).size(), std::size_t(kQ));
    CHECK_EQ(r.of(Run)[0].pl[5], kLost);                                 // the first events were overwritten
    CHECK_EQ(r.mux.stats().run_lost, kLost);
    CHECK_EQ(r.mux.stats().run_events, 2u + kQ);
}

// A failure skips every row behind it in one pass (55 rows on a full run): the rows with one reason are ONE event, so the burst
// never overflows the ring and the reader sees "rows a..b skipped, reason X" instead of a Gap.
void test_skip_burst_is_one_event_per_reason() {
    Rig r;
    runev::SkipRun sk;
    r.start(dbg::kRun, 20, false);
    CHECK_EQ(sk.add(r.rq, 500, 10, 7), true);    // opens a range
    for (std::uint16_t row = 11; row < 65; row++) CHECK_EQ(sk.add(r.rq, 500 + row, row, 7), false);
    CHECK_EQ(sk.open(), true);
    CHECK_EQ(unsigned(r.rq.pushed()), 0u);                               // nothing goes out before the range is closed
    CHECK_EQ(sk.add(r.rq, 600, 65, 9), true);                                  // another reason: the first range closes into one event
    CHECK_EQ(unsigned(r.rq.pushed()), 1u);
    sk.flush(r.rq);
    CHECK_EQ(unsigned(r.rq.pushed()), 2u);
    sk.flush(r.rq);                                                      // nothing open any more: no empty event
    CHECK_EQ(unsigned(r.rq.pushed()), 2u);
    CHECK_EQ(sk.open(), false);
    r.drain();
    const auto run = r.of(Run);
    CHECK_EQ(run.size(), 2u);
    CHECK_EQ(r.mux.stats().run_lost, 0u);
    CHECK_EQ(run[0].pl[4], unsigned(runev::Code::RowSkips));
    CHECK_EQ(u32(run[0].pl, 0), 500u);                                   // stamped with the first skipped row
    CHECK_EQ(run[0].pl[5], 10u);                                         // first row
    CHECK_EQ(u16(run[0].pl, 6), 64u);                                    // last row
    CHECK_EQ(u16(run[0].pl, 8), 7u);                                     // reason
    CHECK_EQ(u16(run[0].pl, 10), 55u);                                   // rows
    CHECK_EQ(run[1].pl[5], 65u);
    CHECK_EQ(u16(run[1].pl, 6), 65u);
    CHECK_EQ(u16(run[1].pl, 10), 1u);
    // a row that does not follow the last one (another unit's rows) starts a new range even with the same reason
    sk.add(r.rq, 700, 70, 9);
    sk.add(r.rq, 701, 80, 9);
    sk.flush(r.rq);
    CHECK_EQ(unsigned(r.rq.pushed()), 4u);
    // the page's own queue: any other event closes the open range first, so the stream keeps the order the page saw
    const auto at = runev::live().pushed();
    runev::skip(900, 3, 5); runev::skip(901, 4, 5);
    CHECK_EQ(runev::live().pushed(), at);
    runev::emit(runev::Code::End, 950, 1, 2, 3);
    CHECK_EQ(runev::live().pushed(), at + 2);
    CHECK_EQ(int(runev::live().at(at).code), int(runev::Code::RowSkips));
    CHECK_EQ(unsigned(runev::live().at(at).d), 2u);
    CHECK_EQ(int(runev::live().at(at + 1).code), int(runev::Code::End));
    runev::flush();
    CHECK_EQ(runev::live().pushed(), at + 2);
}

void test_run_history_starts_at_the_oldest_event() {
    Rig r;
    for (std::uint16_t i = 0; i < 5; i++) r.rq.push({100u + i, runev::Code::Stage, std::uint8_t(i), 0, 0, 0});
    r.start(dbg::kRun, 20, false);
    r.drain();
    CHECK_EQ(r.of(Run).size(), 0u);                                      // from now: the past is not sent
    r.start(dbg::kRun, 20, true);
    r.drain();
    CHECK_EQ(r.of(Run).size(), 5u);
    CHECK_EQ(r.of(Hello)[0].pl[8], 1u);
}

// ---- Log ----

struct Entry { std::uint32_t ms, seq; unsigned id; };

// The entries of the Log frames, with their absolute times and numbers (a reader's walk of base_ms and dt).
std::vector<Entry> decode_log(const std::vector<Frame>& fr) {
    std::vector<Entry> out;
    for (const auto& f : fr) {
        if (f.kind != Log) continue;
        std::uint32_t ms = u32(f.pl, 0), seq = u32(f.pl, 4);
        for (std::size_t i = 8; i < f.pl.size(); i += 1 + f.pl[i]) {
            ms += LogRing::dt_of(&f.pl[i]);
            std::size_t id_at = i + 1;
            while (f.pl[id_at] & 0x80) id_at++;                          // skip the dt varint
            out.push_back({ms, seq++, f.pl[id_at + 1]});                 // ids below 128: one varint byte
        }
    }
    return out;
}

void test_log_round_trip_with_times_and_numbers() {
    Rig r;
    log_push(r.lg, 50, 1, 0);                                            // before the start: not sent
    r.start(dbg::kLog);
    log_push(r.lg, 80, 2, 0);
    log_push(r.lg, 95, 3, 0);
    log_push(r.lg, 400, 4, 0);
    r.drain();
    const auto e = decode_log(r.frames());
    CHECK_EQ(e.size(), 3u);
    CHECK_EQ(e[0].ms, 80u);
    CHECK_EQ(e[0].seq, 1u);                                              // entry 0 was the one before the start
    CHECK_EQ(e[0].id, 2u);
    CHECK_EQ(e[1].ms, 95u);
    CHECK_EQ(e[2].ms, 400u);
    CHECK_EQ(e[2].id, 4u);
    CHECK_EQ(r.mux.stats().log_entries, 3u);
    CHECK_EQ(r.mux.stats().log_lost, 0u);
}

void test_log_overrun_is_reported_exactly() {
    Rig r;
    r.start(dbg::kLog);
    // BinLog<128>: 3-byte entries, 42 fit. Push 100 while the host takes nothing.
    for (std::uint32_t i = 1; i <= 100; i++) log_push(r.lg, 10 * i, std::uint16_t(i % 100 + 1), 0);
    r.drain();
    const auto fr = r.frames();
    std::uint32_t lost = 0;
    for (auto& f : fr)
        if (f.kind == Gap) {
            CHECK_EQ(f.pl[0], 2u);
            lost += u32(f.pl, 1);
        }
    const auto e = decode_log(fr);
    CHECK_EQ(e.size() + lost, 100u);                                     // every entry is either seen or counted
    CHECK_EQ(lost, 100u - 42u);
    CHECK_EQ(e.front().seq, 58u);                                        // numbers stay absolute
    CHECK_EQ(e.front().ms, 10u * 59u);                                   // and so do the times, from the ring's base
    CHECK_EQ(e.back().ms, 1000u);
    CHECK_EQ(r.mux.stats().log_lost, lost);
}

void test_log_history_starts_at_the_oldest_kept_entry() {
    Rig r;
    for (std::uint32_t i = 1; i <= 100; i++) log_push(r.lg, 10 * i, std::uint16_t(i % 100 + 1), 0);
    r.start(dbg::kLog, 20, true);
    r.drain();
    const auto e = decode_log(r.frames());
    CHECK_EQ(e.size(), 42u);
    CHECK_EQ(e.front().ms, 590u);
    CHECK_EQ(r.mux.stats().log_lost, 0u);                                // history starts at the kept entries: nothing "lost"
}

// ---- Trace (the existing Streamer inside the multiplexer) ----

void test_trace_frames_pass_through_unchanged() {
    Rig r;
    r.start(dbg::kTrace);
    for (std::uint8_t i = 1; i <= 6; i++) push_rec(r.tr, i);
    r.drain();
    const auto fr = r.frames();
    std::size_t k = 0;
    while (k < fr.size() && fr[k].kind != Start) k++;
    CHECK_EQ(k < fr.size(), true);
    CHECK_EQ(fr[k].pl.size(), 13u);
    std::vector<int> tags;
    for (auto& f : fr)
        if (f.kind == Data)
            for (std::size_t i = 4; i < f.pl.size(); i += f.pl[i]) tags.push_back(f.pl[i + 2 + 4]);   // len type ts(4), then payload
    CHECK_EQ(tags.size(), 6u);
    CHECK_EQ(r.mux.stats().trace_records, 6u);
}

void test_trace_overrun_gives_a_lost_frame() {
    Rig r;
    r.start(dbg::kTrace);
    r.drain();                                                            // the Start frame
    r.wire.clear();
    for (std::uint8_t i = 0; i < 200; i++) push_rec(r.tr, i, 20);         // 4000 bytes into a 600 byte ring
    r.drain();
    const auto lost = r.of(Lost);
    CHECK_EQ(lost.size(), 1u);
    CHECK_EQ(u32(lost[0].pl, 0) + r.mux.stats().trace_records, 200u);
}

// ---- Power ----

dbg::PowerSample sample(std::uint16_t vbus, std::int16_t ibus, std::uint16_t cc1 = 3300) {
    dbg::PowerSample s;
    s.valid = true;
    s.vbus = vbus;
    s.ibus = ibus;
    s.cc1 = cc1;
    s.cc2 = 0;
    s.dp = 600;
    s.dm = 0;
    return s;
}

void test_power_first_sample_then_change_only_then_keepalive() {
    Rig r;
    r.fake.ps = sample(5000, 120);
    r.start(dbg::kPower, 100);
    for (int i = 0; i < 100; i++) r.pass();                              // 100 ms of an unchanged reading
    auto pw = r.of(Power);
    CHECK_EQ(pw.size(), 1u);                                             // the first one, then silence
    CHECK_EQ(pw[0].pl.size(), 19u);
    CHECK_EQ(u16(pw[0].pl, 4), 1u);
    CHECK_EQ(pw[0].pl[6], 1u);
    CHECK_EQ(u16(pw[0].pl, 7), 5000u);
    CHECK_EQ(u16(pw[0].pl, 9), 120u);
    CHECK_EQ(u16(pw[0].pl, 11), 3300u);
    CHECK_EQ(u16(pw[0].pl, 15), 600u);
    r.fake.ps = sample(5020, 125);                                       // inside the deadband
    for (int i = 0; i < 100; i++) r.pass();
    CHECK_EQ(r.of(Power).size(), 1u);
    r.fake.ps = sample(9000, 125);                                       // a real step
    r.pass(); r.pass(); r.pass(); r.pass(); r.pass(); r.pass(); r.pass(); r.pass(); r.pass(); r.pass(); r.pass();
    pw = r.of(Power);
    CHECK_EQ(pw.size(), 2u);
    CHECK_EQ(u16(pw[1].pl, 7), 9000u);
    for (int i = 0; i < 1100; i++) r.pass();                             // keepalive after a second of no change
    CHECK_EQ(r.of(Power).size(), 3u);
}

void test_power_rate_is_bounded_and_unavailable_is_marked() {
    Rig r;
    r.fake.ps = sample(5000, 0);
    r.start(dbg::kPower, 5000);                                          // clamped to the 100 Hz ceiling
    CHECK_EQ(r.mux.power_hz(), dbg::kPowerHzMax);
    for (int i = 0; i < 1000; i++) r.pass();
    CHECK_EQ(r.fake.power_calls <= 101, true);                            // one probe call per 10 ms
    r.start(dbg::kPower, 0);
    CHECK_EQ(r.mux.power_hz(), dbg::kPowerHzDefault);
    r.fake.ps.cc1 = r.fake.ps.cc2 = r.fake.ps.dp = r.fake.ps.dm = dbg::kUnread;
    r.fake.ps.vbus = 5100;                                               // the ADC pins belong to something else
    r.pass(); r.pass();
    const auto pw = r.of(Power);
    CHECK_EQ(pw.size(), 1u);
    CHECK_EQ(u16(pw[0].pl, 11), dbg::kUnread);
}

void test_power_folds_into_a_window_when_the_fifo_has_no_room() {
    Rig r;
    r.fake.ps = sample(5000, 100);
    r.start(dbg::kPower, 100);
    for (int i = 0; i < 3; i++) r.pass();                                // the first frame is out
    r.wire.clear();
    const auto before = r.mux.stats();
    for (int i = 0; i < 100; i++) {                                      // 100 ms with a full FIFO and a moving reading
        r.fake.ps = sample(std::uint16_t(5000 + 10 * i), std::int16_t(100 + 5 * i));
        r.pass(dbg::kFrameMax - 1);
    }
    CHECK_EQ(r.of(Power).size(), 0u);
    CHECK_EQ(r.mux.stats().power_folded > before.power_folded, true);
    CHECK_EQ(r.mux.stats().stalls > 0u, true);
    r.pass();                                                            // room again: one frame carries the window
    const auto pw = r.of(Power);
    CHECK_EQ(pw.size(), 1u);
    CHECK_EQ(pw[0].pl.size(), 29u);
    // the probe is read every 10 ms, so the window holds about ten samples of the rising reading
    const unsigned n = u16(pw[0].pl, 4);
    CHECK_EQ(n >= 9u && n <= 11u, true);
    CHECK_EQ(u16(pw[0].pl, 23), u16(pw[0].pl, 7));                       // vbus max is the newest value (it only rises)
    CHECK_EQ(u16(pw[0].pl, 27), u16(pw[0].pl, 9));                       // ibus max too
    CHECK_EQ(u16(pw[0].pl, 7) > u16(pw[0].pl, 21), true);                // vbus min below it
    CHECK_EQ(u16(pw[0].pl, 21) >= 5000u && u16(pw[0].pl, 21) <= 5100u, true);
    CHECK_EQ(u16(pw[0].pl, 25) >= 100u && u16(pw[0].pl, 25) <= 150u, true);
    CHECK_EQ(u16(pw[0].pl, 19) >= 80u && u16(pw[0].pl, 19) <= 100u, true);   // span_ms of the window
}

// ---- Saturation and priority ----

void test_saturation_keeps_run_events_and_trace_accounting_and_power_still_arrives() {
    Rig r;
    r.fake.ps = sample(5000, 0);
    r.start(dbg::kAll, 100);
    std::uint32_t run_pushed = 0, trace_pushed = 0;
    std::size_t fifo = 0;                                                // the CDC FIFO: 256 bytes, the host takes 16 per ms
    for (int ms = 0; ms < 3000; ms++) {
        r.fake.ps = sample(std::uint16_t(5000 + (ms / 20) * 100), 0);    // a step every 20 ms
        for (int k = 0; k < 2; k++) push_rec(r.tr, std::uint8_t(trace_pushed++), 30);   // 60 B/ms of trace: far above the link
        if (ms % 50 == 0) r.rq.push({r.fake.now, runev::Code::Stage, std::uint8_t(run_pushed++ & 0xFF), 0, 0, 0});
        log_push(r.lg, r.fake.now, std::uint16_t(1 + ms % 100), 0);
        fifo = fifo > 16 ? fifo - 16 : 0;
        const std::size_t before = r.wire.size();
        r.pass(256 - fifo);
        fifo += r.wire.size() - before;
        CHECK_EQ(fifo <= 256, true);
    }
    r.drain();                                                           // the host catches up: pending Lost/Gap reports go out
    int bad = 0;
    const auto fr = r.frames(&bad);
    CHECK_EQ(bad, 0);
    // run events: every one is seen or counted, none vanishes
    std::uint32_t run_seen = 0, run_gap = 0, trace_gap = 0, log_gap = 0;
    for (auto& f : fr) {
        if (f.kind == Run) run_seen++;
        if (f.kind == Gap && f.pl[0] == 3) run_gap += u32(f.pl, 1);
        if (f.kind == Gap && f.pl[0] == 2) log_gap += u32(f.pl, 1);
        if (f.kind == Lost) trace_gap += u32(f.pl, 0);
    }
    CHECK_EQ(run_pushed, 60u);
    CHECK_EQ(run_seen + run_gap, run_pushed);
    CHECK_EQ(run_gap, 0u);                                               // run events outrank trace: none were overwritten
    // trace: every record the ring overwrote was either sent first or reported lost, and none is counted twice
    const auto st = r.mux.stats();
    CHECK_EQ(r.tr.records_pushed(), trace_pushed);
    CHECK_EQ(st.trace_lost, trace_gap);
    CHECK_EQ(st.trace_lost > 0u, true);                                  // it really did saturate
    CHECK_EQ(st.trace_records + st.trace_lost <= trace_pushed, true);
    CHECK_EQ(st.trace_records + st.trace_lost >= r.tr.records_dropped(), true);
    CHECK_EQ(st.log_lost, log_gap);                                      // whatever the log lost was reported
    CHECK_EQ(st.log_entries > 500u, true);                               // and a flooding trace did not starve it
    // power: decimated, not starved: a frame at least every kPowerStaleMs plus the frame time
    std::vector<std::uint32_t> pms;
    for (auto& f : fr)
        if (f.kind == Power) pms.push_back(u32(f.pl, 0));
    CHECK_EQ(pms.size() > 3000u / 700u, true);
    for (std::size_t i = 1; i < pms.size(); i++) CHECK_EQ(pms[i] - pms[i - 1] <= dbg::kPowerStaleMs + 50, true);
    CHECK_EQ(st.power_folded > 0u, true);
}

void test_every_pass_is_bounded() {
    Rig r;
    r.fake.ps = sample(5000, 0);
    r.start(dbg::kAll, 100);
    for (int ms = 0; ms < 200; ms++) {
        r.fake.ps = sample(std::uint16_t(5000 + 50 * ms), std::int16_t(ms));
        for (int k = 0; k < 4; k++) push_rec(r.tr, std::uint8_t(k), 30);
        for (int k = 0; k < 4; k++) r.rq.push({r.fake.now, runev::Code::Stage, std::uint8_t(k), 0, 0, 0});
        for (int k = 0; k < 4; k++) log_push(r.lg, r.fake.now, std::uint16_t(k + 1), 0);
        const std::size_t before = r.wire.size();
        const int calls = r.fake.power_calls;
        const unsigned n = r.pass();
        CHECK_EQ(n <= dbg::kMaxFramesPerPass, true);
        CHECK_EQ(r.wire.size() - before <= dbg::kMaxFramesPerPass * dbg::kFrameMax, true);
        CHECK_EQ(r.fake.power_calls - calls <= 1, true);                 // at most one probe read per pass
    }
    // per kind: no source takes more than its share of a pass
    Rig q;
    for (std::uint32_t i = 0; i < 12; i++) q.rq.push({i, runev::Code::Stage, 0, 0, 0, 0});
    for (std::uint32_t i = 0; i < 40; i++) log_push(q.lg, i, 1, 0);
    for (std::uint8_t i = 0; i < 20; i++) push_rec(q.tr, i, 30);
    q.start(dbg::kAll, 20, true);
    q.drain(3);                                                          // header, then the first data passes
    for (int i = 0; i < 6; i++) q.pass();
    unsigned pass_run = 0, pass_log = 0, pass_data = 0;
    q.wire.clear();
    q.pass();
    for (auto& f : q.frames()) {
        pass_run += f.kind == Run;
        pass_log += f.kind == Log;
        pass_data += f.kind == Data;
    }
    CHECK_EQ(pass_run <= dbg::kKindFramesPerPass, true);
    CHECK_EQ(pass_log <= dbg::kKindFramesPerPass, true);
    CHECK_EQ(pass_data <= dbg::kKindFramesPerPass, true);

    // wall cost of a pass that has something of every kind to send (host -O0; the K2 is a few times slower per call but
    // does the same bounded work: at most kMaxFramesPerPass frames of at most kFrameMax bytes and one probe read)
    Rig c;
    c.fake.ps = sample(5000, 0);
    c.start(dbg::kAll, 100);
    c.drain(8);
    double total = 0, worst = 0;
    for (int ms = 0; ms < 2000; ms++) {
        c.fake.ps = sample(std::uint16_t(5000 + 50 * (ms % 40)), std::int16_t(ms % 7));
        for (int k = 0; k < 3; k++) push_rec(c.tr, std::uint8_t(k), 30);
        c.rq.push({c.fake.now, runev::Code::Stage, 0, 0, 0, 0});
        log_push(c.lg, c.fake.now, 5, 0);
        const auto t0 = std::chrono::steady_clock::now();
        c.pass();
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        total += us;
        if (us > worst) worst = us;
        c.wire.clear();
    }
    std::printf("dbg stream pass cost (host -O0): mean %.1f us, worst %.1f us, %u frames max per pass\n", total / 2000, worst,
                dbg::kMaxFramesPerPass);
    // The wall-clock bound is a timing check, not a functional one: on a loaded host a single preempted pass
    // blows any limit, which is what made `make gate` flaky (ROADMAP 2026-11-09, reproduced by
    // tools/flaky_repro.sh: worst 8426 us on a loaded host). The functional bound is the work counted above
    // (frames and bytes per pass, at most one probe read). The timing check runs only when explicitly asked
    // for, outside `make gate`: K2_TEST_TIMING=1 ./host-build/test_dbg_stream
    if (std::getenv("K2_TEST_TIMING"))
        CHECK_EQ(worst < 5000.0, true);
}

void test_nothing_is_sent_when_stopped_and_stats_survive_the_stop() {
    Rig r;
    r.start(dbg::kRun);
    r.rq.push({1, runev::Code::Stage, 0, 0, 0, 0});
    r.drain();
    CHECK_EQ(r.mux.stats().run_events, 1u);
    r.mux.stop();
    r.wire.clear();
    r.rq.push({2, runev::Code::Stage, 0, 0, 0, 0});
    CHECK_EQ(r.pass(), 0u);
    CHECK_EQ(r.wire.size(), 0u);
    CHECK_EQ(r.mux.stats().run_events, 1u);
    CHECK_EQ(r.mux.running(), false);
}

void test_frames_all_pass_their_crc_and_fit_the_budget() {
    Rig r;
    r.fake.ps = sample(5000, 0);
    r.rq.push({1, runev::Code::Stage, 0, 0, 0, 0});
    for (std::uint8_t i = 0; i < 9; i++) push_rec(r.tr, i, 40);
    for (std::uint32_t i = 1; i <= 60; i++) log_push(r.lg, 7 * i, std::uint16_t(i), 0);
    r.start(dbg::kAll, 100, true);
    for (int i = 0; i < 300; i++) {
        r.fake.ps = sample(std::uint16_t(5000 + i * 40), 0);
        r.pass();
    }
    int bad = 0;
    const auto fr = r.frames(&bad);
    CHECK_EQ(bad, 0);
    for (auto& f : fr) CHECK_EQ(3 + f.pl.size() + 2 <= dbg::kFrameMax, true);
    bool kinds[11] = {};
    for (auto& f : fr) kinds[f.kind] = true;
    for (int k = 1; k <= 10; k++)
        if (k != Lost && k != Gap) CHECK_EQ(kinds[k], true);              // every kind was produced (Lost/Gap only on loss)
}

}  // namespace

int main() {
    test_hello_then_boot_text_then_beat();
    test_header_goes_out_before_everything_even_in_a_tiny_fifo();
    test_mask_selects_kinds();
    test_run_event_round_trip_and_gap();
    test_skip_burst_is_one_event_per_reason();
    test_run_history_starts_at_the_oldest_event();
    test_log_round_trip_with_times_and_numbers();
    test_log_overrun_is_reported_exactly();
    test_log_history_starts_at_the_oldest_kept_entry();
    test_trace_frames_pass_through_unchanged();
    test_trace_overrun_gives_a_lost_frame();
    test_power_first_sample_then_change_only_then_keepalive();
    test_power_rate_is_bounded_and_unavailable_is_marked();
    test_power_folds_into_a_window_when_the_fifo_has_no_room();
    test_saturation_keeps_run_events_and_trace_accounting_and_power_still_arrives();
    test_every_pass_is_bounded();
    test_nothing_is_sent_when_stopped_and_stats_survive_the_stop();
    test_frames_all_pass_their_crc_and_fit_the_budget();
    return test_report("test_dbg_stream");
}
