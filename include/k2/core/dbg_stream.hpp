// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The multiplexed live debug stream over the CDC shell (`stream on [log|trace|power[=Hz]|run|all]`, docs/DEBUG_STREAMING.md,
// tools/k2_stream.py). One framing, the one `trace stream` already used (trace_stream.hpp: 0xA6, kind, len, payload, crc16),
// with more kinds. Pure code over the rings and a Probe: no hardware, host-tested in tests/test_dbg_stream.cpp.
//
// There is no frame queue. Every source is a ring that keeps its own history and a reader cursor here, so a host that falls
// behind costs the device nothing, and what the ring overwrote becomes an explicit Gap (or Lost) frame with the exact
// count. The one source without a ring, the power samples, folds into a window instead (min/max over the window).
// No compression: records are domain encoded (ids, delta timestamps, varints) and sent as they happen, so a crash loses
// nothing but the last frame and one lost frame is one lost record, never a block (docs/DEBUG_STREAMING.md).
//
// Kinds (all multi-byte fields little endian; Start/Data/Lost are trace_stream.hpp's, unchanged):
//   Hello(4)  ver(1) mask(1) power_hz(2) ms(4) flags(1: bit0 history from the oldest) hash(ascii, up to 24)
//   Boot(11)  boot_id(2) reset_flags(4)                                   right after Hello, once per stream start: the session
//             boot counter (it counts the K2's boots, so two boots in one recording differ) and the CRM reset-cause
//             flags of this boot (bits 26..31: PIN POR SW IWDG WWDG LPWR). Added after v1 hosts shipped: they skip it
//   Text(5)   ms(4) tag(1: 1 boot info line) chunk(up to 56)             the boot header after Hello
//   Log(6)    base_ms(4) seq(4) whole BinLog entries; base_ms is the time of the entry before the first one, seq the entry
//             number of the first; each entry is `len dt id args` as k2/core/binlog.hpp lays it out (tools/logdb.py decodes it)
//   Power(7)  ms(4) n(2) valid(1) vbus_mv(2) ibus_ma(2, signed) cc1(2) cc2(2) dp(2) dm(2)   the newest sample of the window;
//             a window of n > 1 samples appends span_ms(2) vbus_min(2) vbus_max(2) ibus_min(2) ibus_max(2). 0xFFFF = unread
//   Run(8)    ms(4) code(1) a(1) b(2) c(2) d(2)                          k2/chargertest/run_events.hpp
//   Gap(9)    src(1: 2 log, 3 run) count(4) bytes(4) flags(1)           entries the ring overwrote before they were sent
//   Beat(10)  ms(4) stack_free(2) msp_free(2) page(1) flags(1) usb_dropped(4) then nine u32 counters:
//             trace_records trace_lost log_entries log_lost run_events run_lost power_frames power_folded stalls
//
// Priority in one pass (the main loop calls service() every millisecond, a bounded number of frames each time):
//   control (Hello, boot Text) > run > power that waited too long > trace and log (taking turns at going first) > Beat >
//   power. Each kind sends at most kKindFramesPerPass frames a pass, the pass at most kMaxFramesPerPass, and a frame goes
//   out only when the FIFO has room for a worst-case one, so a pass never waits and never overfills.
// Trace, log and run events are never dropped without a Lost/Gap report. Power goes last and is decimated first: with no
// room, samples fold into the window, and a window older than kPowerStaleMs overtakes trace and log so it cannot starve.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/core/binlog.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/chargertest/run_events.hpp"
#include "k2/core/trace_stream.hpp"

