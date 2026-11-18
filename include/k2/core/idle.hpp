// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The idle machine: when the K2 sits unattended the screen dims, then turns off, and (while off) the core clock may drop.
// Pure timing and activity rules, no hardware; the glue that applies the outputs (backlight, panel, HCLK divider) and feeds the
// inputs is src/core/idle_hw.cpp, the persisted settings are in src/ui/uiprefs.cpp.
//
// States: Active -> Dim (after dim_after_s of no activity) -> Off (after off_after_s). A timeout of 0 means never; an off
// timeout at or below the dim timeout skips the dim step. Dim keeps painting the full content and only lowers the backlight
// (to the smaller of dim_brightness and brightness); Off turns the panel off and stops painting.
//
// Activity restarts the idle time (the machine never goes back from Off on its own):
//   - a button press (press()): in Active it is delivered and restarts the timer; in Dim or Off it only wakes the screen and is
//     swallowed (press() returns false, the glue drops that whole press);
//   - VBUS appearing, disappearing or moving by more than max(250 mV, 2.5 %) against the last value that counted;
//   - bus current above 50 mA, either direction;
//   - any PD message or attach / detach (Inputs::pd_event, one tick per event batch);
//   - busy (a tool or charger test running, the boot screen): holds Active for as long as it lasts;
//   - an alarm (Home W alarm, an Error footer): holds Active for as long as the glue reports it;
//   - a state-changing remote command (remote_activity()), only when Config::wake_on_remote is set. A plain ping or read
//     must never wake the screen: the glue only calls remote_activity() for commands that change something.
//
// force() pins the state for the bench (the remote "power" command): Active holds the screen awake, Dim and Off pin it down
// until a press (which wakes and releases the pin) or force(Auto). Auto restarts the idle time.
//
// low_clock_ok tells the glue the HCLK divider may be raised: only in Off, with no PD contract being negotiated and nothing busy.
//
// All times are the 32-bit millisecond clock. The machine adds each tick's elapsed time to a saturating idle counter, so the
// clock wrap is harmless and an idle period longer than the wrap never reads as activity.
#pragma once

#include <cstdint>

namespace k2::idle {

enum class State : std::uint8_t { Active, Dim, Off };
enum class Force : std::uint8_t { Auto, Active, Dim, Off };

inline constexpr std::int32_t kVbusStepMv = 250;       // a VBUS move above max(this, kVbusStepPermille) is activity
inline constexpr std::int32_t kVbusStepPermille = 25;  // 2.5 % of the reference
inline constexpr std::int32_t kBusActiveMa = 50;       // |IBUS| above this is activity
inline constexpr std::uint8_t kMaxPct = 100;

struct Config {
    std::uint16_t dim_after_s = 60;        // 0 = never
    std::uint16_t off_after_s = 300;       // 0 = never
    std::uint8_t brightness = 100;         // percent, 0..100
    std::uint8_t dim_brightness = 30;      // percent, 0..100
    bool wake_on_remote = true;            // a state-changing remote command counts as activity (RAM only)
};

struct Inputs {
    std::uint32_t now_ms = 0;
    bool busy = false;
    bool alarm = false;
    bool pd_negotiating = false;           // a contract is being negotiated: the core clock stays up
    bool pd_event = false;                 // a PD message or attach / detach since the last tick
    bool vbus_known = false;
    std::int32_t vbus_mv = 0;
    std::int32_t ibus_ma = 0;
};

struct Output {
    State state = State::Active;
    std::uint8_t brightness = kMaxPct;     // target backlight, percent
    bool panel_on = true;
    bool paint = true;                     // the UI may paint (false only in Off)
    bool low_clock_ok = false;
    bool woke = false;                     // the screen left Off since the last tick: repaint everything
};

class Machine {
public:
    const Config& config() const { return cfg_; }
    void set_config(const Config& c);

    // One pass of the glue (every 20 ms is plenty): folds the inputs in and returns what to apply.
    Output tick(const Inputs& in);
    // A button press (the press-down). Returns whether the press is delivered to the UI (false: it only woke the screen).
    bool press(std::uint32_t now_ms);
    // Any other button activity (release, long press): restarts the idle time.
    void touch(std::uint32_t now_ms);
    // A state-changing remote command arrived. Wakes only with Config::wake_on_remote.
    void remote_activity(std::uint32_t now_ms);

    void force(Force f);
    Force force_mode() const { return force_; }
    State state() const { return state_; }
    std::uint32_t idle_s() const { return idle_ms_ / 1000u; }

private:
    void advance(std::uint32_t now_ms);
    State effective() const;
    void settle();

    Config cfg_;
    State state_ = State::Active;
    Force force_ = Force::Auto;
    bool started_ = false;
    bool vref_valid_ = false;
    bool woke_ = false;
    std::uint32_t last_ms_ = 0;
    std::uint32_t idle_ms_ = 0;
    std::int32_t vref_mv_ = 0;
};

}  // namespace k2::idle
