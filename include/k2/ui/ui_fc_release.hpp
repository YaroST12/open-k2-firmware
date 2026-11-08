// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include <cstdint>
#include "k2/pd/pd_link.hpp"

namespace k2::trig {

// A release command is not proof that the source obeyed it. This monitor survives menu exit.
class Release {
public:
    enum class State : uint8_t { Idle, Releasing, Safe, Unsafe };
    void start(uint32_t now) {
        if (busy()) return;   // repeated cancel must not postpone the deadline
        state_ = State::Releasing;
        started_ = last_ = now;
        stable_ = false;
    }
    void poll(uint32_t now, int mv, bool engines_idle) {
        if (state_ == State::Safe && (!pd::is_vsafe5(mv) || !engines_idle)) state_ = State::Idle;
        if (!busy()) return;
        const uint32_t gap = now - last_;
        last_ = now;
        if (pd::is_vsafe5(mv) && engines_idle) {
            if (!stable_ || gap > 250) { stable_ = true; stable_since_ = now; }
            if (now - stable_since_ >= 500) { state_ = State::Safe; return; }
        } else stable_ = false;
        if (now - started_ >= 8000) state_ = State::Unsafe;
    }
    bool busy() const { return state_ == State::Releasing || state_ == State::Unsafe; }
    State state() const { return state_; }
private:
    State state_ = State::Idle;
    uint32_t started_ = 0, last_ = 0, stable_since_ = 0;
    bool stable_ = false;
};
}  // namespace k2::trig
