// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The direct-plug line fallback (pd::LineFallback): our single Rd sits on a line that shows the source's Rp, the other line
// showed an Rp too (a direct plug, both receptacle lines pulled up) and VBUS has not come up: move the Rd to the other
// line on a schedule (1.5 s, 1.5 s, 5 s, 5 s), then stay. Pure timing; the wire sequence is in test_fusb302_scan.cpp.
#include "test_util.h"
#include "k2/pd/pd_rdseek.hpp"

using namespace k2;

namespace {

// One poll every 20 ms with the given inputs. Returns the ms (from t0) at which it said "move", or -1 within span_ms.
struct In { bool eligible = true, rp = true, dark = true, other = true; };
int first_move(pd::LineFallback& f, std::uint32_t t0, std::uint32_t span_ms, In in = {}) {
    for (std::uint32_t t = 0; t <= span_ms; t += 20)
        if (f.update(t0 + t, in.eligible, in.rp, in.dark, in.other)) return int(t);
    return -1;
}

void test_a_dark_first_line_on_a_direct_plug_moves_at_the_first_dwell() {
    pd::LineFallback f;
    const int at = first_move(f, 1000, 3000);
    CHECK_EQ(at >= int(pd::kLineTrialMs[0]), true);
    CHECK_EQ(at <= int(pd::kLineTrialMs[0]) + 100, true);   // + the one-line-quiet settle of the other line's reading
    CHECK_EQ(int(f.moves()), 1);
}

void test_the_whole_schedule_is_1500_1500_5000_5000_then_stay() {
    pd::LineFallback f;
    std::uint32_t t = 0;
    int when[8] = {0};
    int n = 0;
    for (; t < 30000; t += 20)
        if (f.update(t, true, true, true, true) && n < 8) when[n++] = int(t);
    CHECK_EQ(n, 3);                                           // three moves: A -> B -> A -> B, the fourth dwell stays
    CHECK_EQ(int(f.moves()), 3);
    CHECK_EQ(when[0] >= 1500 && when[0] <= 1600, true);
    CHECK_EQ(when[1] - when[0] >= 1500 && when[1] - when[0] <= 1600, true);
    CHECK_EQ(when[2] - when[1] >= 5000 && when[2] - when[1] <= 5100, true);
    CHECK_EQ(f.live(30000), false);                           // exhausted: the hints take the footer back
}

void test_vbus_up_on_the_second_line_stays_and_no_further_move() {
    pd::LineFallback f;
    std::uint32_t t = 0;
    int moved_at = -1;
    for (; t < 3300 && moved_at < 0; t += 20) if (f.update(t, true, true, true, true)) moved_at = int(t);
    CHECK_EQ(moved_at >= 0, true);
    // the charger woke on the second line: VBUS comes up
    for (; t < 40000; t += 20) CHECK_EQ(f.update(t, true, true, false, true), false);
    CHECK_EQ(int(f.moves()), 1);
    CHECK_EQ(f.live(t), false);
    // even if VBUS sags again later in the same attach (a source reset), the line is the source's: still no move
    for (std::uint32_t k = 0; k < 20000; k += 20, t += 20) CHECK_EQ(f.update(t, true, true, true, true), false);
}

void test_a_slow_waker_is_found_on_the_second_line_in_the_longer_pass() {
    // The first line never answers; the charger needs 4 s on the second line (pattern slow-rp-wake): the 1.5 s dwells would
    // miss it, the second pass (first line 5 s, other line 5 s) gives it the time. VBUS rises 4 s after the Rd landed on B.
    pd::LineFallback f;
    std::uint32_t t = 0;
    int line = 1;                                              // the line our Rd is on
    std::uint32_t on_b_since = 0;
    bool woke = false;
    for (; t < 60000 && !woke; t += 20) {
        const bool vbus_up = line == 2 && on_b_since != 0 && t - on_b_since >= 4000;
        if (vbus_up) { woke = true; break; }
        if (f.update(t, true, true, true, true)) { line = line == 1 ? 2 : 1; on_b_since = line == 2 ? t : 0; }
    }
    // dwell 1 (A, 1.5 s), dwell 2 (B, 1.5 s: too short, the waker needs 4 s), dwell 3 (A, 5 s), dwell 4 (B, 5 s: found)
    CHECK_EQ(woke, true);
    CHECK_EQ(line, 2);
    CHECK_EQ(int(f.moves()), 3);
}

void test_a_one_line_cable_never_moves_and_never_claims_the_screen() {
    pd::LineFallback f;
    In in; in.other = false;                                   // the other line floats: no direct plug
    CHECK_EQ(first_move(f, 0, 30000, in), -1);
    CHECK_EQ(int(f.moves()), 0);
    CHECK_EQ(f.live(30000), false);                            // the old "No VBUS. Hold + few secs" hint stays
}

void test_a_spike_on_the_other_line_is_not_a_direct_plug() {
    pd::LineFallback f;
    std::uint32_t t = 0;
    for (; t < 1480; t += 20) f.update(t, true, true, true, false);
    CHECK_EQ(f.update(t, true, true, true, true), false);      // one tick high at the end of the dwell ...
    t += 20;
    CHECK_EQ(f.update(t, true, true, true, false), false);     // ... and gone again: no move
    CHECK_EQ(int(f.moves()), 0);
}

void test_a_pinned_or_ineligible_line_never_moves() {
    pd::LineFallback f;
    In in; in.eligible = false;                                // key mode PLUS/OK, pd-reread --line, Ra mode, inline, PD seen
    CHECK_EQ(first_move(f, 0, 30000, in), -1);
    CHECK_EQ(int(f.moves()), 0);
    CHECK_EQ(f.live(30000), false);
    // eligibility returns: the dwell starts now, not from the start of the silence
    const int at = first_move(f, 40000, 3000);
    CHECK_EQ(at >= int(pd::kLineTrialMs[0]), true);
}

void test_a_line_without_an_rp_is_the_old_seeks_business() {
    pd::LineFallback f;
    In in; in.rp = false;                                      // our Rd on a line that shows no Rp (RdSeek moves it)
    CHECK_EQ(first_move(f, 0, 30000, in), -1);
    CHECK_EQ(int(f.moves()), 0);
}

void test_vbus_up_before_the_dwell_ends_stays_and_never_moves() {
    pd::LineFallback f;
    std::uint32_t t = 0;
    for (; t < 1000; t += 20) CHECK_EQ(f.update(t, true, true, true, true), false);
    for (; t < 30000; t += 20) CHECK_EQ(f.update(t, true, true, false, true), false);   // the first line woke
    CHECK_EQ(int(f.moves()), 0);
}

void test_live_is_true_only_while_a_direct_plug_is_being_tried() {
    pd::LineFallback f;
    CHECK_EQ(f.live(0), false);                                // nothing known yet
    std::uint32_t t = 0;
    for (; t < 200; t += 20) f.update(t, true, true, true, true);
    CHECK_EQ(f.live(t), true);                                 // armed, direct plug seen, dwell running
    for (; t < 1700; t += 20) f.update(t, true, true, true, true);
    CHECK_EQ(int(f.moves()), 1);
    CHECK_EQ(f.live(t), true);                                 // a dwell on the second line
    f.reset();
    CHECK_EQ(f.live(t), false);
    CHECK_EQ(int(f.moves()), 0);
}

void test_a_fresh_attach_restores_the_schedule() {
    pd::LineFallback f;
    first_move(f, 0, 3000);
    CHECK_EQ(int(f.moves()), 1);
    f.reset();
    CHECK_EQ(int(f.moves()), 0);
    CHECK_EQ(first_move(f, 100000, 3000) >= int(pd::kLineTrialMs[0]), true);
}

void test_the_idle_wake_already_alternates_a_direct_plug_one_line_at_a_time() {
    // Wake-sleeping-port covers a direct plug with its own hold: both lines read the open signature, the hold alternates line
    // by line every dwell and is never on both (the dark-attach release hands the line back to it).
    pd::IdleWake w;
    w.set_enabled(true);
    std::uint32_t t = 0;
    const std::uint8_t first = w.update(t, 3300, 3300);
    CHECK_EQ(first == 1 || first == 2, true);
    t += pd::kSlowWakeDwellMs + 20;
    const std::uint8_t second = w.update(t, 3300, 3300);
    CHECK_EQ(second != 0 && second != first, true);
}

}  // namespace

int main() {
    test_a_dark_first_line_on_a_direct_plug_moves_at_the_first_dwell();
    test_the_whole_schedule_is_1500_1500_5000_5000_then_stay();
    test_vbus_up_on_the_second_line_stays_and_no_further_move();
    test_a_slow_waker_is_found_on_the_second_line_in_the_longer_pass();
    test_a_one_line_cable_never_moves_and_never_claims_the_screen();
    test_a_spike_on_the_other_line_is_not_a_direct_plug();
    test_a_pinned_or_ineligible_line_never_moves();
    test_a_line_without_an_rp_is_the_old_seeks_business();
    test_vbus_up_before_the_dwell_ends_stays_and_never_moves();
    test_live_is_true_only_while_a_direct_plug_is_being_tried();
    test_a_fresh_attach_restores_the_schedule();
    test_the_idle_wake_already_alternates_a_direct_plug_one_line_at_a_time();
    return test_report("test_linefallback");
}
