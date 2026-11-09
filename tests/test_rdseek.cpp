// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Rd seek: our own Rd went on the line a passive scan picked (no Rd then), and no Rp answers it.
// Pure timing and budget; the wire sequence is in test_fusb302_scan.cpp.
#include "test_util.h"
#include "k2/pd/pd_rdseek.hpp"

using namespace k2;

namespace {

// A line that never answers: the poll sees no Rp every 20 ms. Returns the ms at which the seek said "move", or -1.
int first_move(pd::RdSeek& s, std::uint32_t t0, std::uint32_t span_ms, std::uint8_t line = 1) {
    for (std::uint32_t t = 0; t <= span_ms; t += 20)
        if (s.update(t0 + t, line, true, false)) return int(t);
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
    for (int i = 0; i < 8; i++, t += 20) CHECK_EQ(s.update(t, 1, true, false), false);     // 160 ms of silence
    CHECK_EQ(s.update(t, 1, true, true), false);                                            // the Rp shows up
    t += 20;
    for (int i = 0; i < 8; i++, t += 20) CHECK_EQ(s.update(t, 1, true, false), false);     // silence again: the clock started over
    CHECK_EQ(int(s.moves()), 0);
}

void test_an_ineligible_line_never_moves_and_forgets_the_silence() {
    pd::RdSeek s;
    std::uint32_t t = 0;
    for (int i = 0; i < 50; i++, t += 20) CHECK_EQ(s.update(t, 1, false, false), false);   // not our Rd (inline, pinned, Ra, PD seen)
    CHECK_EQ(s.update(t, 1, true, false), false);                                           // eligibility returns: dwell starts now
    CHECK_EQ(first_move(s, t + 20, 1000) >= int(pd::kRdSeekDwellMs) - 20, true);
}

void test_the_budget_is_bounded_and_a_fresh_attach_restores_it() {
    pd::RdSeek s;
    std::uint32_t t = 0;
    int moves = 0;
    for (int i = 0; i < 400; i++, t += 20) moves += s.update(t, 1, true, false) ? 1 : 0;   // 8 s of a line that never answers
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

// ---- pattern slow-rp-wake (2026-11-09), red first ----

// Standby signature on CC2 (open ~3.0 V, collapsing under our Rd), the Rp shows after 1100 ms: no move before
// kSlowWakeDwellMs - the seek stays on CC2 and the attach lands there (the splitter's Rp came 1072 ms after the Rd).
void test_a_standby_signature_line_gets_the_slow_dwell_and_no_move_before_the_rp() {
    pd::RdSeek s;
    s.note_open(1, 3300); s.note_open(2, 3050);   // both lines read the standby pull-up while open
    std::uint32_t t = 1000;
    for (int i = 0; i < 55; i++, t += 20) CHECK_EQ(s.update(t, 2, true, false), false);   // 1100 ms of no Rp under the Rd
    CHECK_EQ(int(s.moves()), 0);                    // 1100 ms > kRdSeekDwellMs, but the standby line gets kSlowWakeDwellMs
    CHECK_EQ(s.update(t, 2, true, true), false);    // the Rp at 1100 ms: attach on CC2, never a move
    CHECK_EQ(int(s.moves()), 0);
    // and the 500 ms no-Rp detach is held off inside the window:
    CHECK_EQ(s.holding_slow(t - 20, 2), true);
    CHECK_EQ(s.holding_slow(pd::kSlowWakeDwellMs + t, 2), false);   // the window ends with the dwell
}
// No standby signature on either line (a coupled net, nothing): the moves stay at the unchanged 200 ms dwell.
void test_a_line_without_the_signature_keeps_the_200ms_moves() {
    pd::RdSeek s;
    s.note_open(1, 1200); s.note_open(2, 0);   // neither line reads the standby pull-up
    const int at = first_move(s, 1000, 1000, 1);
    CHECK_EQ(at >= int(pd::kRdSeekDwellMs), true);
    CHECK_EQ(at <= int(pd::kRdSeekDwellMs) + 20, true);
    CHECK_EQ(s.holding_slow(1000 + at, 1), false);
}
// Standby on CC1 while the pick was CC2: the seek goes to CC1 first - a move at the first silent poll, no 200 ms dwell.
void test_a_standby_on_the_other_line_moves_there_first() {
    pd::RdSeek s;
    s.note_open(1, 3100);   // CC1 read the standby signature open
    s.note_open(2, 900);    // the picked CC2 did not
    CHECK_EQ(s.update(1000, 2, true, false), true);   // go to CC1 at once
    CHECK_EQ(int(s.moves()), 1);
    // and once on CC1 the slow dwell applies there:
    std::uint32_t t = 1020;
    for (int i = 0; i < 50; i++, t += 20) CHECK_EQ(s.update(t, 1, true, false), false);   // 1000 ms on CC1
    CHECK_EQ(int(s.moves()), 1);   // still no second move before kSlowWakeDwellMs
}

}  // namespace

int main() {
    test_a_silent_line_moves_after_the_dwell_and_not_before();
    test_an_answering_rp_restarts_the_dwell();
    test_an_ineligible_line_never_moves_and_forgets_the_silence();
    test_the_budget_is_bounded_and_a_fresh_attach_restores_it();
    test_the_whole_budget_fits_in_about_a_second_with_the_detach_after_it();
    test_a_standby_signature_line_gets_the_slow_dwell_and_no_move_before_the_rp();
    test_a_line_without_the_signature_keeps_the_200ms_moves();
    test_a_standby_on_the_other_line_moves_there_first();
    return test_report("test_rdseek");
}
