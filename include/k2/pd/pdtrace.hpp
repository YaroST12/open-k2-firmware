// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD black box: a raw FUSB302 / PD trace recorder. Every wire event (RX frame, TX request and its
// result, hard / soft reset, CC attach and detach, each configuration register write, link and
// sink-engine state changes, VBUS / current samples, late polls) is appended as a tiny binary
// record to a static RAM ring. Nothing is formatted on the device: the host reads the ring over the
// remote command 0x70 (tools/pd_trace.py) and decodes it. The HID text log drops lines under load;
// this channel does not lose a record unless the ring wraps before the host reads it.
//
// Record layout (little endian), one per event:
//   len(1)  total bytes of the record, header included (6 .. kMaxRecord)
//   type(1) Type below
//   ts(4)   microseconds from systick::micros() (wraps every ~71 min, the host unwraps)
//   payload (len - 6 bytes), per type:
//     Rx         token(1) header(2) objects(4 * n) crc(4)     n = header bits 14:12, crc as read
//     RxBad      token(1) b1(1) b2(1)                          unknown FIFO token, FIFO flushed
//     Tx         header(2) objects(4 * n)                      as written to the TX FIFO (MessageID in)
//     TxResult   result(1) elapsed_ms(1) interrupta(1)         result: TxSent / RetryFail / Timeout / RxGoodCrc
//     Reset      kind(1)                                       ResetKind
//     Cc         event(1) cc(1) status0(1)                     CcEvent; cc 0 none, 1, 2; STATUS0 raw
//     Reg        reg(1) value(1)                               bit 7 of reg set = the I2C write failed
//     Status0    status0(1)                                    STATUS0 changed (COMP, BC_LVL, VBUSOK)
//     Irq        interrupta(1) interruptb(1) interrupt(1)      nonzero values read (and so cleared)
//     Link       flags(1)                                      bit0 inline, bit1 auto_sink
//     Sink       state(1) result(1) flags(1) pos(1)            pd::SinkStatus; flags bit0 held, bit1 epr
//     Vbus       mv(2) ma(2, signed)
//     Poll       gap_us(4)                                     RX FIFO was non-empty; time since the
//                                                              previous service() entry (an upper bound
//                                                              of the latency from the frame's arrival)
//     Mark       code(1) a(2) b(2)                             Mark codes below
//     Edges      idx(1) first_units(4) varint edges            D+/D- edges, k2/fastcharge/edges.hpp; only in an Edges capture
//     RxRepeat   count(2) span_ms(2)                           the Source_Capabilities recorded before was received `count` more
//                                                              times (same objects; MessageID and CRC aside) over `span_ms`; the
//                                                              record's ts is that of the first repeat. See Collapse.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "k2/pd/pdtrace_link.hpp"

