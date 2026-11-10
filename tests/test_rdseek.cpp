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
    CHECK_EQ(s.holding_slow(t, 2), true);           // inside the window the 500 ms no-Rp detach is held off
    CHECK_EQ(s.update(t, 2, true, true), false);    // the Rp at 1100 ms: attach on CC2, never a move
    CHECK_EQ(int(s.moves()), 0);
    CHECK_EQ(s.holding_slow(t, 2), false);          // the Rp ended the window
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

// ---- pattern slow-rp-wake, idle path (2026-11-10), red first ----

// The idle/no-source path: an idle K2 with no source detected and no Rd of its own presents none, so a sleeping
// source (the splitter's port: one CC line reading the standby signature open, VBUS 78 mV, "No source") is never
// asked and never wakes - only a pinned `pd-reread --pin 2 --no-run` woke it (be3f589). The fix: a line with the
// standby signature open is treated as a sleeping source and our Rd is held on that ONE line for kSlowWakeDwellMs
// at a time (pd::IdleWake), so the ~2 Hz scan attaches on it once its Rp comes (1072 ms after the Rd, measured).
void test_the_idle_wake_holds_rd_on_a_standby_signature_line() {
    pd::IdleWake w;
    w.set_enabled(true);
    CHECK_EQ(int(w.update(1000, 0, 3050)), 2);                    // CC2 read the standby pull-up open: Rd on CC2
    CHECK_EQ(int(w.update(1020, 0, 3050)), 2);                      // held through the dwell
    CHECK_EQ(int(w.update(1000 + pd::kSlowWakeDwellMs - 1, 0, 3050)), 2);
    CHECK_EQ(int(w.update(1000 + pd::kSlowWakeDwellMs, 0, 3050)), 2);  // the dwell ran out: re-armed at once, still one line
    CHECK_EQ(int(w.line()), 2);
    CHECK_EQ(w.line() == 1 || w.line() == 2, true);                  // never both lines
}
// No standby signature (a coupled net reads less, nothing attached reads low/unknown): no Rd is held at all.
void test_the_idle_wake_holds_nothing_without_the_signature() {
    pd::IdleWake w;
    w.set_enabled(true);
    CHECK_EQ(int(w.update(0, -1, -1)), 0);       // unknown levels
    CHECK_EQ(int(w.update(20, 1200, 0)), 0);     // a coupled net (the PLUS key on CC2) reads below kStandbyOpenMv
    CHECK_EQ(int(w.update(40, 2799, 0)), 0);     // just below the signature
    CHECK_EQ(int(w.update(60, 2800, 0)), 1);     // the signature boundary: Rd on CC1
    CHECK_EQ(int(w.line()), 1);
}
// Both lines read the signature: the hold alternates line by line every dwell, never both (Debug Accessory).
void test_the_idle_wake_alternates_when_both_lines_read_the_signature() {
    pd::IdleWake w;
    w.set_enabled(true);
    std::uint32_t t = 1000;
    CHECK_EQ(int(w.update(t, 3100, 3000)), 1);                          // the higher open reading first
    t += pd::kSlowWakeDwellMs;
    CHECK_EQ(int(w.update(t, 3100, 3000)), 2);                          // the other line on the next dwell
    t += pd::kSlowWakeDwellMs;
    CHECK_EQ(int(w.update(t, 3100, 3000)), 1);                          // and back
    CHECK_EQ(int(w.line()), 1);
}
// The signature gone (the readings refreshed while no Rd of ours was on, e.g. the port is empty now): the hold ends.
void test_the_idle_wake_releases_when_no_line_reads_the_signature_any_more() {
    pd::IdleWake w;
    w.set_enabled(true);
    CHECK_EQ(int(w.update(1000, 0, 3050)), 2);
    CHECK_EQ(int(w.update(1000 + pd::kSlowWakeDwellMs, 0, 0)), 0);      // after the dwell: no signature, no Rd
    CHECK_EQ(int(w.update(1000 + pd::kSlowWakeDwellMs + 20, 0, 0)), 0);
    CHECK_EQ(int(w.line()), 0);
}
// An attach that lands while the hold is on keeps the 500 ms no-Rp detach off for the rest of the dwell window, same
// as the seek's holding_slow: the sleeping source may still be waking when the scan picks its line.
void test_an_attach_during_the_hold_keeps_the_no_rp_detach_off_inside_the_dwell() {
    pd::IdleWake w;
    w.set_enabled(true);
    const std::uint32_t t0 = 1000;
    CHECK_EQ(int(w.update(t0, 0, 3050)), 2);
    w.on_attach(2);                                                      // the scan attached on the held line
    CHECK_EQ(w.holding_slow(t0 + 100, 2, 3050), true);
    CHECK_EQ(w.holding_slow(t0 + pd::kSlowWakeDwellMs - 1, 2, 3050), true);
    CHECK_EQ(w.holding_slow(t0 + pd::kSlowWakeDwellMs, 2, 3050), false);  // the window is over
    CHECK_EQ(w.holding_slow(t0 + 100, 1, 0), false);                      // the other line is not in the window
    CHECK_EQ(w.holding_slow(t0 + 100, 2, 1200), false);                   // and not without the signature on it
    w.reset();
    CHECK_EQ(w.holding_slow(t0 + 100, 2, 3050), false);
}
// RdSeek::reset() keeps the open readings: they are the standby evidence a later Rd presentation reads back, and the
// row's bug was that reset() on attach wiped them, so an idle K2 never held a steady Rd long enough.
void test_reset_keeps_the_open_readings() {
    pd::RdSeek s;
    s.note_open(1, 3300);
    s.note_open(2, 3050);
    s.update(1000, 2, true, false);   // some seek state
    s.reset();
    CHECK_EQ(int(s.moves()), 0);
    CHECK_EQ(s.standby(1), true);
    CHECK_EQ(s.standby(2), true);
    CHECK_EQ(s.open_mv(1), 3300);
    CHECK_EQ(s.open_mv(2), 3050);
    CHECK_EQ(s.open_mv(0), -1);
    s.note_open(2, 900);              // a fresh open reading replaces the old one
    CHECK_EQ(s.standby(2), false);
    CHECK_EQ(s.open_mv(2), 900);
}

