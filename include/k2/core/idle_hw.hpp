// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The idle glue: feeds the pure idle machine (k2/core/idle.hpp) from the live firmware and applies what it decides: the
// backlight level, the panel sleep and wake, the UI's paint gate and the AHB divider. The main loop calls service() every
// 20 ms before the UI tick; the button path calls press() / touch(); the remote dispatcher calls request() before each
// command runs. The settings are idle::Config; the bench levers (force, forced divider, wake-on-remote) are RAM only.
//
// Low clock: while the machine reports Off with nothing attached, no contract and nothing busy, the core drops to
// 240 MHz / kLowClockDiv. It goes back to full speed on the first sign of life: a PD message or VBUS move (checked every
// pass by pass_guard()), any state-changing remote command or shell input, a button, or the machine leaving Off. It is also
// refused (and reverted) while a DWT-cycle user runs (the D+/D- engines hardcode 240 cycles per microsecond), see Block.
#pragma once

#include <cstdint>

#include "k2/core/idle.hpp"

namespace k2 {
class Ui;
}

namespace k2::idle_hw {

inline constexpr std::uint8_t kLowClockDiv = 4;       // 60 MHz; /16 would put APB under the USB register interface's minimum
inline constexpr std::uint32_t kSaveDelayMs = 2000;    // one flash write per burst of PLUS/MINUS steps
inline constexpr std::uint32_t kClockHoldMs = 10000;   // after a state-changing remote command the clock stays full this long

// Why the low clock is refused right now (Status::blocked): a running user of DWT cycle timing or of a shared timer.
enum Block : std::uint8_t {
    kBlockLoad = 1 << 0,        // a load pulse on PA8
    kBlockUart2 = 1 << 1,       // USART2 claimed (the loop probe / a protocol lab)
    kBlockSniff = 1 << 2,       // the D+/D- edge sniffer is armed
    kBlockMonitor = 1 << 3,     // the passive SuperVOOC monitor is running
    kBlockFc = 1 << 4,          // a Quick Charge / SuperVOOC / UFCS trigger or scan is running
    kBlockHold = 1 << 5,        // a state-changing remote command ran within kClockHoldMs
};

struct Status {
    idle::State state = idle::State::Active;
    idle::Force force = idle::Force::Auto;
    std::uint8_t brightness_now = 100;     // what the backlight is asked for now (lcd::brightness())
    std::uint8_t panel = 0;                // 0 awake, 1 asleep, 2 waking
    std::uint8_t hclk_div = 1;             // the divider in force now
    std::uint8_t force_div = 0;            // the bench override, 0 = automatic
    std::uint8_t blocked = 0;              // Block bits
    std::uint32_t idle_s = 0;              // seconds since the last activity
    idle::Config cfg;
};

// Boot: nothing to start yet (the saved settings are loaded by load_config() once the file system is up).
void init();
// Boot, after the file system is mounted: the saved levels and timeouts (defaults when none).
void load_config();
// Every 20 ms from the slow tasks, before the UI tick. boot_screen = the QR screen owns the panel.
void service(Ui& ui, bool boot_screen, std::uint32_t now);
// A physical button went down. Returns whether it is delivered to the UI: false = it only woke the screen, drop the whole press.
bool press(std::uint32_t now);
// Any other physical button event (release, long press).
void touch(std::uint32_t now);
// A remote request is about to run. state_changing = it is not on the read-only list (remote::wakes_screen): it counts as
// activity (when Config::wake_on_remote) and the clock goes back to full speed for kClockHoldMs. Shell input comes here too.
void request(bool state_changing, std::uint32_t now);
// Every main-loop pass: back to full clock at the first PD message, VBUS move or CC attach while the clock is low.
void pass_guard();

Status status();
const idle::Config& config();
// Takes the whole config (clamped). Brightness and timeout changes are saved once kSaveDelayMs passes quiet and nothing runs on the ports.
void set_config(const idle::Config& c);
void force(idle::Force f);
void force_div(std::uint8_t n);   // 0 = automatic, else 1 / 2 / 4 / 8; refused values are ignored

}  // namespace k2::idle_hw
