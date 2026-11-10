// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the Waiting stage's pure parts: the footer line choice (the hold-+ hint) and the + key watcher that paces
// the [WAKE] log lines. The event swallow and the paging lock are in test_ui_events.cpp, the page-level proof in
// test_ui_charger_test.cpp.
#include <cstring>
#include <initializer_list>
#include "test_util.h"
#include "k2/ui/wait_plus.hpp"

using namespace k2;
using namespace k2::waitplus;

static constexpr unsigned kFooterCells = 27;   // ui_style: the footer's cell budget

static void test_footer_priorities() {
    // Ready beats everything.
    CHECK_STR(footer(true, 3300, 3300, 0, 0).text, "Charger up, starting");
    // A charger already up: the 0.5..4.5 V standby line and the 3 s "no caps" line are unchanged.
    CHECK_STR(footer(false, 3300, 0, 800, 100).text, "Standby. Wake the charger");
    CHECK_STR(footer(false, 3300, 0, 4400, 100).text, "Standby. Wake the charger");
    CHECK_STR(footer(false, 3300, 0, 5000, 3000).text, "VBUS up, no PD caps yet");
    CHECK_STR(footer(false, 3300, 0, 5000, 2999).text, "Charger to the top port");   // up for under 3 s: caps may still come
    // Nothing on either CC and no VBUS: say how to wake it (the + key is wired to the CC pins).
    CHECK_STR(footer(false, 0, 0, 0, 100).text, "No charger. Hold + few secs");
    CHECK_STR(footer(false, 2500, 2500, 100, 100).text, "No charger. Hold + few secs");   // above the Rp ladder = open
    // A source's Rp on one line, no VBUS: asleep, same hint.
    CHECK_STR(footer(false, 1700, 0, 0, 100).text, "No VBUS. Hold + few secs");
    CHECK_STR(footer(false, 0, 400, 300, 100).text, "No VBUS. Hold + few secs");
    // Unknown readings (-1) are not "open": they fall to the no-VBUS line.
    CHECK_STR(footer(false, -1, -1, 0, 100).text, "No VBUS. Hold + few secs");
}

static void test_footer_tone_and_fit() {
    CHECK_EQ(int(footer(true, 0, 0, 0, 0).tone), int(Tone::Ok));
    CHECK_EQ(int(footer(false, 0, 0, 0, 0).tone), int(Tone::Caution));
    CHECK_EQ(int(footer(false, 1700, 0, 0, 0).tone), int(Tone::Info));
    // Every line the function can say fits the footer.
    const int cc[] = {-1, 0, 400, 900, 1700, 2500};
    const int32_t vbus[] = {0, 300, 800, 4400, 5000, 9000};
    for (int a : cc)
        for (int b : cc)
            for (int32_t v : vbus)
                for (uint32_t w : {0u, 2999u, 3000u}) {
                    const char *t = footer(false, a, b, v, w).text;
                    CHECK_COND(std::strlen(t) <= kFooterCells);
                }
    CHECK_COND(std::strlen(footer(true, 0, 0, 0, 0).text) <= kFooterCells);
}

static void test_cc_open() {
    CHECK_COND(cc_open(0));
    CHECK_COND(cc_open(199));
    CHECK_COND(!cc_open(200));
    CHECK_COND(!cc_open(2199));
    CHECK_COND(cc_open(2200));
    CHECK_COND(!cc_open(-1));
}

// The watcher: Press at the down edge, a Sample every 100 ms while down, Release at the up edge, Samples for 2 s after it.
struct Run { int press = 0, sample = 0, release = 0; uint32_t last_ms = 0; };
static Run drive(PlusWatch &w, uint32_t from, uint32_t to, bool active, bool down) {
    Run r;
    for (uint32_t t = from; t < to; t += 20) {
        switch (w.step(t, active, down)) {
        case PlusWatch::Act::Press: r.press++; r.last_ms = t; break;
        case PlusWatch::Act::Sample: r.sample++; r.last_ms = t; break;
        case PlusWatch::Act::Release: r.release++; r.last_ms = t; break;
        case PlusWatch::Act::None: break;
        }
    }
    return r;
}
static void test_watch_window() {
    PlusWatch w;
    Run idle = drive(w, 0, 5000, true, false);   // never pressed: silent, however long the wait
    CHECK_EQ(idle.press + idle.sample + idle.release, 0);
    Run down = drive(w, 5000, 7000, true, true);   // 2 s held
    CHECK_EQ(down.press, 1);
    CHECK_EQ(down.sample >= 18 && down.sample <= 20, true);   // ~10 Hz
    Run up = drive(w, 7000, 11000, true, false);   // released: one Release, then the 2 s tail, then silence
    CHECK_EQ(up.release, 1);
    CHECK_EQ(up.press, 0);
    CHECK_EQ(up.sample >= 18 && up.sample <= 20, true);
    CHECK_EQ(up.last_ms >= 8900 && up.last_ms <= 9100, true);   // the last sample ~2 s after the release
    Run quiet = drive(w, 11000, 14000, true, false);
    CHECK_EQ(quiet.press + quiet.sample + quiet.release, 0);
}
static void test_watch_not_waiting() {
    PlusWatch w;
    CHECK_EQ(int(w.step(100, false, true)), int(PlusWatch::Act::None));   // + down on another stage: nothing
    CHECK_EQ(int(w.step(200, false, true)), int(PlusWatch::Act::None));
    // Entering Waiting with + already down counts as a press at the first tick.
    CHECK_EQ(int(w.step(300, true, true)), int(PlusWatch::Act::Press));
    // Leaving Waiting mid-hold ends the window silently; a new Waiting starts a fresh one.
    CHECK_EQ(int(w.step(320, false, true)), int(PlusWatch::Act::None));
    CHECK_EQ(int(w.step(340, true, true)), int(PlusWatch::Act::Press));
}
static void test_watch_second_press_in_tail() {
    PlusWatch w;
    drive(w, 0, 1000, true, true);
    Run up = drive(w, 1000, 1500, true, false);
    CHECK_EQ(up.release, 1);
    Run again = drive(w, 1500, 1520, true, true);   // pressed again inside the tail: a new Press, the window restarts
    CHECK_EQ(again.press, 1);
    Run tail = drive(w, 1520, 1800, true, true);
    CHECK_EQ(tail.sample >= 2 && tail.sample <= 3, true);
}

int main() {
    test_footer_priorities();
    test_footer_tone_and_fit();
    test_cc_open();
    test_watch_window();
    test_watch_not_waiting();
    test_watch_second_press_in_tail();
    return test_report("test_wait_plus");
}
