// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Longest gap between two calls of the same loop, in ms: the instrument that shows what blocks the cooperative main loop.
// `loop` is noted at the top of every main-loop iteration (a healthy iteration is 1 ms: SysTick ends the wfi), `ui` at the
// top of every Ui::tick (a healthy gap is the 20 ms service period). A window (begin_window .. peak) holds the longest gap
// since it began: the charger test opens one when a row starts and reads it when the row ends ([CT] gap).
#pragma once
#include <cstdint>

namespace k2::gap {

struct Meter {
    void note(uint32_t now) {
        if (seen_) {
            const uint32_t d = now - last_;
            if (d > peak_) peak_ = d;
        }
        last_ = now;
        seen_ = true;
    }
    void begin_window() { peak_ = 0; }
    uint32_t peak() const { return peak_; }

private:
    uint32_t last_ = 0, peak_ = 0;
    bool seen_ = false;
};

// How long the charger-test page wanted to paint and could not: it sits out every tick while the engine is inside a window the
// LCD idle hook does not serve (Runner::paint_blocked), and the screen holds still for that whole time. The page notes every
// tick with whether it was blocked; a window runs from its first blocked tick to the first free one. Two peaks are kept: the
// row's (begin_window .. peak, read when the row ends, the [CT] gap blocked= field) and the run's (begin_run .. run_peak, the
// DebugLcd reply). Both include a window still open at the time they are read.
struct BlockMeter {
    void note(uint32_t now, bool blocked) {
        if (blocked) {
            if (!open_) { open_ = true; start_ = now; }
            return;
        }
        if (open_) {
            const uint32_t d = now - start_;
            if (d > row_) row_ = d;
            if (d > run_) run_ = d;
            open_ = false;
        }
    }
    void begin_window(uint32_t now) {   // an open window belongs to the new row from here (the run keeps what it had so far)
        row_ = 0;
        if (open_) {
            if (now - start_ > run_) run_ = now - start_;
            start_ = now;
        }
    }
    void begin_run(uint32_t now) { run_ = 0; begin_window(now); }
    uint32_t peak(uint32_t now) const { return open_ && now - start_ > row_ ? now - start_ : row_; }
    uint32_t run_peak(uint32_t now) const { return open_ && now - start_ > run_ ? now - start_ : run_; }

private:
    uint32_t start_ = 0, row_ = 0, run_ = 0;
    bool open_ = false;
};

inline Meter loop;           // main loop iterations
inline Meter ui;             // Ui::tick calls
inline BlockMeter blocked;   // charger-test page ticks sat out inside an engine window

}  // namespace k2::gap