namespace k2::pdtrace {

enum class Type : std::uint8_t {
    Rx = 1, RxBad, Tx, TxResult, Reset, Cc, Reg, Status0, Irq, Link, Sink, Vbus, Poll, Mark, Edges, RxRepeat,
};
// RxGoodCrc: no TXSENT, but the partner's GoodCRC with our MessageID came out of the RX FIFO (fusb302.cpp rx_ack_peek): sent.
enum class TxResult : std::uint8_t { TxSent = 0, RetryFail = 1, Timeout = 2, RxGoodCrc = 3 };
enum class ResetKind : std::uint8_t { HardRx = 0, HardTx = 1, SoftRx = 2, SoftTx = 3 };
enum class CcEvent : std::uint8_t { Detach = 0, Attach = 1 };
// Mark codes
inline constexpr std::uint8_t kMarkCaps = 1;     // Source_Capabilities seen: a = auto_sink | inline << 1 | held << 2, b = sink state
inline constexpr std::uint8_t kMarkAutoReq = 2;  // run-start Request decision on Source_Capabilities: a = pd::Err, or 0x100 | inline 1 | no CC 2 | sink busy 4 | already requested 8 when none was made, or 0x200 | pd::Err for the Request the acked-caps rule made
inline constexpr std::uint8_t kMarkBoot = 3;     // trace started
inline constexpr std::uint8_t kMarkStartWait = 4;  // the charger-test START's Rd wait ended: a = VBUS mV, b = elapsed ms | 0x8000 when VBUS came up
inline constexpr std::uint8_t kMarkReread = 5;     // source-cap re-read: start a = mask, b = PDOs held; done a = 0x8000 | skipped << 8 | ok, b = avs << 15 | caps_rx << 8 | pdos_last
inline constexpr std::uint8_t kMarkQc3Batch = 6;   // a QC3 / QC3+ pulse batch is about to play: a = VBUS mV before it (the next mark's a is the result), b = signed pulse count (int16)
inline constexpr std::uint8_t kMarkVconn = 7;      // VCONN_Swap experiment: a = state | why << 4 | applied << 8 | active cc << 12, b = mV on the net VCONN goes on
inline constexpr std::uint8_t kMarkRefusal = 8;    // a refused 6 A hunt row: a = HuntCause | answer << 8 | (timeout 1) << 12, b = targeted APDO limit in mA (the capture freezes with Trigger::Refusal)

inline constexpr std::size_t kHeader = 6;
inline constexpr std::size_t kMaxRecord = kHeader + 1 + 2 + 7 * 4 + 4;  // 41: a 7-object Rx frame
inline constexpr std::size_t kRingBytes = 8192;
inline constexpr std::size_t kReadHeader = 9;  // flags(1) next(4) head(4), then records

// Flags of a read response.
inline constexpr std::uint8_t kFlagLost = 1;    // the cursor pointed at overwritten bytes: it was moved forward
inline constexpr std::uint8_t kFlagFrozen = 2;  // recording is frozen
inline constexpr std::uint8_t kFlagFilling = 8;  // capture read: the slot is still recording post-trigger records
inline constexpr std::uint8_t kFlagReset = 4;   // the cursor was beyond the head (device rebooted): restarted at the tail

// Byte ring of whole records. `head` counts every byte ever written, so a cursor is simply a
// byte offset that stays valid until the ring overwrites it. Pure: no hardware, host-tested.
template <std::size_t N>
class Ring {
public:
    static_assert(N > 2 * 255);

    // Append one record. `payload` may be empty; `len` = 6 + payload size must fit one byte.
    // Returns false (and records nothing) while frozen.
    bool push(Type type, std::uint32_t ts, std::span<const std::uint8_t> payload) {
        if (frozen_) { missed_++; return false; }
        const std::size_t len = kHeader + payload.size();
        if (len > 255) return false;
        while (head_ - tail_ + len > N) { tail_ += buf_[tail_ % N]; dropped_++; }   // drop the oldest whole record
        pushed_++;
        put(std::uint8_t(len));
        put(std::uint8_t(type));
        for (int i = 0; i < 4; i++) put(std::uint8_t(ts >> (8 * i)));
        for (std::uint8_t b : payload) put(b);
        return true;
    }

    struct ReadResult {
        std::uint8_t flags = 0;
        std::uint32_t next = 0;    // cursor for the following read
        std::size_t bytes = 0;     // bytes copied into `out`: whole records only
    };
    // Copy whole records from `cursor` into `out`. A cursor older than the oldest record is moved
    // up to it (kFlagLost); a cursor beyond the head restarts at the tail (kFlagReset).
    ReadResult read(std::uint32_t cursor, std::span<std::uint8_t> out) const {
        ReadResult r;
        if (frozen_) r.flags |= kFlagFrozen;
        if (cursor > head_) { cursor = tail_; r.flags |= kFlagReset; }
        else if (cursor < tail_) { cursor = tail_; r.flags |= kFlagLost; }
        while (cursor != head_) {
            const std::size_t len = buf_[cursor % N];
            if (r.bytes + len > out.size()) break;
            for (std::size_t i = 0; i < len; i++) out[r.bytes + i] = buf_[(cursor + i) % N];
            r.bytes += len;
            cursor += len;
        }
        r.next = cursor;
        return r;
    }

    // Cursor of the oldest whole record such that at most `max` bytes lie between it and the head.
    std::uint32_t cursor_for_last(std::size_t max) const {
        std::uint32_t c = tail_;
        while (head_ - c > max) c += buf_[c % N];
        return c;
    }

    // A record start (an offset `head()` had when the record was pushed) that the ring has not overwritten yet.
    bool holds(std::uint32_t off) const { return off - tail_ < head_ - tail_; }
    std::uint8_t byte_at(std::uint32_t off) const { return buf_[off % N]; }

