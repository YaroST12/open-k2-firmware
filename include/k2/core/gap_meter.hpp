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

inline Meter loop;   // main loop iterations
inline Meter ui;     // Ui::tick calls

}  // namespace k2::gap
