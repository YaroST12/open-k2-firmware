// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The idle machine (k2/core/idle.hpp): pure timing and activity rules.
#include "k2/core/idle.hpp"

namespace k2::idle {

namespace {

constexpr std::int32_t absi(std::int32_t v) { return v < 0 ? -v : v; }

std::uint8_t clamp_pct(std::uint8_t v) { return v > kMaxPct ? kMaxPct : v; }

}  // namespace

void Machine::set_config(const Config& c) {
    cfg_ = c;
    cfg_.brightness = clamp_pct(cfg_.brightness);
    cfg_.dim_brightness = clamp_pct(cfg_.dim_brightness);
    settle();
}

void Machine::advance(std::uint32_t now_ms) {
    if (!started_) {
        started_ = true;
        last_ms_ = now_ms;
        return;
    }
    const std::uint32_t delta = now_ms - last_ms_;   // modular: the clock wrap cancels
    // An older stamp (a press sampled in a wait loop with a fresh millis(), then the loop's tick with its earlier now) is no
    // elapsed time: read modular, it would be a 49-day idle that turns the screen off.
    if (static_cast<std::int32_t>(delta) <= 0) return;
    last_ms_ = now_ms;
    idle_ms_ = delta > 0xFFFFFFFFu - idle_ms_ ? 0xFFFFFFFFu : idle_ms_ + delta;
}

State Machine::effective() const {
    switch (force_) {
    case Force::Active: return State::Active;
    case Force::Dim: return State::Dim;
    case Force::Off: return State::Off;
    case Force::Auto: break;
    }
    const std::uint32_t off_ms = static_cast<std::uint32_t>(cfg_.off_after_s) * 1000u;
    const std::uint32_t dim_ms = static_cast<std::uint32_t>(cfg_.dim_after_s) * 1000u;
    if (off_ms != 0 && idle_ms_ >= off_ms) return present_ ? State::Dim : State::Off;   // attached: never past Dim
    if (dim_ms != 0 && idle_ms_ >= dim_ms) return State::Dim;
    return State::Active;
}

void Machine::settle() {
    const State s = effective();
    if (state_ == State::Off && s != State::Off) woke_ = true;
    state_ = s;
}

void Machine::touch(std::uint32_t now_ms) {
    advance(now_ms);
    idle_ms_ = 0;
    settle();
}

void Machine::remote_activity(std::uint32_t now_ms) {
    if (cfg_.wake_on_remote) touch(now_ms);
}

bool Machine::press(std::uint32_t now_ms) {
    advance(now_ms);
    settle();
    const bool deliver = state_ == State::Active;
    if (force_ == Force::Dim || force_ == Force::Off) force_ = Force::Auto;   // a press releases a pinned screen
    idle_ms_ = 0;
    settle();
    return deliver;
}

void Machine::force(Force f) {
    if (f == Force::Auto && force_ != Force::Auto) idle_ms_ = 0;   // releasing a pin restarts the idle time
    force_ = f;
    settle();
}

Output Machine::tick(const Inputs& in) {
    advance(in.now_ms);
    bool activity = in.busy || in.alarm || in.pd_event || absi(in.ibus_ma) > kBusActiveMa;
    if (in.vbus_known) {
        if (!vref_valid_) {
            vref_valid_ = true;
            vref_mv_ = in.vbus_mv;
            present_ = in.vbus_mv >= kVbusPresentMv;
        } else {
            std::int32_t thr = absi(vref_mv_) * kVbusStepPermille / 1000;
            if (thr < kVbusStepMv) thr = kVbusStepMv;
            if (absi(in.vbus_mv - vref_mv_) > thr) {
                activity = true;
                vref_mv_ = in.vbus_mv;
            }
            const bool present = in.vbus_mv >= kVbusPresentMv;
            if (present != present_) {   // attach or detach: the user is probably there
                present_ = present;
                activity = true;
            }
        }
    }
    if (activity) idle_ms_ = 0;
    settle();

    Output o;
    o.state = state_;
    o.woke = woke_;
    woke_ = false;
    o.panel_on = state_ != State::Off;
    o.paint = o.panel_on;
    switch (state_) {
    case State::Active: o.brightness = cfg_.brightness; break;
    case State::Dim: o.brightness = cfg_.dim_brightness < cfg_.brightness ? cfg_.dim_brightness : cfg_.brightness; break;
    case State::Off: o.brightness = 0; break;
    }
    o.low_clock_ok = state_ != State::Active && !in.busy && !in.pd_negotiating;
    return o;
}

}  // namespace k2::idle
