// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Safety rules of the PA8 test-load PWM (k2::load), pure logic so the host tests cover them.
// PA8 (TIM1 CH1) is pulsed for about 150 ms before the VOOC wake train; what sits behind
// it is unverified, so every pulse is bounded: duty <= 95 %, <= 200 ms, VBUS inside a window and
// MCU temperature <= 70 C for the whole pulse. Any trip forces the duty to 0.
#pragma once
#include <cstdint>

namespace k2::load {

inline constexpr uint8_t kMaxDutyPct = 95;
inline constexpr uint32_t kMaxMs = 200;
inline constexpr int32_t kMinVbusMv = 4500;    // dip below this aborts
inline constexpr int32_t kMaxVbusMv = 6000;    // the load is applied only below 6.0 V
inline constexpr int32_t kMaxMcuDeciC = 700;

enum class Abort : uint8_t { None, Timeout, VbusLow, VbusHigh, Temp, External, Done };

struct Inputs {
    int32_t vbus_mv = 0;
    int32_t mcu_deci_c = 0;
    bool mcu_valid = false;
    bool external_stop = false;   // BACK, USB disconnect, fault, a PD/fc session or a sniff armed
};

// Why a pulse must not start (Abort::None = allowed).
constexpr Abort start_check(uint8_t duty, uint32_t ms, const Inputs &in) {
    if (in.external_stop) return Abort::External;
    if (duty == 0 || duty > kMaxDutyPct || ms == 0 || ms > kMaxMs) return Abort::External;
    if (in.vbus_mv < kMinVbusMv) return Abort::VbusLow;
    if (in.vbus_mv >= kMaxVbusMv) return Abort::VbusHigh;
    if (in.mcu_valid && in.mcu_deci_c > kMaxMcuDeciC) return Abort::Temp;
    return Abort::None;
}

// One pulse: begin() once, then poll() often; poll returns the duty to apply (0 once finished).
class Pulse {
public:
    Abort begin(uint8_t duty, uint32_t ms, uint32_t now, const Inputs &in) {
        const Abort a = start_check(duty, ms, in);
        if (a != Abort::None) { reason_ = a; duty_ = 0; return a; }
        duty_ = duty; end_ = now + ms; start_ = now; reason_ = Abort::None; active_ = true;
        return Abort::None;
    }
    uint8_t poll(uint32_t now, const Inputs &in) {
        if (!active_) return 0;
        if (in.external_stop) return stop(Abort::External);
        if (in.vbus_mv < kMinVbusMv) return stop(Abort::VbusLow);
        if (in.vbus_mv >= kMaxVbusMv) return stop(Abort::VbusHigh);
        if (in.mcu_valid && in.mcu_deci_c > kMaxMcuDeciC) return stop(Abort::Temp);
        if (static_cast<int32_t>(now - end_) >= 0) return stop(Abort::Done);
        return duty_;
    }
    uint8_t stop(Abort why) { if (active_) { active_ = false; reason_ = why; } duty_ = 0; return 0; }
    bool active() const { return active_; }
    Abort reason() const { return reason_; }
private:
    uint8_t duty_ = 0;
    uint32_t end_ = 0, start_ = 0;
    Abort reason_ = Abort::None;
    bool active_ = false;
};

}  // namespace k2::load
