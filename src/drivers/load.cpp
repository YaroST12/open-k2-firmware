// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/drivers/load.hpp"
#include "k2/drivers/power_calc.hpp"

#include "k2/drivers/adc.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::load {

using namespace reg;

namespace {

// TIM1 (advanced timer, APB2): STM32F1 register layout.
struct Tim {
    R32 CTRL1, CTRL2, STCTRL, IDEN, ISTS, SWEVT, CM1, CM2, CCTRL, CVAL, DIV, PR, RPR, C1DT, C2DT, C3DT, C4DT, BRK;
};
static_assert(offsetof(Tim, C1DT) == 0x34 && offsetof(Tim, BRK) == 0x44);
inline Tim &tim1() { return at<Tim>(0x40012C00); }

constexpr uint32_t kApb2Tim1 = 1u << 11;
constexpr uint32_t kTimerHz = 240'000'000;   // APB2 120 MHz with a prescaler != 1 doubles the timer clock
constexpr uint32_t kPwmHz = 80'000;           // 80 kHz switching
constexpr uint32_t kPeriod = 100;             // ARR 99
constexpr uint32_t kDiv = kTimerHz / (kPwmHz * kPeriod);   // 30

Pulse g_pulse;
volatile bool g_active;
volatile uint32_t g_deadline_ms;
volatile uint8_t g_duty;
bool g_blocked;
Abort g_last = Abort::None;

void hw_off() {
    tim1().C1DT = 0;
    pins::load_pwm.clear();
    pins::load_pwm.mode(Mode::OutPp50);   // plain low output, the timer no longer drives it
    g_duty = 0;
    g_active = false;
}

void hw_on(uint8_t duty) {
    tim1().C1DT = duty;
    pins::load_pwm.mode(Mode::AfPp50);
    g_duty = duty;
}

void tick_hook() {   // SysTick ISR: the hard timeout, independent of the main loop
    if (g_active && static_cast<int32_t>(systick::millis() - g_deadline_ms) >= 0) hw_off();
}

Inputs inputs() {
    Inputs in;
    if (const auto pm = power::sample_now()) in.vbus_mv = pm->vbus_mv;
    if (const auto a = adc::read()) { in.mcu_deci_c = a->mcu.deci_c; in.mcu_valid = a->mcu.valid; }
    in.external_stop = g_blocked || !usb::configured();   // a unplugged USB host aborts a probe
    return in;
}

}  // namespace

void init() {
    crm().APB2EN |= Crm::APB2EN_GPIOA | kApb2Tim1;
    Tim &t = tim1();
    t.CTRL1 = 0;
    t.DIV = kDiv - 1;
    t.PR = kPeriod - 1;
    t.C1DT = 0;
    t.CM1 = (t.CM1 & ~0xFFu) | (6u << 4) | (1u << 3);   // CH1 PWM mode 1, output compare preload
    t.CCTRL = 1u;                                       // CH1 output enable, active high
    t.BRK = 1u << 15;                                   // main output enable (advanced timer)
    t.SWEVT = 1u;                                       // load the prescaler
    t.CTRL1 = (1u << 7) | 1u;                           // ARR preload, counter enable
    pins::load_pwm.clear();
    pins::load_pwm.mode(Mode::OutPp50);
    systick::set_tick_hook(tick_hook);
}

Abort start(uint8_t duty_pct, uint32_t ms) {
    const uint32_t now = systick::millis();
    const Abort a = g_pulse.begin(duty_pct, ms, now, inputs());
    if (a != Abort::None) { g_last = a; return a; }
    // Pin on first, guard armed last: a SysTick hw_off() between the two must find the pin
    // already on, never be undone by a later hw_on() with the guard disarmed (all volatile, so
    // the compiler keeps this order).
    g_deadline_ms = now + ms;
    hw_on(duty_pct);
    g_active = true;
    return Abort::None;
}

void stop(Abort why) {
    hw_off();
    g_pulse.stop(why);
    g_last = why;
}

void poll() {
    if (!g_active) return;
    const uint8_t d = g_pulse.poll(systick::millis(), inputs());
    if (d == 0) { hw_off(); g_last = g_pulse.reason(); }
}

bool active() { return g_active; }
uint8_t duty() { return g_duty; }
Abort last_reason() { return g_last; }
void set_blocked(bool blocked) { g_blocked = blocked; if (blocked && g_active) stop(Abort::External); }

Result probe(uint8_t duty_pct, uint32_t ms) {
    Result r;
    r.duty = duty_pct;
    const auto pm0 = power::sample_now();
    const auto ad0 = adc::read();
    if (pm0) { r.idle_ma = pm0->current_ma; r.vbus_before_mv = pm0->vbus_mv; r.min_vbus_mv = pm0->vbus_mv; }
    if (ad0) r.temp_before_deci_c = ad0->mcu.deci_c;
    r.reason = start(duty_pct, ms);
    if (r.reason != Abort::None) return r;
    const uint32_t t0 = systick::millis();
    r.peak_ma = r.idle_ma;
    uint32_t hi = 0, n = 0;
    while (g_active) {
        for (int k = 0; k < 64; k++) { hi += pins::load_pwm.read(); n++; }
        if (const auto pm = power::sample_now()) {
            const int32_t i = power::abs_sat(pm->current_ma);
            if (i > r.peak_ma) r.peak_ma = i;
            if (pm->vbus_mv < r.min_vbus_mv) r.min_vbus_mv = pm->vbus_mv;
        }
        poll();
        usb::service();
        systick::delay_us(500);
    }
    r.pin_high_pct = n ? static_cast<uint8_t>(hi * 100 / n) : 0;
    r.ms = static_cast<uint16_t>(systick::millis() - t0);
    r.reason = g_last;
    systick::delay_ms(20);   // let the averages settle before the after-read
    if (const auto ad = adc::read()) r.temp_after_deci_c = ad->mcu.deci_c;
    return r;
}

}  // namespace k2::load