namespace k2::dbg {

using trace_stream::Kind;

inline constexpr std::uint8_t kVersion = 1;
enum Mask : std::uint8_t { kLog = 1, kTrace = 2, kPower = 4, kRun = 8, kAll = kLog | kTrace | kPower | kRun };
enum class Src : std::uint8_t { Log = 2, Run = 3 };

inline constexpr std::size_t kFrameMax = 96;                 // the largest frame the multiplexer builds or reserves room for
inline constexpr unsigned kMaxFramesPerPass = 8;             // bounds the work of one main-loop pass
inline constexpr unsigned kKindFramesPerPass = 2;            // per kind: a busy source cannot starve the others
inline constexpr std::size_t kLogChunk = 64;                 // BinLog bytes per Log frame (an entry is at most 64: len + BinLog::kEntryMax)
inline constexpr std::size_t kTextChunk = 56;
inline constexpr std::size_t kHashMax = 24;
inline constexpr std::uint16_t kPowerHzMin = 1, kPowerHzMax = 100, kPowerHzDefault = 20;
inline constexpr std::uint32_t kKeepaliveMs = 1000;          // an unchanged power reading is still sent this often
inline constexpr std::uint32_t kPowerStaleMs = 500;          // a waiting power window overtakes trace and log after this
inline constexpr std::uint32_t kBeatMs = 1000;
inline constexpr int kVbusDeadbandMv = 30, kIbusDeadbandMa = 15, kLevelDeadbandMv = 30;
inline constexpr std::uint16_t kUnread = 0xFFFF;

struct PowerSample {
    std::uint32_t ms = 0;
    bool valid = false;                       // the power monitor answered
    std::uint16_t vbus = 0;
    std::int16_t ibus = 0;
    std::uint16_t cc1 = kUnread, cc2 = kUnread, dp = kUnread, dm = kUnread;   // kUnread while the ADC pins belong to another owner
};

struct BeatInfo {
    std::uint16_t stack_free = 0, msp_free = 0;
    std::uint8_t page = 0, flags = 0;
    std::uint32_t usb_dropped = 0;
};

struct BootInfo {
    std::uint16_t boots = 0;           // the session boot counter: this boot's id
    std::uint32_t reset_flags = 0;     // CRM CTRLSTS reset-cause bits of this boot
};

// What the firmware knows and the pure code does not: the clock, the readings, the boot header.
struct Probe {
    virtual std::uint32_t now_ms() = 0;
    virtual bool power(PowerSample& s) = 0;                       // the latest readings, false when there are none
    virtual void beat(BeatInfo& b) = 0;
    virtual std::size_t boot_len() = 0;                           // the held boot header (0 when none)
    virtual void boot_read(std::size_t off, std::span<std::uint8_t> out) = 0;
    virtual std::string_view hash() = 0;
    virtual void boot(BootInfo& b) = 0;                           // boot id and reset cause, for the Boot frame

protected:
    ~Probe() = default;
};

struct Stats {
    std::uint32_t frames = 0;
    std::uint32_t trace_records = 0, trace_lost = 0;
    std::uint32_t log_entries = 0, log_lost = 0;
    std::uint32_t run_events = 0, run_lost = 0;
    std::uint32_t power_frames = 0, power_folded = 0, power_same = 0;
    std::uint32_t stalls = 0;   // passes with something waiting and no room in the FIFO
};

class Mux {
public:
    bool running() const { return running_; }
    std::uint8_t mask() const { return mask_; }
    std::uint16_t power_hz() const { return hz_; }

    Stats stats() const {
        Stats s = stats_;
        s.trace_records = trace_.stats().records;
        s.trace_lost = trace_.stats().lost;
        return s;
    }

    // Begin streaming `mask`'s kinds: from now, or with `history` from the oldest each ring still holds.
    template <std::size_t TN, std::size_t LN, std::size_t RN>
    void start(std::uint8_t mask, std::uint16_t power_hz, bool history, const pdtrace::Ring<TN>& tr, const BinLog<LN>& lg,
               const runev::Queue<RN>& rq, std::uint32_t now) {
        *this = Mux{};
        running_ = true;
        mask_ = mask;
        history_ = history;
        hz_ = power_hz < kPowerHzMin ? kPowerHzDefault : power_hz > kPowerHzMax ? kPowerHzMax : power_hz;
        if (mask & kTrace) trace_.start(tr, history);
        log_cursor_ = history ? lg.tail_abs() : lg.head_abs();
        log_seq_ = history ? lg.entries_dropped() : lg.entries_pushed();
        log_base_ = history ? lg.base_ms() : lg.last_ms();
        run_cursor_ = history ? rq.oldest() : rq.pushed();
        next_tick_ = now;
        beat_at_ = now - kBeatMs;   // the first Beat follows the boot header at once
    }
    void stop() {
        running_ = false;
        trace_.stop();
    }

