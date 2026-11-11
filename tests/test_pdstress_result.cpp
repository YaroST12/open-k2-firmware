// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress result blob (pdstress/result.hpp): the raw material of a run, so a run can be re-scored on the host later.
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "k2/pdstress/result.hpp"
#include "test_util.h"

using namespace k2::pdstress;

namespace {
std::uint32_t rd32(const std::uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24; }
std::uint16_t rd16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }

std::array<std::int16_t, 640> g_buf{};

Record fresh() {
    Record r{std::span<std::int16_t>(g_buf)};
    r.clear();
    return r;
}

std::vector<std::uint8_t> read_all(const result::Info& i, const Record& r, std::size_t chunk) {
    std::vector<std::uint8_t> out;
    std::array<std::uint8_t, 64> b{};
    for (std::size_t off = 0; off < result::kTotalBytes;) {
        const std::size_t n = result::read(i, r, off, std::span<std::uint8_t>(b.data(), chunk));
        if (n == 0) break;
        out.insert(out.end(), b.begin(), b.begin() + n);
        off += n;
    }
    return out;
}
}  // namespace

static void header_carries_the_run() {
    Record r = fresh();
    bump_transition(r.rec(1));
    bump_transition(r.rec(1));
    bump_event(r.rec(1), Event::WaitRx);
    bump_event(r.rec(2), Event::HardResetRx);
    result::Info i;
    i.stop = Stop::Done; i.finished = true; i.release_unconfirmed = true;
    i.n_order = 3; i.reboots = 1; i.order[0] = 1; i.order[1] = 2; i.order[2] = 0;
    i.opt.mask = 0x0123; i.opt.cycles_pct = 150; i.opt.pace = 2; i.opt.max_ms = 123456; i.opt.max_transitions = 777;
    i.transitions = 2; i.elapsed_ms = 98765; i.stamp_suspect = 4;
    i.seq_flags[1] = 0x01 | 0x40;
    std::array<std::uint8_t, result::kHeaderBytes> h{};
    result::build_header(i, r, h);
    CHECK_EQ(h[0], result::kVersion);
    CHECK_EQ(h[1], static_cast<std::uint8_t>(Stop::Done));
    CHECK_EQ(h[2], kSeqCount);
    CHECK_EQ(h[3], 3);
    CHECK_EQ(h[4], 1);
    CHECK_EQ(h[5], 0x02 | 0x04);   // finished, release unconfirmed; not running
    CHECK_EQ(h[6], 2);
    CHECK_EQ(rd16(&h[8]), 0x0123);
    CHECK_EQ(rd16(&h[10]), 150);
    CHECK_EQ(rd16(&h[12]), 777);
    CHECK_EQ(rd16(&h[14]), 2);
    CHECK_EQ(rd32(&h[16]), 123456u);
    CHECK_EQ(rd32(&h[20]), 98765u);
    CHECK_EQ(rd16(&h[28]), 4);
    CHECK_EQ(rd16(&h[30]), kSeqWords);
    // the firmware's own score, per event: points and the total in hundredths per 100 transitions
    const Counts t = r.total(kMaxSeqs);
    CHECK_EQ(rd32(&h[24]), score_cp(t));
    CHECK_EQ(rd32(&h[32 + 4 * static_cast<std::size_t>(Event::WaitRx)]), 2u);
    CHECK_EQ(rd32(&h[32 + 4 * static_cast<std::size_t>(Event::HardResetRx)]), 20u);
    CHECK_EQ(h[result::kOrderOff + 0], 1);
    CHECK_EQ(h[result::kOrderOff + 2], 0);
    CHECK_EQ(h[result::kFlagsOff + 1], 0x41);
}

static void record_follows_header_word_for_word() {
    Record r = fresh();
    add_sample(r.rec(0), Metric::Accept, 83);
    bump_event(r.rec(5), Event::VbusBand, 3);
    result::Info i;
    const auto all = read_all(i, r, 53);
    CHECK_EQ(all.size(), result::kTotalBytes);
    // word k of the record is bytes kHeaderBytes + 2k, little endian
    for (std::size_t k = 0; k < kRecordWords; ++k)
        if (rd16(&all[result::kHeaderBytes + 2 * k]) != std::uint16_t(g_buf[k])) { CHECK_COND(false); break; }
    const std::size_t w = 5 * kSeqWords + 1 + static_cast<std::size_t>(Event::VbusBand);
    CHECK_EQ(rd16(&all[result::kHeaderBytes + 2 * w]), 3);
}

static void chunks_of_any_size_read_the_same_bytes() {
    Record r = fresh();
    bump_event(r.rec(3), Event::SoftResetRx, 7);
    result::Info i;
    i.n_order = 2; i.order[0] = 3; i.order[1] = 9;
    const auto a = read_all(i, r, 53);
    const auto b = read_all(i, r, 7);
    const auto c = read_all(i, r, 1);
    CHECK_COND(a == b);
    CHECK_COND(a == c);
    std::array<std::uint8_t, 8> tail{};
    CHECK_EQ(result::read(i, r, result::kTotalBytes, tail), size_t(0));
    CHECK_EQ(result::read(i, r, result::kTotalBytes - 3, tail), size_t(3));
}

static void total_is_header_plus_record() {
    CHECK_EQ(result::kTotalBytes, result::kHeaderBytes + kMaxSeqs * kSeqWords * 2);
    CHECK_COND(result::kOrderOff + kMaxSeqs <= result::kFlagsOff);
    CHECK_COND(result::kFlagsOff + kMaxSeqs <= result::kHeaderBytes);
}

int main() {
    header_carries_the_run();
    record_follows_header_word_for_word();
    chunks_of_any_size_read_the_same_bytes();
    total_is_header_plus_record();
    return test_report("pdstress_result");
}
