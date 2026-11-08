// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PA8 test load (TIM1 CH1 PWM, ~80 kHz). It is presumed to switch a dummy load; a short pulse
// (~150 ms) ahead of the VOOC wake train, only while VBUS < 6.0 V. Duty is 0 whenever nothing asked for it.
// Every pulse is bounded by load_guard.hpp: <= 95 %, <= 200 ms, VBUS 4.5..6.0 V, MCU <= 70 C. The
// deadline is enforced from the SysTick interrupt, the VBUS / temperature rules from poll().
#pragma once
#include <cstdint>

#include "k2/drivers/load_guard.hpp"

namespace k2::load {

struct Result {
    Abort reason = Abort::None;     // Done = ran the full time
    uint8_t duty = 0;
    uint16_t ms = 0;                // time actually on
    int32_t idle_ma = 0;            // current before the pulse
    int32_t peak_ma = 0;            // highest current during the pulse
    int32_t min_vbus_mv = 0;
    int32_t vbus_before_mv = 0;
    uint8_t pin_high_pct = 0;       // PA8 read back during the pulse: proves the timer drives the pin
    int16_t temp_before_deci_c = 0, temp_after_deci_c = 0;
};

// Clock, TIM1 setup, PA8 as a low output (duty 0). Hooks the SysTick deadline.
void init();

// Start a pulse (non-blocking). Returns None when it started, else why it was refused. The caller
// keeps calling poll() (main loop) until active() is false.
Abort start(uint8_t duty_pct, uint32_t ms);
void poll();
// Duty 0 now. Safe from any context, any time.
void stop(Abort why = Abort::External);
bool active();
uint8_t duty();
Abort last_reason();

// Mark the outside world: a PD/fastcharge session, a sniff capture or a phone attached makes
// start() refuse and aborts a running pulse.
void set_blocked(bool blocked);

// Blocking probe: pulse, sampling the INA226 every ms; at most 200 ms.
Result probe(uint8_t duty_pct, uint32_t ms);

}  // namespace k2::load