    // One main-loop pass: whole frames to `write` while `room` (free bytes in the CDC TX FIFO) holds a worst-case frame, at
    // most kMaxFramesPerPass of them, never waiting. Returns the frames sent.
    template <std::size_t TN, std::size_t LN, std::size_t RN, class Write>
    unsigned service(const pdtrace::Ring<TN>& tr, const BinLog<LN>& lg, const runev::Queue<RN>& rq, Probe& pr, std::size_t room,
                     Write&& write) {
        if (!running_) return 0;
        const std::uint32_t now = pr.now_ms();
        if (mask_ & kPower) sample(now, pr);
        if (room < kFrameMax) {
            if (!hello_ || pending(tr, lg, rq) || ((mask_ & kPower) && send_due(now))) stats_.stalls++;
            return 0;
        }
        std::array<std::uint8_t, kFrameMax> f;
        unsigned n = 0;
        const auto can = [&] { return room >= kFrameMax && n < kMaxFramesPerPass; };
        const auto emit = [&](std::size_t len) {
            if (len == 0) return false;
            write(std::span<const std::uint8_t>(f.data(), len));
            room -= len;
            n++;
            stats_.frames++;
            return true;
        };

        // control: Hello, then the boot header; nothing else moves before it is out
        if (!hello_ && can()) {
            hello_ = emit(hello(f, now, pr));
        }
        if (hello_ && !boot_sent_ && can()) boot_sent_ = emit(boot_frame(f, pr));
        if (!hello_ || !boot_sent_) return n;
        for (const std::size_t total = pr.boot_len(); text_off_ < total && can();) {
            const std::size_t k = total - text_off_ < kTextChunk ? total - text_off_ : kTextChunk;
            trace_stream::put32(f.data() + 3, now);
            f[7] = 1;
            pr.boot_read(text_off_, std::span<std::uint8_t>(f.data() + 8, k));
            text_off_ += k;
            emit(trace_stream::seal_frame(f, Kind::Text, 5 + k));
        }
        if (text_off_ < pr.boot_len()) return n;

        if (mask_ & kRun)
            for (unsigned i = 0; i < kKindFramesPerPass && can(); i++)
                if (!emit(run_frame(f, rq))) break;
        if ((mask_ & kPower) && can() && send_due(now) && now - last_power_ms_ >= kPowerStaleMs) emit(power_frame(f, now));
        // trace and log take turns at going first, so a flooding trace cannot starve the log of the FIFO room
        for (int turn = 0; turn < 2; turn++) {
            if ((turn == 0) != log_first_) {
                if (mask_ & kTrace)
                    for (unsigned i = 0; i < kKindFramesPerPass && can(); i++)
                        if (!emit(trace_.next(tr, f))) break;
            } else if (mask_ & kLog) {
                for (unsigned i = 0; i < kKindFramesPerPass && can(); i++)
                    if (!emit(log_frame(f, lg))) break;
            }
        }
        log_first_ = !log_first_;
        if (can() && now - beat_at_ >= kBeatMs) {
            beat_at_ = now;
            emit(beat(f, now, pr));
        }
        if ((mask_ & kPower) && can() && send_due(now)) emit(power_frame(f, now));
        return n;
    }

private:
    struct Window {
        std::uint32_t first_ms = 0;
        std::uint16_t n = 0;
        std::uint16_t vmin = 0, vmax = 0;
        std::int16_t imin = 0, imax = 0;
        PowerSample last;
    };

    template <std::size_t TN, std::size_t LN, std::size_t RN>
    bool pending(const pdtrace::Ring<TN>& tr, const BinLog<LN>& lg, const runev::Queue<RN>& rq) const {
        return ((mask_ & kTrace) && trace_.pending(tr)) || ((mask_ & kLog) && log_cursor_ != lg.head_abs()) ||
               ((mask_ & kRun) && run_cursor_ != rq.pushed());
    }

    static int absdiff(int a, int b) { return a > b ? a - b : b - a; }
    static bool level_moved(std::uint16_t a, std::uint16_t b) {
        return (a == kUnread) != (b == kUnread) || (a != kUnread && absdiff(a, b) > kLevelDeadbandMv);
    }

