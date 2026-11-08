// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the USB IN report queue: log lines must never evict a queued screenshot frame.
#include <array>
#include <cstdint>

#include "test_util.h"
#include "k2/core/tx_queue.hpp"

using namespace k2;

namespace {

using Q = TxQueue<16, 4>;
std::array<std::uint8_t, 4> rep(std::uint8_t v) { return {v, v, v, v}; }

void test_log_never_evicts_frames() {
    Q q;
    for (int i = 0; i < 15; i++) CHECK_EQ(q.push_report(rep(std::uint8_t(0x80 + i))), 1);   // 15 frames fill it
    CHECK_EQ(q.free(), 0);
    // A burst of log lines arrives while the frames wait for the host: they are dropped, the frames stay.
    for (int i = 0; i < 5; i++) CHECK_EQ(q.push_log(rep(0x5B)), 0);
    CHECK_EQ(q.dropped(), 5);
    for (int i = 0; i < 15; i++) {
        const std::uint8_t *p = q.front();
        CHECK_EQ(p != nullptr, 1);
        CHECK_EQ(p[0], 0x80 + i);   // every frame, in order
        q.pop();
    }
    CHECK_EQ(q.front() == nullptr, 1);
}

void test_logs_fit_while_there_is_room() {
    Q q;
    CHECK_EQ(q.push_log(rep(1)), 1);
    CHECK_EQ(q.push_log(rep(2)), 1);
    CHECK_EQ(q.free(), 13);
    CHECK_EQ(q.front()[0], 1);
    q.pop();
    CHECK_EQ(q.front()[0], 2);
}

void test_report_on_full_queue_drops_oldest() {   // the caller checks free(); this is the last resort
    Q q;
    for (int i = 0; i < 15; i++) q.push_report(rep(std::uint8_t(i)));
    CHECK_EQ(q.push_report(rep(99)), 1);
    CHECK_EQ(q.dropped(), 1);
    CHECK_EQ(q.front()[0], 1);
}

}  // namespace

int main() {
    test_log_never_evicts_frames();
    test_logs_fit_while_there_is_room();
    test_report_on_full_queue_drops_oldest();
    return test_report("test_tx_queue");
}