// The setting: off (the default) the idle wake never picks a line, whatever the open readings say, so the idle Rd stays
// what it was before the feature; and RdSeek::reset(false) wipes the open readings exactly like the old reset().
void test_a_disabled_idle_wake_holds_nothing() {
    pd::IdleWake w;
    CHECK_EQ(w.enabled(), false);                          // default off
    CHECK_EQ(int(w.update(1000, 3300, 3300)), 0);
    CHECK_EQ(int(w.line()), 0);
    w.set_enabled(true);
    CHECK_EQ(w.enabled(), true);
    CHECK_EQ(int(w.update(1020, 3300, 100)), 1);
    w.set_enabled(false);                                  // switched off mid-hold: the Rd goes at once
    CHECK_EQ(int(w.line()), 0);
    CHECK_EQ(int(w.update(1040, 3300, 100)), 0);
    w.on_attach(1);
    CHECK_EQ(w.holding_slow(1050, 1, 3300), false);
}
void test_reset_without_keep_wipes_the_open_readings() {
    pd::RdSeek s;
    s.note_open(1, 3300);
    s.note_open(2, 3050);
    s.reset(false);                                        // the setting is off: the behaviour before the idle wake
    CHECK_EQ(s.standby(1), false);
    CHECK_EQ(s.standby(2), false);
    CHECK_EQ(s.open_mv(1), -1);
    CHECK_EQ(s.open_mv(2), -1);
    s.note_open(1, 3300);
    s.reset(true);
    CHECK_EQ(s.open_mv(1), 3300);
}

// A dark attach: no VBUS (< 3 V) and no PD message for kDarkAttachMs, setting on. Anything else is not dark.
void test_a_dark_attach_needs_the_setting_no_vbus_no_pd_and_the_dwell() {
    pd::IdleWake w;
    CHECK_EQ(w.dark_attach(10000, 0, 0, false), false);                     // setting off: never
    w.set_enabled(true);
    CHECK_EQ(w.dark_attach(10000, 0, 0, false), true);
    CHECK_EQ(w.dark_attach(10000, 0, -1, false), true);                     // no reading yet counts as dark
    CHECK_EQ(w.dark_attach(pd::kDarkAttachMs - 1, 0, 0, false), false);     // not yet
    CHECK_EQ(w.dark_attach(pd::kDarkAttachMs, 0, 0, false), true);
    CHECK_EQ(w.dark_attach(10000, 0, pd::kDarkVbusMv, false), false);       // a live VBUS (legacy 5 V) is not dark
    CHECK_EQ(w.dark_attach(10000, 0, 0, true), false);                      // a decoded PD message is not dark
    CHECK_EQ(w.dark_attach(5000, 4000, 0, false), false);                   // measured from the attach, not from boot
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
    test_the_idle_wake_holds_rd_on_a_standby_signature_line();
    test_the_idle_wake_holds_nothing_without_the_signature();
    test_the_idle_wake_alternates_when_both_lines_read_the_signature();
    test_the_idle_wake_releases_when_no_line_reads_the_signature_any_more();
    test_an_attach_during_the_hold_keeps_the_no_rp_detach_off_inside_the_dwell();
    test_reset_keeps_the_open_readings();
    test_a_disabled_idle_wake_holds_nothing();
    test_reset_without_keep_wipes_the_open_readings();
    test_a_dark_attach_needs_the_setting_no_vbus_no_pd_and_the_dwell();
    return test_report("test_rdseek");
}
