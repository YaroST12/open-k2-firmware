// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The shell-drive clock behind the PC-link icon's hourglass: when the host drives the K2 (a control request, see
// remote::shell_control) it reads "shown". Shown from the first control request, held for the whole of a charger test or
// tool run that began while it was showing (the shell started it), otherwise cleared kHoldMs after the last control request.
// Local keys are not an input: they never clear it. The caller repaints only on the edges these return. All times are the 32-bit ms clock (modular differences).
#pragma once

#include <cstdint>

namespace k2::shell {

inline constexpr uint32_t kHoldMs = 5000;

class Band {
public:
    // A control request arrived. True when it just appeared.
    bool control(uint32_t now) {
        until_ = now + kHoldMs;
        const bool edge = !on_;
        on_ = true;
        return edge;
    }
    // Once per main-loop pass. `run`: a charger test or tool run is active. True when it just went away.
    bool tick(uint32_t now, bool run) {
        if (run && !prev_run_ && on_) owned_ = true;   // the run began while the shell was driving: the shell's run
        if (!run && prev_run_ && owned_) {
            owned_ = false;
            until_ = now + kHoldMs;                    // a held state lingers 5 s past the end of the run
        }
        prev_run_ = run;
        if (!on_ || owned_ || static_cast<int32_t>(now - until_) < 0) return false;
        on_ = false;
        return true;
    }
    bool shown() const { return on_; }
    void reset() { *this = Band{}; }

private:
    uint32_t until_ = 0;
    bool on_ = false, owned_ = false, prev_run_ = false;
};

}  // namespace k2::shell
