// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger-test run events for the live debug stream (docs/DEBUG_STREAMING.md): stage changes, row start and end with the
// verdict and reason, unit attach/detach, identity arm/disarm, suspend/resume. Each is one 12 byte record in a small ring
// that the main loop writes (ui_charger_test.cpp) and the stream reads (k2/core/dbg_stream.hpp), so nothing here runs in an ISR.
//
// Like the other stream sources the ring is the queue: a reader keeps an absolute event number, and one that fell behind
// finds how many events were overwritten (oldest() - cursor) and reports them as a Gap frame, never silently.
// With no stream running, emit() still costs one 12 byte store, so the page needs no "is anyone listening" flag.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace k2::runev {

// The meaning of a, b, c, d per code (all unsigned, little endian on the wire):
enum class Code : std::uint8_t {
    Start = 1,     // a = unit count, b = rows planned, c = 0 (a resumed run says Resume instead)
    Stage = 2,     // a = ui stage
    RowStart = 3,  // a = row index, b = group, c = algorithm, d = position in the group
    RowEnd = 4,    // a = row index, b = verdict, c = reason, d = measured mV
    RowEnd2 = 5,   // a = row index, b = tries, c = requested mV, d = requested mA
    Attach = 6,    // a = session mode (0 PD, 1 legacy)
    Detach = 7,    // a = 1 when the row asked for it
    Hold = 8,      // a = session mode
    Identity = 9,  // a = 1 armed, 0 disarmed
    Suspend = 10,  // a = row index (a charger attached while a run was open)
    Resume = 11,   // a = row index
    End = 12,      // a = final state (Done/Stopped), b = rows finished, c = reason
    Phase = 13,    // a = phase after a PD event the page saw
    RowSkips = 14, // rows a..b (contiguous) all skipped for reason c, d = how many rows; replaces one RowEnd per skipped row
};

struct Event {
    std::uint32_t ms;
    Code code;
    std::uint8_t a;
    std::uint16_t b, c, d;
};
static_assert(sizeof(Event) == 12);

template <std::size_t N>
class Queue {
public:
    void push(const Event& e) {
        buf_[pushed_ % N] = e;
        pushed_++;
    }
    std::uint32_t pushed() const { return pushed_; }
    // The number of the oldest event still held.
    std::uint32_t oldest() const { return pushed_ > N ? pushed_ - static_cast<std::uint32_t>(N) : 0; }
    // The event numbered `n` (oldest() <= n < pushed()).
    const Event& at(std::uint32_t n) const { return buf_[n % N]; }

private:
    std::array<Event, N> buf_{};
    std::uint32_t pushed_ = 0;
};

// A failure skips every row behind it, up to ~55 in one pass: more than the ring holds. Rows that are skipped one after the
// other for the same reason are therefore ONE event (RowSkips), held back until the range is closed (a row with another reason
// or not next in line, any other event, or flush()). Nothing is lost: the range says which rows and how many.
class SkipRun {
public:
    // A row was skipped. Closes the open range into `q` when this row does not extend it. True when it opened a new range.
    template <std::size_t N>
    bool add(Queue<N>& q, std::uint32_t now_ms, std::uint16_t row, std::uint16_t reason) {
        if (open_ && reason == reason_ && row == static_cast<std::uint16_t>(last_ + 1)) {
            last_ = row;
            count_++;
            return false;
        }
        flush(q);
        open_ = true;
        ms_ = now_ms; first_ = last_ = row; reason_ = reason; count_ = 1;
        return true;
    }
    template <std::size_t N>
    void flush(Queue<N>& q) {
        if (!open_) return;
        open_ = false;
        q.push({ms_, Code::RowSkips, static_cast<std::uint8_t>(first_ > 255 ? 255 : first_), last_, reason_, count_});
    }
    bool open() const { return open_; }

private:
    bool open_ = false;
    std::uint32_t ms_ = 0;
    std::uint16_t first_ = 0, last_ = 0, reason_ = 0, count_ = 0;
};

inline constexpr std::size_t kQueueEvents = 16;   // 192 bytes of .bss; a row emits at most three, a stream drains it every pass; a failure's skip rows are one RowSkips event per reason, so the ring holds a whole run's burst
using LiveQueue = Queue<kQueueEvents>;

inline LiveQueue g_live;
inline SkipRun g_skips;
inline const LiveQueue& live() { return g_live; }

// Any other event closes the open skip range first, so the stream keeps the order the page saw.
inline void emit(Code c, std::uint32_t now_ms, std::uint8_t a = 0, std::uint16_t b = 0, std::uint16_t cc = 0,
                 std::uint16_t d = 0) {
    g_skips.flush(g_live);
    g_live.push({now_ms, c, a, b, cc, d});
}
// A skipped row; true when it opened a new range (the page logs one line per range, not per row).
inline bool skip(std::uint32_t now_ms, std::uint16_t row, std::uint16_t reason) { return g_skips.add(g_live, now_ms, row, reason); }
inline void flush() { g_skips.flush(g_live); }

}  // namespace k2::runev