    // The sampling tick (hz_): fold the newest reading into the window. Never sends.
    void sample(std::uint32_t now, Probe& pr) {
        if (static_cast<std::int32_t>(now - next_tick_) < 0) return;
        next_tick_ = now + 1000u / hz_;
        PowerSample s;
        if (!pr.power(s)) return;
        s.ms = now;
        const bool moved = !sent_any_ || s.valid != sent_.valid || absdiff(s.vbus, sent_.vbus) > kVbusDeadbandMv ||
                           absdiff(s.ibus, sent_.ibus) > kIbusDeadbandMa || level_moved(s.cc1, sent_.cc1) ||
                           level_moved(s.cc2, sent_.cc2) || level_moved(s.dp, sent_.dp) || level_moved(s.dm, sent_.dm);
        if (moved) dirty_ = true;
        if (win_.n == 0) {
            win_ = {now, 0, s.vbus, s.vbus, s.ibus, s.ibus, s};
        } else {
            if (s.vbus < win_.vmin) win_.vmin = s.vbus;
            if (s.vbus > win_.vmax) win_.vmax = s.vbus;
            if (s.ibus < win_.imin) win_.imin = s.ibus;
            if (s.ibus > win_.imax) win_.imax = s.ibus;
            win_.last = s;
            // a sample that joins a window which is already due folded instead of getting its own frame
            if (send_due(now)) stats_.power_folded++; else stats_.power_same++;
        }
        if (win_.n < 0xFFFF) win_.n++;
    }
    bool send_due(std::uint32_t now) const {
        return win_.n && (dirty_ || !sent_any_ || now - last_power_ms_ >= kKeepaliveMs);
    }

    std::size_t boot_frame(std::array<std::uint8_t, kFrameMax>& f, Probe& pr) const {
        BootInfo b;
        pr.boot(b);
        trace_stream::put16(f.data() + 3, b.boots);
        trace_stream::put32(f.data() + 5, b.reset_flags);
        return trace_stream::seal_frame(f, Kind::Boot, 6);
    }

    std::size_t hello(std::array<std::uint8_t, kFrameMax>& f, std::uint32_t now, Probe& pr) const {
        std::uint8_t* p = f.data() + 3;
        p[0] = kVersion;
        p[1] = mask_;
        trace_stream::put16(p + 2, (mask_ & kPower) ? hz_ : 0);
        trace_stream::put32(p + 4, now);
        p[8] = history_ ? 1 : 0;
        const std::string_view h = pr.hash();
        const std::size_t k = h.size() < kHashMax ? h.size() : kHashMax;
        for (std::size_t i = 0; i < k; i++) p[9 + i] = static_cast<std::uint8_t>(h[i]);
        return trace_stream::seal_frame(f, Kind::Hello, 9 + k);
    }

    template <std::size_t RN>
    std::size_t run_frame(std::array<std::uint8_t, kFrameMax>& f, const runev::Queue<RN>& rq) {
        std::uint8_t* p = f.data() + 3;
        if (run_cursor_ < rq.oldest()) {
            const std::uint32_t lost = rq.oldest() - run_cursor_;
            p[0] = static_cast<std::uint8_t>(Src::Run);
            trace_stream::put32(p + 1, lost);
            trace_stream::put32(p + 5, lost * static_cast<std::uint32_t>(sizeof(runev::Event)));
            p[9] = 0;
            run_cursor_ = rq.oldest();
            stats_.run_lost += lost;
            return trace_stream::seal_frame(f, Kind::Gap, 10);
        }
        if (run_cursor_ >= rq.pushed()) return 0;
        const runev::Event& e = rq.at(run_cursor_++);
        trace_stream::put32(p, e.ms);
        p[4] = static_cast<std::uint8_t>(e.code);
        p[5] = e.a;
        trace_stream::put16(p + 6, e.b);
        trace_stream::put16(p + 8, e.c);
        trace_stream::put16(p + 10, e.d);
        stats_.run_events++;
        return trace_stream::seal_frame(f, Kind::Run, 12);
    }

