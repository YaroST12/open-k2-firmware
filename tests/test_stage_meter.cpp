// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The main loop's stage meter (k2/core/stage_meter.hpp): the main loop marks the start of each poll it calls; the meter
// keeps the longest stage of the window with when it began and how far the millisecond clock moved meanwhile (a stage
// whose real time is far longer than its millisecond advance ran with interrupts masked).
#include "k2/core/stage_meter.hpp"
#include "test_util.h"

#define CHECK(c) CHECK_EQ(!!(c), 1)

using namespace k2;
using Id = stage::Id;

namespace {

constexpr std::uint32_t kCyc = 240'000;   // cycles per ms

// The worst stage, its length, its start (ms since the window began) and the ms the clock moved.
void test_worst_stage_and_when() {
    stage::Meter m;
    m.reset(1000 * kCyc, 1000);
    m.mark(Id::Usb, 1000 * kCyc, 1000);
    m.mark(Id::UiTick, 1002 * kCyc, 1002);
    m.mark(Id::Idle, 1171 * kCyc, 1171);   // the UI tick ran 169 ms, starting 2 ms into the window
    m.mark(Id::Usb, 1172 * kCyc, 1172);
    m.mark(Id::Remote, 1173 * kCyc, 1173);
    const stage::Worst w = m.worst();
    CHECK_EQ(int(w.id), int(Id::UiTick));
    CHECK_EQ(w.us, 169'000u);
    CHECK_EQ(w.at_ms, 2u);
    CHECK_EQ(w.ticked_ms, 169u);
    CHECK_STR(stage::name(w.id), "ui");
}

// Interrupts masked: the cycle counter runs on, the millisecond clock does not (SysTick waits).
void test_masked_stage_shows_in_ticked_ms() {
    stage::Meter m;
    m.reset(0, 0);
    m.mark(Id::RdComeback, 10 * kCyc, 10);
    m.mark(Id::Session, 130 * kCyc, 11);   // 120 ms of real time, 1 ms on the clock
    const stage::Worst w = m.worst();
    CHECK_EQ(int(w.id), int(Id::RdComeback));
    CHECK_EQ(w.us, 120'000u);
    CHECK_EQ(w.ticked_ms, 1u);
    CHECK_EQ(w.at_ms, 10u);
}

// reset() opens a new window: what came before is gone, the stage running across the reset is measured from the reset.
void test_reset_opens_a_window() {
    stage::Meter m;
    m.reset(0, 0);
    m.mark(Id::UiTick, 0, 0);
    m.mark(Id::Idle, 200 * kCyc, 200);
    m.mark(Id::Fctrig, 300 * kCyc, 300);
    m.reset(310 * kCyc, 310);
    m.mark(Id::Idle, 315 * kCyc, 315);   // Fctrig ran 15 ms, 5 of them inside the window
    const stage::Worst w = m.worst();
    CHECK_EQ(int(w.id), int(Id::Fctrig));
    CHECK_EQ(w.us, 5'000u);
    CHECK_EQ(w.at_ms, 0u);
    stage::Meter fresh;
    CHECK_EQ(fresh.worst().us, 0u);
    CHECK_STR(stage::name(fresh.worst().id), "none");
}

// Every id has its own short name.
void test_names() {
    for (int i = 0; i < int(Id::Count_); ++i)
        for (int j = i + 1; j < int(Id::Count_); ++j)
            CHECK(strcmp(stage::name(Id(i)), stage::name(Id(j))) != 0);
}

}  // namespace

int main() {
    test_worst_stage_and_when();
    test_masked_stage_shows_in_ticked_ms();
    test_reset_opens_a_window();
    test_names();
    return test_report("test_stage_meter");
}
