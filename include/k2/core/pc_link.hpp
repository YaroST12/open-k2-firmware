// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PC-link icon's state, the HID (PC) port as the user sees it: nothing / power only / a PC using the port / a PC and the
// shell at work. Pure: the caller feeds the aux rail (the HID port's VBUS, -1 when unread), whether the USB host is active
// (mounted and awake) and whether the shell is driving; it repaints the icon only when update() returns a new state.
#pragma once

#include <cstdint>

#include "k2/core/bss_zero.hpp"
#include "k2/core/loop_guard.hpp"

namespace k2::pclink {

// The numeric values are the wire byte (session part offset 31) and the order of the icons.
enum class State : uint8_t { None = 0, Power = 1, Host = 2, Shell = 3 };

inline constexpr const char *name(State s) {
    switch (s) {
    case State::Power: return "power";
    case State::Host: return "host";
    case State::Shell: return "shell";
    default: return "none";
    }
}

struct Input {
    int32_t aux_mv;   // the HID port's VBUS; negative: no reading yet
    bool host;        // usb::host_active(): configured and not suspended
    bool shell;       // the shell is driving (shell::Band::shown())
};

class Tracker {
public:
    State update(const Input &in) {
        if (in.aux_mv >= 0) up_ = in.aux_mv >= (up_ ? loopguard::kAuxGoneMv : loopguard::kAuxPresentMv);
        if (in.host) return in.shell ? State::Shell : State::Host;   // a mounted PC is there whatever the divider reads
        return up_ ? State::Power : State::None;
    }

private:
    bool up_ K2_BSS_INIT(false);
};

}  // namespace k2::pclink
