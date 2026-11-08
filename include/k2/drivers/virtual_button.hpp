// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// One remote-injected button: a virtual pin level that is high for the requested time, then held
// low for a release gap before the next press is accepted. Pure: time in, level out.
#pragma once
#include <cstdint>

#include "k2/drivers/buttons.hpp"

namespace k2 {

class VirtualButton {
public:
    // The debouncer needs the release stable for kDebounceMs before it fires the Short; a press
    // sent sooner would merge with the previous one. A few ms of margin for the 1 ms sampling.
    static constexpr uint32_t kReleaseGapMs = ButtonDetector::kDebounceMs + 10;

    // True while the injected press holds the pin level high. The main loop can stall (LCD redraw,
    // screenshot) for longer than a press. The level therefore stays high past the window until the
    // loop has seen it high for the debounce time, counted from its first sample; otherwise the
    // detector sees one high sample, then low, and the press is lost.
    bool down(uint32_t now_ms) {
        if (!armed_) return false;
        if (!sampled_) {
            sampled_ = true;
            first_ms_ = now_ms;
        }
        const bool seen = now_ms - first_ms_ >= ButtonDetector::kDebounceMs + 2;
        if (static_cast<int32_t>(now_ms - until_ms_) < 0) {
            if (seen) confirmed_ = true;
            return true;
        }
        if (confirmed_) return false;
        if (seen) {                                                    // seen high long enough
            confirmed_ = true;
            until_ms_ = now_ms + 1;                                    // this sample is the last high one
            free_ms_ = until_ms_ + kReleaseGapMs;
        }
        return true;
    }

    // False (Busy) while the previous press is held or its release is still settling.
    bool press(uint32_t now_ms, uint32_t ms) {
        if (armed_ && (!confirmed_ || static_cast<int32_t>(now_ms - free_ms_) < 0)) return false;
        armed_ = true;
        sampled_ = false;
        confirmed_ = false;
        ms_ = ms;
        until_ms_ = now_ms + ms;
        free_ms_ = until_ms_ + kReleaseGapMs;
        return true;
    }

private:
    bool armed_ = false;
    bool sampled_ = false;
    bool confirmed_ = false;
    uint32_t first_ms_ = 0;
    uint32_t ms_ = 0;
    uint32_t until_ms_ = 0;
    uint32_t free_ms_ = 0;
};

}  // namespace k2
