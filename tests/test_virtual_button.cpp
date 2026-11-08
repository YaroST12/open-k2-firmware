// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A remote burst must reach the UI as one event per press (QA 2026-11-01: minus x8 + plus x2 moved
// the selection by 1). The test drives the real ButtonDetector the way main.cpp does.
#include <initializer_list>
#include "k2/drivers/buttons.hpp"
#include "k2/drivers/virtual_button.hpp"
#include "test_util.h"

using namespace k2;

namespace {
// The remote client: sends a press whenever the firmware accepts it (a Busy answer is retried one
// millisecond later), for `presses` presses of `ms` each. Returns the Short events the UI saw.
int burst(int presses, uint32_t ms, uint32_t usb_latency_ms) {
    VirtualButton vb;
    ButtonDetector det;
    int events = 0, sent = 0;
    uint32_t next_try = 0;
    for (uint32_t now = 1; now < 1000 + presses * (ms + 400); now++) {
        if (sent < presses && now >= next_try && vb.press(now, ms)) { sent++; next_try = now + usb_latency_ms; }
        else if (sent < presses && now >= next_try) next_try = now + 1;
        if (det.update(vb.down(now), now) == ButtonEvent::Short) events++;
    }
    return events;
}
}

int main() {
    CHECK_EQ(burst(10, 100, 2), 10);
    CHECK_EQ(burst(10, 30, 1), 10);
    CHECK_EQ(burst(1, 100, 2), 1);

    // Main-loop stall (LCD redraw, screenshot): nothing samples the pin for longer than the press.
    // The press must still be seen, one event (intermittent ignored OK, 2026-11-02).
    for (uint32_t stall : {60u, 100u, 150u, 400u}) {
        VirtualButton sv;
        ButtonDetector sd;
        int ev = 0;
        sv.press(1, 100);
        for (uint32_t now = 1 + stall; now < 1500; now++)
            if (sd.update(sv.down(now), now) == ButtonEvent::Short) ev++;
        CHECK_EQ(ev, 1);
    }

    // Sampled once inside the window, then the loop stalls past its end: the detector saw a single
    // high sample, never the debounce time, and lost the press (device repro 2026-11-02).
    for (uint32_t stall : {30u, 60u, 100u, 150u, 400u}) {
        VirtualButton sv;
        ButtonDetector sd;
        int ev = 0;
        sv.press(1, 100);
        for (uint32_t now = 1; now < 1500; now++) {
            if (now > 1 && now < 1 + stall) continue;      // loop stalled right after the first sample
            if (sd.update(sv.down(now), now) == ButtonEvent::Short) ev++;
        }
        CHECK_EQ(ev, 1);
    }

    // Plus then OK back to back (OK sent while the plus press is still held), stalls after each edge.
    for (uint32_t stall : {0u, 40u, 120u, 300u}) {
        VirtualButton p, o;
        ButtonDetector dp, dok;
        int evp = 0, evo = 0;
        p.press(1, 100);
        o.press(30, 100);
        uint32_t resume = 0;
        for (uint32_t now = 1; now < 1500; now++) {
            if (now < resume) continue;
            if (dp.update(p.down(now), now) == ButtonEvent::Short) { evp++; resume = now + stall; }
            if (dok.update(o.down(now), now) == ButtonEvent::Short) evo++;
            if (now == 2 || now == 31) resume = now + 1 + stall;
        }
        CHECK_EQ(evp, 1);
        CHECK_EQ(evo, 1);
    }

    VirtualButton vb;
    CHECK_EQ(vb.press(100, 50), true);
    CHECK_EQ(vb.down(120), true);
    CHECK_EQ(vb.press(120, 50), false);      // still held: Busy
    CHECK_EQ(vb.down(145), true);            // sampled every ms in the real loop
    CHECK_EQ(vb.down(150), false);
    CHECK_EQ(vb.press(155, 50), false);      // released, but the detector has not seen the release yet
    CHECK_EQ(vb.press(150 + VirtualButton::kReleaseGapMs, 50), true);
    return test_report("test_virtual_button");
}
