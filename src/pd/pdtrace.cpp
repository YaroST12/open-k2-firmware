// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD black box recorder, see k2/pd/pdtrace.hpp. Writers run in the main loop only (fusb302 service /
// poll, power poll, remote), never in an interrupt, so the ring needs no lock.
#include "k2/pd/pdtrace.hpp"

#include "k2/core/bbstore.hpp"
#include "k2/core/hot.h"
#include "k2/fastcharge/edges.hpp"

#include <initializer_list>

#include "k2/drivers/systick.hpp"

namespace k2::pdtrace {

namespace {

Ring<kRingBytes> g_ring;
SampleGate g_gate;
Captures<kCapSlots, kCapBytes> g_caps;
DipDetector g_dip;
bbstore::IdleGate g_idle;
view::LinkAcc g_link;
std::uint8_t g_last_pos;
std::uint32_t g_contract_ms;
bool g_contract_seen;
std::uint32_t g_prev_entry_us, g_entry_us;   // entry times of the previous and the current service() call
bool g_have_prev;
std::size_t g_edge_slot;

Collapse<kRingBytes> g_collapse;

// One record into the ring and the capture slots still filling (Collapse decides which records get here).
K2_HOT bool emit(Type t, std::uint32_t ts, std::span<const std::uint8_t> p) {
    const std::uint32_t h0 = g_ring.head();
    if (!g_ring.push(t, ts, p)) return false;
    std::array<std::uint8_t, kMaxRecord> r;   // the record just written, for the slots still filling
    const auto n = g_ring.read(h0, r).bytes;
    g_caps.feed(systick::millis(), {r.data(), n});
    return true;
}
std::uint32_t g_stamp_us, g_last_ts_us;   // the frame's INT_N edge (stamp_rx), the newest record's timestamp
bool g_have_stamp;
K2_HOT void push(Type t, std::span<const std::uint8_t> p) {
    if (!g_ring.frozen() && (t == Type::Rx || t == Type::Tx || t == Type::Vbus)) g_idle.touch(systick::millis());   // a collapsed frame is activity too
    std::uint32_t ts = g_have_stamp && (t == Type::Rx || t == Type::RxBad || t == Type::Poll) ? g_stamp_us : systick::micros();
    if (std::int32_t(ts - g_last_ts_us) < 0) ts = g_last_ts_us;   // the ring's time stays monotone (the host unwraps by comparing)
    g_last_ts_us = ts;
    g_collapse.record(g_ring, t, ts, p, emit);
}
// What Collapse holds back (a run of repeated frames, a Poll) goes into the ring before it is read, frozen or copied to a capture.
K2_HOT void flush() { g_collapse.flush(emit); }
K2_HOT void rec(Type t, std::initializer_list<std::uint8_t> p) { push(t, {p.begin(), p.size()}); }
K2_HOT void rec_buf(Type t, std::uint8_t* p, std::size_t n) { push(t, {p, n}); }
K2_HOT void trigger(Trigger t) { flush(); g_caps.start(t, systick::millis(), g_ring); }
std::uint8_t lo(unsigned v) { return std::uint8_t(v); }
std::uint8_t hi(unsigned v) { return std::uint8_t(v >> 8); }

}  // namespace

void init() {
    g_ring.freeze(false);
    g_ring.clear();
    g_collapse.reset();
    g_caps.clear();
    g_idle.touch(systick::millis());
    g_link.reset();
    mark(kMarkBoot);
}
void freeze(bool on) {
    if (on) flush();
    g_ring.freeze(on);
}
void clear() { g_ring.clear(); g_collapse.reset(); }
void clear_captures() { g_caps.clear(); }
bool edges_begin() {
    g_edge_slot = g_caps.begin_open(Trigger::Edges, systick::millis(), g_ring, 0);
    return true;
}
bool edges_chunk(std::uint32_t ts_us, std::span<const std::uint8_t> payload) {
    std::array<std::uint8_t, 255> r;
    const std::size_t len = kHeader + payload.size();
    if (len > r.size()) return false;
    r[0] = std::uint8_t(len);
    r[1] = std::uint8_t(Type::Edges);
    for (int i = 0; i < 4; i++) r[2 + i] = std::uint8_t(ts_us >> (8 * i));
    for (std::size_t i = 0; i < payload.size(); i++) r[kHeader + i] = payload[i];
    return g_caps.append(g_edge_slot, {r.data(), len});
}
std::size_t edges_store(std::span<const std::uint32_t> w, std::uint32_t arm_cycles, std::uint32_t arm_us) {
    edges_begin();
    std::size_t from = 0;
    std::uint8_t idx = 0;
    while (from < w.size()) {
        std::uint8_t buf[edges::kChunkBytes + 8];
        std::size_t len = 0;
        const std::size_t used = edges::encode_chunk(w, from, idx, buf, len);
        if (used == 0) break;
        if (!edges_chunk(edges::word_us(w[from], arm_cycles, arm_us), {buf, len})) break;   // slot full
        from += used;
        idx++;
    }
    return from;
}
const view::Link& live_link() { return g_link.link(); }
std::uint32_t since_activity_ms() { return g_idle.since(systick::millis()); }
bool idle() { return g_idle.idle(systick::millis()); }
bool restore_capture(Trigger t, std::span<const std::uint8_t> data, std::size_t pre) { return g_caps.restore(t, data, pre); }
void keep_capture(std::size_t slot, bool on) { g_caps.keep(slot, on); }

CaptureInfo capture_info(std::size_t slot) {
    CaptureInfo i;
    if (slot >= kCapSlots) return i;
    const auto& s = g_caps.slot(slot);
    if (!s.seq) return i;
    i.trigger = s.trigger;
    i.len = s.len;
    i.pre = s.pre;
    i.age_ms = systick::millis() - s.at_ms;
    i.filling = s.filling;
    i.seq = s.seq;
    i.stored = s.stored;
    return i;
}

std::span<const std::uint8_t> capture_data(std::size_t slot) {
    if (slot >= kCapSlots) return {};
    const auto& s = g_caps.slot(slot);
    return s.seq ? std::span<const std::uint8_t>(s.buf.data(), s.len) : std::span<const std::uint8_t>{};
}

namespace {
EventSink g_sink = nullptr;
std::uint32_t first_object(std::span<const std::uint8_t> p) {
    return p.size() >= 4 ? std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24 : 0;
}
}  // namespace
void set_event_sink(EventSink s) { g_sink = s; }

K2_HOT void rx(std::uint8_t token, std::uint16_t header, std::span<const std::uint8_t> objs_and_crc) {
    std::array<std::uint8_t, 3 + 7 * 4 + 4> b;
    std::size_t n = 0;
    b[n++] = token; b[n++] = lo(header); b[n++] = hi(header);
    for (std::uint8_t v : objs_and_crc) if (n < b.size()) b[n++] = v;
    g_link.feed_message(true, {b.data(), n});
    rec_buf(Type::Rx, b.data(), n);
    g_have_stamp = false;
    if (g_sink) g_sink(kEvRx, header, n >= 3 + 4 + 4 ? first_object({b.data() + 3, n - 3}) : 0);
}
K2_HOT void rx_bad(std::uint8_t token, std::uint8_t b1, std::uint8_t b2) { rec(Type::RxBad, {token, b1, b2}); g_have_stamp = false; }
K2_HOT void stamp_rx(std::uint32_t us) { g_stamp_us = us; g_have_stamp = true; }
K2_HOT void tx(std::uint16_t header, std::span<const std::uint8_t> objs, std::uint8_t sop_token) {
    std::array<std::uint8_t, 3 + 7 * 4> b;   // header, up to 7 objects, trailing SOP token
    std::size_t n = 0;
    b[n++] = lo(header); b[n++] = hi(header);
    for (std::uint8_t v : objs) if (n + 1 < b.size()) b[n++] = v;
    b[n++] = sop_token;
    g_link.feed_message(false, {b.data(), n});
    rec_buf(Type::Tx, b.data(), n);
    if (g_sink) g_sink(kEvTx, header, first_object(objs));
}
K2_HOT void tx_result(TxResult r, std::uint32_t elapsed_ms, std::uint8_t interrupta) {
    rec(Type::TxResult, {std::uint8_t(r), std::uint8_t(elapsed_ms > 255 ? 255 : elapsed_ms), interrupta});
}
K2_HOT void reset(ResetKind k) {
    rec(Type::Reset, {std::uint8_t(k)});
    if (g_sink) g_sink(kEvReset, std::uint16_t(k), 0);
    if (k == ResetKind::HardRx || k == ResetKind::HardTx) { trigger(Trigger::HardReset); g_link.reset(); }
}
K2_HOT void cc(CcEvent e, std::uint8_t cc_line, std::uint8_t s0) {
    rec(Type::Cc, {std::uint8_t(e), cc_line, s0});
    if (g_sink) g_sink(kEvCc, std::uint16_t(e), cc_line);
    if (e == CcEvent::Detach) { trigger(Trigger::Detach); g_last_pos = 0; g_link.reset(); }
}
K2_HOT void reg_write(std::uint8_t reg, std::uint8_t value, bool ok) {
    rec(Type::Reg, {std::uint8_t(ok ? reg : reg | 0x80), value});
}
K2_HOT void status0(std::uint8_t v) { rec(Type::Status0, {v}); }
K2_HOT void irq(std::uint8_t a, std::uint8_t b, std::uint8_t i) {
    if (a | b | i) rec(Type::Irq, {a, b, i});
}
void link(bool inline_mode, bool auto_sink) {
    rec(Type::Link, {std::uint8_t((inline_mode ? 1 : 0) | (auto_sink ? 2 : 0))});
}
void sink(std::uint8_t state, std::uint8_t result, bool held, bool epr, std::uint8_t pos) {
    rec(Type::Sink, {state, result, std::uint8_t((held ? 1 : 0) | (epr ? 2 : 0)), pos});
    if (pos != 0 && pos != g_last_pos) {
        g_contract_ms = systick::millis();
        g_contract_seen = true;
        trigger(Trigger::Contract);
    }
    g_last_pos = pos;
}
K2_HOT void vbus_sample(std::int32_t mv, std::int32_t ma) {
    const std::uint32_t now = systick::millis();
    g_caps.tick(now);
    // a deliberate voltage change is not a dip: the Contract capture covers it
    const bool dip = g_dip.update(now, mv);
    if (dip && !(g_contract_seen && now - g_contract_ms < 500)) {
        rec(Type::Vbus, {lo(unsigned(mv)), hi(unsigned(mv)), lo(unsigned(ma)), hi(unsigned(ma))});
        trigger(Trigger::VbusDip);
    }
    if (!g_gate.due(now, mv, ma)) return;
    rec(Type::Vbus, {lo(unsigned(mv)), hi(unsigned(mv)), lo(unsigned(ma)), hi(unsigned(ma))});
}
K2_HOT void service_called() {
    g_have_stamp = false;   // a stamp the last call did not spend (a frame that failed to read) is stale
    g_have_prev = g_entry_us != 0;
    g_prev_entry_us = g_entry_us;
    g_entry_us = systick::micros() | 1;   // never 0: 0 means "no call yet"
}
K2_HOT void poll_gap() {
    const std::uint32_t now = systick::micros();
    const std::uint32_t gap = g_have_prev ? now - g_prev_entry_us : 0;
    rec(Type::Poll, {lo(gap), lo(gap >> 8), lo(gap >> 16), lo(gap >> 24)});
}
void mark(std::uint8_t code, std::uint16_t a, std::uint16_t b) {
    rec(Type::Mark, {code, lo(a), hi(a), lo(b), hi(b)});
}
void refusal(std::uint16_t a, std::uint16_t b) {
    mark(kMarkRefusal, a, b);
    trigger(Trigger::Refusal);
}

std::size_t remote(std::uint8_t op, std::uint32_t arg, std::span<std::uint8_t> out) {
    if (out.size() < kReadHeader) return 0;
    flush();
    Ring<kRingBytes>::ReadResult r;
    if (op == kOpCapList) {
        constexpr std::size_t need = kReadHeader > 4 + kCapSlots * 7 ? kReadHeader : 4 + kCapSlots * 7;
        if (out.size() < need) return 0;
        const std::uint32_t now = systick::millis();
        for (int i = 0; i < 4; i++) out[i] = std::uint8_t(now >> (8 * i));
        for (std::size_t k = 0; k < kCapSlots; k++) {
            const auto& s = g_caps.slot(k);
            std::uint8_t* o = &out[4 + k * 7];
            o[0] = std::uint8_t(s.seq ? s.trigger : Trigger::None);
            o[1] = lo(s.len); o[2] = hi(s.len);
            const std::uint32_t age = s.seq ? now - s.at_ms : 0;
            for (int i = 0; i < 4; i++) o[3 + i] = std::uint8_t(age >> (8 * i));
        }
        return need;
    }
    if (op == kOpCapRead) {
        const std::size_t k = arg >> 16;
        std::uint32_t off = arg & 0xFFFF;
        if (k >= kCapSlots) return 0;
        const auto& s = g_caps.slot(k);
        std::size_t n = 0;
        if (s.seq && off < s.len) {
            // whole records only: walk from the offset (the host passes back what it was given)
            auto room = out.size() - kReadHeader;
            // A length byte below the header (corrupt slot, or an offset inside a record) ends the walk.
            while (off + n < s.len && s.buf[off + n] >= kHeader && s.buf[off + n] <= room - n &&
                   off + n + s.buf[off + n] <= s.len)
                n += s.buf[off + n];
            for (std::size_t i = 0; i < n; i++) out[kReadHeader + i] = s.buf[off + i];
        }
        out[0] = s.filling ? kFlagFilling : 0;
        const std::uint32_t next = off + n, len = s.seq ? s.len : 0;
        for (int i = 0; i < 4; i++) out[1 + i] = std::uint8_t(next >> (8 * i));
        for (int i = 0; i < 4; i++) out[5 + i] = std::uint8_t(len >> (8 * i));
        return kReadHeader + n;
    }
    if (op == kOpCapClear) clear_captures();
    switch (op) {
    case kOpClear: g_ring.clear(); g_collapse.reset(); break;
    case kOpFreeze: g_ring.freeze(arg != 0); break;
    case kOpRead: r = g_ring.read(arg, out.subspan(kReadHeader)); break;
    default: break;
    }
    if (op != kOpRead) {   // (also kOpCapClear)   // clear / freeze / status: report the ring state, next = the oldest cursor
        r.flags = std::uint8_t(g_ring.frozen() ? kFlagFrozen : 0);
        r.next = g_ring.tail();
    }
    out[0] = r.flags;
    for (int i = 0; i < 4; i++) out[1 + i] = std::uint8_t(r.next >> (8 * i));
    const std::uint32_t head = g_ring.head();
    for (int i = 0; i < 4; i++) out[5 + i] = std::uint8_t(head >> (8 * i));
    return kReadHeader + r.bytes;
}

const Ring<kRingBytes>& live_ring() { return g_ring; }

}  // namespace k2::pdtrace