    void clear() { tail_ = head_; dropped_ = pushed_; }   // offsets stay monotonic: a reader sees "lost", not garbage
    void freeze(bool on) { frozen_ = on; }
    bool frozen() const { return frozen_; }
    std::uint32_t head() const { return head_; }
    std::uint32_t tail() const { return tail_; }
    std::uint32_t missed() const { return missed_; }   // records refused while frozen
    // Record numbers for a streaming reader (k2/core/trace_stream.hpp): pushed = records ever written,
    // dropped = records that left the tail (overwritten or cleared), so the record at `tail` is number `dropped`.
    std::uint32_t records_pushed() const { return pushed_; }
    std::uint32_t records_dropped() const { return dropped_; }

private:
    void put(std::uint8_t b) { buf_[head_ % N] = b; head_++; }
    std::array<std::uint8_t, N> buf_{};
    std::uint32_t head_ = 0, tail_ = 0, missed_ = 0, pushed_ = 0, dropped_ = 0;
    bool frozen_ = false;
};

// Decides when a VBUS / current sample is worth a record: at most one per 50 ms, and only when
// either value moved more than 100 mV / 20 mA from the last recorded sample.
class SampleGate {
public:
    static constexpr std::uint32_t kPeriodMs = 50;
    static constexpr std::int32_t kMvStep = 100, kMaMStep = 20;
    bool due(std::uint32_t now_ms, std::int32_t mv, std::int32_t ma) {
        if (have_ && now_ms - last_ms_ < kPeriodMs) return false;
        if (have_ && std::abs(mv - mv_) <= kMvStep && std::abs(ma - ma_) <= kMaMStep) return false;
        have_ = true; last_ms_ = now_ms; mv_ = mv; ma_ = ma;
        return true;
    }
private:
    bool have_ = false;
    std::uint32_t last_ms_ = 0;
    std::int32_t mv_ = 0, ma_ = 0;
};

// Flood control for a source that talks to a deaf sink (hardware 2026-11-07, Baseus 70 W under a PD-silent Legacy unit): it re-sends
// the same Source_Capabilities three times ~2.2 ms apart and again every ~195 ms, each time with a new MessageID and CRC, and every
// frame brought a Poll, an Irq and two Mark records with it: the 8 KB ring wrapped in seconds and the unit itself never showed.
// The recorder routes every record through here:
//   - a Source_Capabilities frame equal to the last recorded one apart from MessageID and CRC is not written; it is counted
//     (the run). The run is written as one RxRepeat record (count, span) when anything else is recorded, a capture starts, the
//     ring is read or frozen.
//   - the Poll record of such a frame goes with it (a Poll is held back until the next record, which tells whether it stays);
//   - while a run is open an Irq (INTERRUPT register only) or a Mark (caps-seen, start-request) that already sits among the
//     records after the last recorded frame is dropped too. Anything different (an Irq with INTERRUPTA / INTERRUPTB, another
//     mark, any other record) ends the run, and the next frame is recorded in full.
// State: a ring offset, a run count with two timestamps and one held Poll (about 28 bytes).
template <std::size_t N>
class Collapse {
public:
    // `write(type, ts, payload) -> bool` appends one record to the ring (and to whatever follows the ring).
    template <class Write>
    void record(const Ring<N>& ring, Type t, std::uint32_t ts, std::span<const std::uint8_t> p, Write&& write) {
        if (ring.frozen()) { write(t, ts, p); return; }
        if (t == Type::Poll && p.size() == poll_.size()) {
            flush_poll(write);
            poll_held_ = true;
            poll_ts_ = ts;
            for (std::size_t i = 0; i < poll_.size(); i++) poll_[i] = p[i];
            return;
        }
        if (t == Type::Rx && is_caps(p)) {
            if (anchored(ring) && same_caps(ring, p)) {
                if (run_ == 0) first_ts_ = ts;
                if (run_ != 0xFFFF) run_++;
                last_ts_ = ts;
                poll_held_ = false;   // this frame's own latency record
                return;
            }
            flush(write);
            const std::uint32_t at = ring.head();
            have_ = write(t, ts, p);
            anchor_ = at;
            return;
        }
        if (bookkeeping(t, p) && anchored(ring) && (run_ == 0 || in_cluster(ring, t, p))) {
            if (run_ != 0) return;   // the frame's own note again
            flush_poll(write);
            write(t, ts, p);
            return;
        }
        flush(write);
        have_ = false;
        write(t, ts, p);
    }
    // Writes what is held back (before a read, a freeze or a capture start).
    template <class Write>
    void flush(Write&& write) {
        flush_run(write);
        flush_poll(write);
    }
    void reset() { have_ = poll_held_ = false; run_ = 0; }
    std::uint16_t run() const { return run_; }

private:
    static constexpr std::size_t kLook = 12;   // records after the anchor that make "its cluster"

