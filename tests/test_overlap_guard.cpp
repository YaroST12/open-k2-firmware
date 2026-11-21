// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The overlap guard (tests/overlap_log.cpp) must see damage and must stay quiet for the legal cases, in the plain and the
// stretched viewports: a guard that never fires (every draw read as repainted) would pass the whole UI walk.
#include "test_util.h"
#include "fake_lcd.h"
#include "overlap_log.h"
#include "ui_stubs.h"
#include "k2/ui/gfx.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_viewport.hpp"
#include "k2/ui/trunc.hpp"
#include <algorithm>
#include <functional>

using namespace k2;

namespace {

// Hits the guard reports for `frame` drawn on a blank panel through viewport `dest` (empty rect = none).
int hits_of(const Rect &dest, const std::function<void(const Surface &)> &frame) {
    std::fill(std::begin(g_panel), std::end(g_panel), kBg);
    overlap_reset();
    trunc::clear();
    const Surface logical{0, 0, 208, 240};
    if (dest.w > 0) {
        ContentViewport viewport(dest, 208, 240);
        frame(logical);
    } else {
        frame(logical);
    }
    trunc::settled();
    const int n = trunc::failures();
    trunc::clear();
    return n;
}

void run(const Rect &dest) {
    // Two labels whose boxes overlap: the second takes ink from the first.
    CHECK_EQ(hits_of(dest, [](const Surface &s) {
        gfx_text(s, kSmall, 20, 50, "Cable", kText, kBg);
        gfx_text(s, kSmall, 36, 54, "Time", kText, kBg);
    }), 1);
    // Clear of each other: nothing.
    CHECK_EQ(hits_of(dest, [](const Surface &s) {
        gfx_text(s, kSmall, 20, 50, "Cable", kText, kBg);
        gfx_text(s, kSmall, 20, 80, "Time", kText, kBg);
    }), 0);
    // A later draw that repaints the whole box of the first: the first is dead, not damaged.
    CHECK_EQ(hits_of(dest, [](const Surface &s) {
        gfx_text(s, kSmall, 20, 50, "Cable", kText, kBg);
        gfx_text(s, kSmall, 20, 50, "Cable", kText, kBg);
    }), 0);
    // The same field redrawn with the same width (a value changing in place).
    CHECK_EQ(hits_of(dest, [](const Surface &s) {
        gfx_text(s, kSmall, 20, 50, "20.0V", kText, kBg);
        gfx_text(s, kSmall, 20, 50, "19.9V", kText, kBg);
    }), 0);
    // A shorter redraw at the same origin leaves the old tail: that is the stale-pill bug, not a field update.
    CHECK_EQ(hits_of(dest, [](const Surface &s) {
        gfx_text(s, kSmall, 20, 50, "Run 1 done, not saved", kText, kBg);
        gfx_text(s, kSmall, 52, 50, "Run 1 saved", kText, kBg);
    }), 1);
    // A deliberate composite (text over a trend, a toast over the page).
    CHECK_EQ(hits_of(dest, [](const Surface &s) {
        gfx_text(s, kSmall, 20, 50, "Cable", kText, kBg);
        K2_OVERLAP_OK;
        gfx_text(s, kSmall, 36, 54, "Time", kText, kBg);
    }), 0);
}

// Rail text is rotated and drawn in panel space (no viewport).
void run_rotated() {
    const Rect none{0, 0, 0, 0};
    const auto pair = [](int x2, int y2) {
        return [x2, y2](const Surface &s) {
            gfx_text_rot(s, kLabelFont, 2, 40, "CHARGER", kText, kBg, TextDir::Ccw);
            gfx_text_rot(s, kLabelFont, static_cast<int16_t>(x2), static_cast<int16_t>(y2), "10s", kText, kBg, TextDir::Ccw);
        };
    };
    CHECK_EQ(hits_of(none, pair(8, 60)), 1);
    CHECK_EQ(hits_of(none, pair(2, 100)), 0);
}

}  // namespace

int main() {
    overlap_enable();
    run(Rect{0, 0, 0, 0});            // rotation 0: no viewport
    run(Rect{16, 0, 208, 240});       // rotation 2: content shifted past the strip, same size
    run(Rect{0, 16, 240, 208});       // landscape: content stretched
    run_rotated();
    return test_report("test_overlap_guard");
}
