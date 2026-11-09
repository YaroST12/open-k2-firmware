// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Rd seek: our own Rd went on the line a passive scan picked (no Rd then), and no Rp answers it.
// Pure timing and budget; the wire sequence is in test_fusb302_scan.cpp.
#include "test_util.h"
#include "k2/pd/pd_rdseek.hpp"

using namespace k2;

namespace {

// A line that never answers: the poll sees no Rp every 20 ms. Returns the ms at which the seek said "move", or -1.
int first_move(pd::RdSeek& s, std::uint32_t t0, std::uint32_t span_ms) {
    for (std::uint32_t t = 0; t <= span_ms; t += 20)
        if (s.update(t0 + t, true, false)) return int(t);
    return -1;
}

void test_a_silent_line_moves_after_the_dwell_and_not_before() {
    pd::RdSeek s;
    const int at = first_move(s, 1000, 1000);
    CHECK_EQ(at >= int(pd::kRdSeekDwellMs), true);
    CHECK_EQ(at <= int(pd::kRdSeekDwellMs) + 20, true);
    CHECK_EQ(int(s.moves()), 1);
}

void test_an_answering_rp_restarts_the_dwell() {
    pd::RdSeek s;
    std::uint32_t t = 5000;
    for (int i = 0; i < 8; i++, t += 20) CHECK_EQ(s.update(t, true, false), false);     // 160 ms of silence
    CHECK_EQ(s.update(t, true, true), false);                                            // the Rp shows up
    t += 20;
    for (int i = 0; i < 8; i++, t += 20) CHECK_EQ(s.update(t, true, false), false);     // silence again: the clock started over
    CHECK_EQ(int(s.moves()), 0);
}

void test_an_ineligible_line_never_moves_and_forgets_the_silence() {
    pd::RdSeek s;
    std::uint32_t t = 0;
    for (int i = 0; i < 50; i++, t += 20) CHECK_EQ(s.update(t, false, false), false);   // not our Rd (inline, pinned, Ra, PD seen)
    CHECK_EQ(s.update(t, true, false), false);                                           // eligibility returns: dwell starts now
    CHECK_EQ(first_move(s, t + 20, 1000) >= int(pd::kRdSeekDwellMs) - 20, true);
}

void test_the_budget_is_bounded_and_a_fresh_attach_restores_it() {
    pd::RdSeek s;
    std::uint32_t t = 0;
    int moves = 0;
    for (int i = 0; i < 400; i++, t += 20) moves += s.update(t, true, false) ? 1 : 0;   // 8 s of a line that never answers
    CHECK_EQ(moves, int(pd::kRdSeekMoves));
    CHECK_EQ(int(s.moves()), int(pd::kRdSeekMoves));
    s.reset();
    CHECK_EQ(int(s.moves()), 0);
    CHECK_EQ(first_move(s, t, 1000) >= 0, true);
}

void test_the_whole_budget_fits_in_about_a_second_with_the_detach_after_it() {
    // dwell x moves, then the DetachTracker's own 500 ms on the last line: the give-up lands near a real sink's debounce plus
    // the source's VBUS-on, not at the old single 500 ms, and not beyond ~1.2 s.
    const std::uint32_t total = pd::kRdSeekDwellMs * pd::kRdSeekMoves + 500;
    CHECK_EQ(total > 500, true);
    CHECK_EQ(total <= 1200, true);
}

}  // namespace

int main() {
    test_a_silent_line_moves_after_the_dwell_and_not_before();
    test_an_answering_rp_restarts_the_dwell();
    test_an_ineligible_line_never_moves_and_forgets_the_silence();
    test_the_budget_is_bounded_and_a_fresh_attach_restores_it();
    test_the_whole_budget_fits_in_about_a_second_with_the_detach_after_it();
    return test_report("test_rdseek");
}