    static bool is_caps(std::span<const std::uint8_t> p) {   // Rx payload: token, header(2), objects, CRC(4)
        if (p.size() < 3 + 4 + 4) return false;
        const unsigned h = p[1] | p[2] << 8;
        return (h & 0x1F) == 1 && (h & 0x8000) == 0 && ((h >> 12) & 7) != 0;   // data message, not extended, has objects
    }
    static bool bookkeeping(Type t, std::span<const std::uint8_t> p) {
        if (t == Type::Irq) return p.size() == 3 && p[0] == 0 && p[1] == 0;
        if (t == Type::Mark) return p.size() >= 1 && (p[0] == kMarkCaps || p[0] == kMarkAutoReq);
        return false;
    }
    bool anchored(const Ring<N>& ring) const { return have_ && ring.holds(anchor_); }
    bool same_caps(const Ring<N>& ring, std::span<const std::uint8_t> p) const {
        if (ring.byte_at(anchor_) != kHeader + p.size() || ring.byte_at(anchor_ + 1) != std::uint8_t(Type::Rx)) return false;
        for (std::size_t i = 0; i + 4 < p.size(); i++) {   // all but the CRC
            std::uint8_t a = ring.byte_at(anchor_ + kHeader + std::uint32_t(i)), b = p[i];
            if (i == 2) { a &= 0xF1; b &= 0xF1; }          // header high byte: MessageID is bits 3:1
            if (a != b) return false;
        }
        return true;
    }
    bool in_cluster(const Ring<N>& ring, Type t, std::span<const std::uint8_t> p) const {
        std::uint32_t o = anchor_ + ring.byte_at(anchor_);
        for (std::size_t k = 0; k < kLook && o != ring.head(); k++) {
            const std::size_t len = ring.byte_at(o);
            if (len < kHeader) return false;
            if (ring.byte_at(o + 1) == std::uint8_t(t) && len == kHeader + p.size()) {
                bool same = true;
                for (std::size_t i = 0; i < p.size() && same; i++) same = ring.byte_at(o + kHeader + std::uint32_t(i)) == p[i];
                if (same) return true;
            }
            o += std::uint32_t(len);
        }
        return false;
    }
    template <class Write>
    void flush_run(Write& write) {
        if (run_ == 0) return;
        const std::uint32_t span = (last_ts_ - first_ts_) / 1000;
        const std::uint16_t ms = span > 0xFFFF ? 0xFFFF : std::uint16_t(span);
        const std::uint8_t pl[4] = {std::uint8_t(run_), std::uint8_t(run_ >> 8), std::uint8_t(ms), std::uint8_t(ms >> 8)};
        write(Type::RxRepeat, first_ts_, std::span<const std::uint8_t>(pl));
        run_ = 0;
    }
    template <class Write>
    void flush_poll(Write& write) {
        if (!poll_held_) return;
        poll_held_ = false;
        write(Type::Poll, poll_ts_, std::span<const std::uint8_t>(poll_));
    }

