// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress result blob: everything a run measured, as bytes, read in chunks over the remote link or the shell. It carries the RAW
// material, not only the verdict: the whole fixed record (per sequence the transition count, every event counter and the latency /
// settle distributions) plus the firmware's own score broken down per event kind. A saved result can be re-scored on the host under
// other weights (tools/pd_stress.py rescore) without re-running the charger.
//
// Layout (little endian), kTotalBytes in all:
//   0  version        1  stop reason     2  sequence count   3  order length   4  reboots
//   5  flags (bit0 running, bit1 finished, bit2 release unconfirmed)            6  pace   7  0
//   8  options.mask (u16)   10 cycles_pct (u16)   12 max_transitions (u16)   14 transitions so far (u16)
//   16 options.max_ms (u32) 20 elapsed_ms (u32)   24 score, hundredths per 100 transitions (u32)
//   28 stamp-suspect count (u16)   30 words per sequence record (u16)
//   32 points per event kind (u32 x kEventCount): count x weight over the whole run
//   80 run order (u8 x kMaxSeqs): the Seq id at each position       92 per-sequence flags (u8 x kMaxSeqs)
//   104 the record: kMaxSeqs x kSeqWords int16 words, sequence slot = Seq id
// Per-sequence flags: bit0 done, bit1 skipped (not applicable or not selected), bit2 given up after reboots, bits 4..7 the Na reason.
//
// Nothing is stored: the header is built into a caller buffer and the record is streamed from the buffer the run already owns, so the
// blob costs no RAM beyond a chunk. Pure code; tests/test_pdstress_result.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pdstress/engine.hpp"

namespace k2::pdstress::result {

inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kEventsOff = 32;
inline constexpr std::size_t kOrderOff = kEventsOff + 4 * kEventCount;
inline constexpr std::size_t kFlagsOff = kOrderOff + kMaxSeqs;
inline constexpr std::size_t kHeaderBytes = 104;
inline constexpr std::size_t kRecordBytes = kRecordWords * 2;
inline constexpr std::size_t kTotalBytes = kHeaderBytes + kRecordBytes;
static_assert(kFlagsOff + kMaxSeqs == kHeaderBytes, "the header layout above");

enum Flag : std::uint8_t { kRunning = 1, kFinished = 2, kReleaseUnconfirmed = 4 };
enum SeqFlag : std::uint8_t { kSeqDone = 1, kSeqSkipped = 2, kSeqGivenUp = 4 };

// What the header needs from the run (the session fills it from the Engine).
struct Info {
    Options opt{};
    Stop stop = Stop::None;
    bool running = false;
    bool finished = false;
    bool release_unconfirmed = false;
    std::uint8_t n_order = 0;
    std::uint8_t reboots = 0;
    std::uint16_t transitions = 0;
    std::uint32_t elapsed_ms = 0;
    std::uint16_t stamp_suspect = 0;
    std::array<std::uint8_t, kMaxSeqs> order{};
    std::array<std::uint8_t, kMaxSeqs> seq_flags{};
};

namespace detail {
inline void put16(std::uint8_t* p, std::uint32_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
inline void put32(std::uint8_t* p, std::uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
}  // namespace detail

inline void build_header(const Info& i, const Record& rec, std::span<std::uint8_t, kHeaderBytes> h) {
    for (auto& b : h) b = 0;
    h[0] = kVersion;
    h[1] = static_cast<std::uint8_t>(i.stop);
    h[2] = static_cast<std::uint8_t>(kSeqCount);
    h[3] = i.n_order;
    h[4] = i.reboots;
    h[5] = std::uint8_t((i.running ? kRunning : 0) | (i.finished ? kFinished : 0) | (i.release_unconfirmed ? kReleaseUnconfirmed : 0));
    h[6] = i.opt.pace;
    detail::put16(&h[8], i.opt.mask);
    detail::put16(&h[10], i.opt.cycles_pct);
    detail::put16(&h[12], i.opt.max_transitions);
    detail::put16(&h[14], i.transitions);
    detail::put32(&h[16], i.opt.max_ms);
    detail::put32(&h[20], i.elapsed_ms);
    const Counts t = rec.total(kMaxSeqs);
    detail::put32(&h[24], score_cp(t));
    detail::put16(&h[28], i.stamp_suspect);
    detail::put16(&h[30], kSeqWords);
    for (std::size_t e = 0; e < kEventCount; ++e) detail::put32(&h[kEventsOff + 4 * e], event_points(t, Event(e)));
    for (std::size_t s = 0; s < kMaxSeqs; ++s) {
        h[kOrderOff + s] = i.order[s];
        h[kFlagsOff + s] = i.seq_flags[s];
    }
}

// Copies bytes [off, off + out.size()) of the blob into `out`; returns the count (0 at or past the end).
inline std::size_t read(const Info& i, const Record& rec, std::size_t off, std::span<std::uint8_t> out) {
    if (off >= kTotalBytes) return 0;
    std::size_t n = kTotalBytes - off;
    if (n > out.size()) n = out.size();
    std::array<std::uint8_t, kHeaderBytes> h{};
    if (off < kHeaderBytes) build_header(i, rec, h);
    const auto w = rec.words();
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t p = off + k;
        if (p < kHeaderBytes) {
            out[k] = h[p];
        } else {
            const std::size_t q = p - kHeaderBytes;
            const std::uint16_t v = static_cast<std::uint16_t>(w[q >> 1]);
            out[k] = (q & 1) ? std::uint8_t(v >> 8) : std::uint8_t(v);
        }
    }
    return n;
}

}  // namespace k2::pdstress::result