    template <std::size_t LN>
    std::size_t log_frame(std::array<std::uint8_t, kFrameMax>& f, const BinLog<LN>& lg) {
        std::uint8_t* p = f.data() + 3;
        if (log_cursor_ < lg.tail_abs()) {
            const std::uint32_t lost = lg.entries_dropped() - log_seq_;
            p[0] = static_cast<std::uint8_t>(Src::Log);
            trace_stream::put32(p + 1, lost);
            trace_stream::put32(p + 5, lg.tail_abs() - log_cursor_);
            p[9] = 0;
            log_cursor_ = lg.tail_abs();
            log_seq_ = lg.entries_dropped();
            log_base_ = lg.base_ms();
            stats_.log_lost += lost;
            return trace_stream::seal_frame(f, Kind::Gap, 10);
        }
        if (log_cursor_ >= lg.head_abs()) return 0;
        const std::size_t got = lg.read(log_cursor_, std::span<std::uint8_t>(p + 8, kLogChunk));
        if (got == 0) return 0;
        trace_stream::put32(p, log_base_);
        trace_stream::put32(p + 4, log_seq_);
        for (std::size_t i = 0; i < got; i += 1u + p[8 + i]) {
            log_base_ += BinLog<LN>::dt_of(p + 8 + i);
            log_seq_++;
            stats_.log_entries++;
        }
        log_cursor_ += static_cast<std::uint32_t>(got);
        return trace_stream::seal_frame(f, Kind::Log, 8 + got);
    }

    std::size_t power_frame(std::array<std::uint8_t, kFrameMax>& f, std::uint32_t now) {
        std::uint8_t* p = f.data() + 3;
        const PowerSample& s = win_.last;
        trace_stream::put32(p, s.ms);
        trace_stream::put16(p + 4, win_.n);
        p[6] = s.valid ? 1 : 0;
        trace_stream::put16(p + 7, s.vbus);
        trace_stream::put16(p + 9, static_cast<std::uint16_t>(s.ibus));
        trace_stream::put16(p + 11, s.cc1);
        trace_stream::put16(p + 13, s.cc2);
        trace_stream::put16(p + 15, s.dp);
        trace_stream::put16(p + 17, s.dm);
        std::size_t len = 19;
        if (win_.n > 1) {
            const std::uint32_t span = s.ms - win_.first_ms;
            trace_stream::put16(p + 19, static_cast<std::uint16_t>(span > 0xFFFF ? 0xFFFF : span));
            trace_stream::put16(p + 21, win_.vmin);
            trace_stream::put16(p + 23, win_.vmax);
            trace_stream::put16(p + 25, static_cast<std::uint16_t>(win_.imin));
            trace_stream::put16(p + 27, static_cast<std::uint16_t>(win_.imax));
            len = 29;
        }
        sent_ = s;
        sent_any_ = true;
        dirty_ = false;
        last_power_ms_ = now;
        win_ = {};
        stats_.power_frames++;
        return trace_stream::seal_frame(f, Kind::Power, len);
    }

    std::size_t beat(std::array<std::uint8_t, kFrameMax>& f, std::uint32_t now, Probe& pr) {
        BeatInfo b;
        pr.beat(b);
        const Stats s = stats();
        std::uint8_t* p = f.data() + 3;
        trace_stream::put32(p, now);
        trace_stream::put16(p + 4, b.stack_free);
        trace_stream::put16(p + 6, b.msp_free);
        p[8] = b.page;
        p[9] = b.flags;
        trace_stream::put32(p + 10, b.usb_dropped);
        const std::uint32_t c[9] = {s.trace_records, s.trace_lost, s.log_entries, s.log_lost, s.run_events,
                                    s.run_lost, s.power_frames, s.power_folded, s.stalls};
        for (std::size_t i = 0; i < 9; i++) trace_stream::put32(p + 14 + 4 * i, c[i]);
        return trace_stream::seal_frame(f, Kind::Beat, 50);
    }

    trace_stream::Streamer trace_;
    Stats stats_;
    Window win_;
    PowerSample sent_;
    std::uint32_t log_cursor_ = 0, log_seq_ = 0, log_base_ = 0, run_cursor_ = 0;
    std::uint32_t next_tick_ = 0, last_power_ms_ = 0, beat_at_ = 0;
    std::size_t text_off_ = 0;
    std::uint16_t hz_ = kPowerHzDefault;
    std::uint8_t mask_ = 0;
    bool running_ = false, history_ = false, hello_ = false, boot_sent_ = false, sent_any_ = false, dirty_ = false, log_first_ = false;
};

}  // namespace k2::dbg