    std::uint32_t anchor_ = 0;          // ring offset of the last Source_Capabilities record written
    std::uint32_t first_ts_ = 0, last_ts_ = 0;
    std::uint16_t run_ = 0;             // frames collapsed since the last RxRepeat record
    std::uint32_t poll_ts_ = 0;
    std::array<std::uint8_t, 4> poll_{};
    bool have_ = false;                 // anchor_ is valid
    bool poll_held_ = false;
};

// Trigger-frozen captures: when something interesting happens the last kPre bytes of the ring are
// copied into a slot, and records that follow are appended for a short while (or until the slot is
// full). The ring keeps running. Slots hold plain records, so the host decodes them like the ring.
enum class Trigger : std::uint8_t { None = 0, Contract = 1, HardReset = 2, VbusDip = 3, Detach = 4, Edges = 5, Refusal = 6 };
inline constexpr std::size_t kCapSlots = 3, kCapBytes = 2048, kCapPre = 1536;
inline constexpr std::uint32_t kCapPostMs = 500, kTriggerCooldownMs = 1000;

template <std::size_t Slots, std::size_t Bytes>
class Captures {
public:
    struct Slot {
        Trigger trigger = Trigger::None;
        std::uint32_t seq = 0;       // order of capture, 0 = empty
        std::uint32_t at_ms = 0;     // millis() at the trigger
        std::uint32_t until_ms = 0;  // post-trigger recording ends here
        std::uint16_t len = 0, pre = 0;
        bool filling = false;
        bool stored = false;         // loaded from flash after a boot: at_ms is meaningless
        bool keep = false;           // a finished capture still waiting for its flash write: a new capture takes another slot
        std::array<std::uint8_t, Bytes> buf{};
    };

    // Start a capture. `ring` supplies the pre-trigger records. A trigger of the same kind inside
    // the cooldown is ignored (a hard-reset loop would otherwise evict every other capture).
    template <std::size_t N>
    bool start(Trigger t, std::uint32_t now_ms, const Ring<N>& ring, std::size_t pre_max = kCapPre) {
        auto& cd = cooldown_[std::size_t(t) % 8];
        if (cd.set && now_ms - cd.at < kTriggerCooldownMs) return false;
        cd = {true, now_ms};
        Slot* pick = pick_slot();
        pick->trigger = t;
        pick->seq = ++seq_;
        pick->at_ms = now_ms;
        pick->until_ms = now_ms + kCapPostMs;
        pick->filling = true;
        pick->stored = false;
        pick->keep = false;
        const auto r = ring.read(ring.cursor_for_last(pre_max < Bytes ? pre_max : Bytes), pick->buf);
        pick->len = std::uint16_t(r.bytes);
        pick->pre = pick->len;
        return true;
    }

    // Open a capture that is filled by explicit append() calls instead of by feed(): the pre-window
    // comes from the ring, no cooldown applies. Returns the slot index.
    template <std::size_t N>
    std::size_t begin_open(Trigger t, std::uint32_t now_ms, const Ring<N>& ring, std::size_t pre_max) {
        Slot* pick = pick_slot();
        pick->trigger = t;
        pick->seq = ++seq_;
        pick->at_ms = pick->until_ms = now_ms;
        pick->filling = false;
        pick->stored = false;
        pick->keep = false;
        const auto r = ring.read(ring.cursor_for_last(pre_max < Bytes ? pre_max : Bytes), pick->buf);
        pick->len = pick->pre = std::uint16_t(r.bytes);
        return std::size_t(pick - slots_.data());
    }
    // Append one whole record to a slot; false when it does not fit.
    bool append(std::size_t slot, std::span<const std::uint8_t> record) {
        if (slot >= Slots) return false;
        auto& s = slots_[slot];
        if (s.len + record.size() > Bytes) return false;
        for (std::uint8_t b : record) s.buf[s.len++] = b;
        return true;
    }

    // Put a capture read back from flash into the oldest slot. It never fills and carries no age.
    bool restore(Trigger t, std::span<const std::uint8_t> data, std::size_t pre) {
        if (data.size() > Bytes) return false;
        Slot* pick = pick_slot();
        pick->trigger = t;
        pick->seq = ++seq_;
        pick->at_ms = pick->until_ms = 0;
        pick->filling = false;
        pick->stored = true;
        pick->keep = false;
        pick->len = std::uint16_t(data.size());
        pick->pre = std::uint16_t(pre < data.size() ? pre : data.size());
        for (std::size_t i = 0; i < data.size(); i++) pick->buf[i] = data[i];
        return true;
    }

    // Append a freshly recorded record (whole) to every slot still filling.
    void feed(std::uint32_t now_ms, std::span<const std::uint8_t> record) {
        for (auto& s : slots_) {
            if (!s.filling) continue;
            if (std::int32_t(now_ms - s.until_ms) >= 0 || s.len + record.size() > Bytes) { s.filling = false; continue; }
            for (std::uint8_t b : record) s.buf[s.len++] = b;
        }
    }
    void tick(std::uint32_t now_ms) {
        for (auto& s : slots_) if (s.filling && std::int32_t(now_ms - s.until_ms) >= 0) s.filling = false;
    }
    void clear() { for (auto& s : slots_) { s.seq = 0; s.trigger = Trigger::None; s.len = 0; s.filling = false; s.stored = false; s.keep = false; } cooldown_ = {}; }
    const Slot& slot(std::size_t i) const { return slots_[i]; }
    static constexpr std::size_t count() { return Slots; }
    // Protects a slot's capture from reuse while a newer capture can take another slot (see Slot::keep).
    void keep(std::size_t i, bool on) { if (i < Slots) slots_[i].keep = on; }

private:
    // The slot a new capture takes: an empty one, else the oldest that is not kept, else the oldest of all (a run that
    // makes more captures than there are slots still loses the oldest, which the persister counts).
    Slot* pick_slot() {
        Slot* oldest = &slots_[0];
        Slot* unkept = nullptr;
        for (auto& s : slots_) {
            if (s.seq == 0) return &s;
            if (s.seq < oldest->seq) oldest = &s;
            if (!s.keep && (!unkept || s.seq < unkept->seq)) unkept = &s;
        }
        return unkept ? unkept : oldest;
    }
    struct Cool { bool set = false; std::uint32_t at = 0; };
    std::array<Slot, Slots> slots_{};
    std::array<Cool, 8> cooldown_{};
    std::uint32_t seq_ = 0;
};

// VBUS dip: more than kDipMv below the reading ~20 ms earlier (no per-millisecond sampling exists,
// so "10 % below contract for 2 ms" is approximated by a step).
class DipDetector {
public:
    static constexpr std::int32_t kDipMv = 300;
    static constexpr std::uint32_t kRefMs = 20;
    bool update(std::uint32_t now_ms, std::int32_t mv) {
        if (!have_) { have_ = true; ref_ = mv; ref_ms_ = now_ms; return false; }
        const bool dip = ref_ - mv > kDipMv;
        if (dip || now_ms - ref_ms_ >= kRefMs) { ref_ = mv; ref_ms_ = now_ms; }
        return dip;
    }
private:
    bool have_ = false;
    std::int32_t ref_ = 0;
    std::uint32_t ref_ms_ = 0;
};

// ---- the on-device recorder (one static ring, src/pd/pdtrace.cpp) ----

void init();                                   // clears the ring, writes the Boot mark
void freeze(bool on);
void clear();
void clear_captures();
// Edge capture (k2/fastcharge/edges.hpp): edges_begin() opens an Edges capture with a short pre-window of PD
// messages and V/I samples; edges_chunk() appends one Type::Edges record (ts = systick us of its
// first edge) and returns false once the slot is full. Main loop only.
bool edges_begin();
bool edges_chunk(std::uint32_t ts_us, std::span<const std::uint8_t> payload);
// Opens an Edges capture and stores as many of `words` as fit (no pre-window: the edges get the whole
// slot, one record per edges::kChunkBytes so a record always fits one remote read). Returns the number
// of edge words stored. Main loop only.
std::size_t edges_store(std::span<const std::uint32_t> words, std::uint32_t arm_cycles, std::uint32_t arm_us);

// Activity for the flash recorder: PD messages and VBUS changes. since_activity_ms() is the time
// since the last one; idle() is true after bbstore::kIdleMs without any.
std::uint32_t since_activity_ms();
bool idle();
// The negotiated link seen on the wire since the last hard reset or detach (k2/pd/pdtrace_link.hpp).
const view::Link& live_link();
// Puts a capture read back from flash into the oldest slot (see Captures::restore).
bool restore_capture(Trigger t, std::span<const std::uint8_t> data, std::size_t pre);

// Read-only view of a capture slot for the UI (slot index 0..kCapSlots-1; empty slots have
// trigger None). `data` holds whole records; the first `pre` bytes precede the trigger.
struct CaptureInfo {
    Trigger trigger = Trigger::None;
    std::uint16_t len = 0, pre = 0;
    std::uint32_t age_ms = 0;
    std::uint32_t seq = 0;       // order of capture; changes when the slot is reused
    bool filling = false;
    bool stored = false;         // read back from flash after a boot: age_ms is meaningless
};
CaptureInfo capture_info(std::size_t slot);
std::span<const std::uint8_t> capture_data(std::size_t slot);
// A finished capture still waiting for its flash write is kept: a new capture takes another slot and only reuses a kept one
// when every slot is kept (the persister's `dropped` count). The flag clears when the slot is reused or cleared.
void keep_capture(std::size_t slot, bool on);

void rx(std::uint8_t token, std::uint16_t header, std::span<const std::uint8_t> objs_and_crc);
void rx_bad(std::uint8_t token, std::uint8_t b1, std::uint8_t b2);
// A TX record body ends with `sop_token`: the RX-style SOP the frame went out on (0xE0 = SOP,
// 0xC0 = SOP', 0xA0 = SOP''), so a decoder can label the direction without guessing. Zero-object
// and one-object Vendor_Defined TX frames previously mislabelled as SOP' in pd_trace.py (it
// inferred SOP' from the 0xFF00 VDO byte alone - that hit the sink's proactive Discover-SVIDs
// REQ on SOP). Default 0xE0.
void tx(std::uint16_t header, std::span<const std::uint8_t> objs, std::uint8_t sop_token = 0xE0);
void tx_result(TxResult r, std::uint32_t elapsed_ms, std::uint8_t interrupta);
void reset(ResetKind k);
void cc(CcEvent e, std::uint8_t cc, std::uint8_t status0);
void reg_write(std::uint8_t reg, std::uint8_t value, bool ok);
void status0(std::uint8_t v);
void irq(std::uint8_t a, std::uint8_t b, std::uint8_t i);
void link(bool inline_mode, bool auto_sink);
void sink(std::uint8_t state, std::uint8_t result, bool held, bool epr, std::uint8_t pos);
void vbus_sample(std::int32_t mv, std::int32_t ma);   // gated by SampleGate
// The INT_N edge time (systick::micros()) of the frame about to be recorded: the Poll, Rx and RxBad records that follow take it as
// their timestamp instead of "now" (the FIFO is read up to a millisecond after the edge), until rx() / rx_bad() spends it. The ring's
// time never goes backwards: a stamp older than the newest record is raised to it. See k2/pd/pd_rxstamp.hpp.
void stamp_rx(std::uint32_t us);
void poll_gap();                               // RX FIFO found non-empty: records the time since the previous service() entry
void service_called();                         // call on entry of every service(): the gap reference
void mark(std::uint8_t code, std::uint16_t a = 0, std::uint16_t b = 0);
// A refused 6 A hunt row: writes kMarkRefusal (a, b as documented there) and freezes a capture with
// Trigger::Refusal so the whole exchange (chain, re-announce, Request, answer) survives the ring wrapping.
void refusal(std::uint16_t a, std::uint16_t b);

// One observer of the PD traffic (the transition capture). The hooks above forward every message and event to
// it, in the main loop, after they are recorded: kind kEvRx / kEvTx a = header, b = first data object (0 if none);
// kEvReset a = ResetKind; kEvCc a = CcEvent, b = cc line. Optional: with no sink set nothing is forwarded.
inline constexpr std::uint8_t kEvRx = 0, kEvTx = 1, kEvReset = 2, kEvCc = 3;
using EventSink = void (*)(std::uint8_t kind, std::uint16_t a, std::uint32_t b);
void set_event_sink(EventSink s);

// Remote command 0x70 helper. op: 0 read from `arg` (cursor), 1 clear, 2 freeze (arg 1) / unfreeze
// (arg 0), 3 status. Writes flags(1) next(4) head(4) and, for a read, whole records into `out`;
// returns the payload size.
// Captures: op 4 lists them (now_ms(4), then per slot trigger(1) len(2) age_ms(4)), op 5 reads one
// (arg = slot << 16 | byte offset; the answer has the read layout with offsets inside the slot),
// op 6 clears them.
inline constexpr std::uint8_t kOpRead = 0, kOpClear = 1, kOpFreeze = 2, kOpStatus = 3, kOpCapList = 4, kOpCapRead = 5,
                              kOpCapClear = 6;
std::size_t remote(std::uint8_t op, std::uint32_t arg, std::span<std::uint8_t> out);
// The live ring, read-only: the CDC stream (k2/core/trace_stream.hpp) reads records from it in the main loop.
const Ring<kRingBytes>& live_ring();

}  // namespace k2::pdtrace
