// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// SysTick time base: 1 ms interrupt from the 240 MHz core clock, plus busy-wait delays.
#include "k2/drivers/systick.hpp"

#include "k2/core/hot.h"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::systick {

namespace {
constexpr std::uint32_t kIcsrPendstset = 1u << 26;   // SCB ICSR: SysTick exception pending
volatile std::uint32_t g_millis;
std::uint32_t g_div = 1;   // the AHB divider SysTick was last programmed for (crm::set_hclk_div)
void (*g_hook)();
void (*g_wait_hook)();
void (*g_tick_hook)();
}

void set_delay_hook(void (*hook)()) { g_hook = hook; }
void set_wait_hook(void (*hook)()) { g_wait_hook = hook; }

void init() {
    auto& st = reg::systick();
    st.LOAD = kReload - 1;
    st.VAL = 0;
    st.CTRL = reg::SysTick::CTRL_CLKSOURCE_CORE | reg::SysTick::CTRL_TICKINT | reg::SysTick::CTRL_ENABLE;
}

void set_tick_hook(void (*hook)()) { g_tick_hook = hook; }

K2_HOT void tick() {
    g_millis = g_millis + 1;
    if (g_tick_hook) g_tick_hook();
}

K2_HOT std::uint32_t millis() { return g_millis; }

// Standard pattern: VAL and the ms count are read until no tick handler ran in between, and the
// SysTick pending bit covers a wrap the handler has not counted yet (VAL re-read after seeing it).
K2_HOT Sample sample() {
    Sample s;
    do {
        s.ms = g_millis;
        s.val = reg::systick().VAL;
        s.pending = reg::scb().ICSR & kIcsrPendstset;
        if (s.pending) s.val = reg::systick().VAL;
    } while (s.ms != g_millis);
    return s;
}

K2_HOT std::uint32_t ticks() { return ticks_of(sample(), g_div); }

K2_HOT std::uint32_t micros() { return micros_of(sample(), g_div); }

void wait_reload() {
    auto& st = reg::systick();
    std::uint32_t prev = st.VAL;
    for (std::uint32_t i = 0; i < 400'000; i++) {   // about 8 ms of loop at the full clock: far more than one tick
        const std::uint32_t v = st.VAL;
        if (v > prev) return;   // the counter wrapped to the reload value: an edge just passed
        prev = v;
    }
}

void set_div(std::uint32_t div) {
    g_div = div;
    auto& st = reg::systick();
    st.LOAD = reload_for(div) - 1;
    st.VAL = 0;   // the next clock reloads: a full millisecond of the new period starts now
}

std::uint32_t div() { return g_div; }

void delay_us(std::uint32_t us) {
    // 60 iterations per microsecond: empirical factor kept from the first LCD bring-up, so
    // every wait is a little longer than nominal; the LCD init timings rely on that margin.
    // The loop runs on the core clock: with the AHB divider raised it needs div times fewer iterations.
    volatile std::uint32_t count = us * 60 / g_div;
    if (us != 0 && count == 0) count = 1;
    while (count--) __asm volatile("nop");
}

void delay_ms(std::uint32_t ms) {
    while (ms--) {
        delay_us(1000);
        usb::service();
        if (g_hook) g_hook();
        if (g_wait_hook) g_wait_hook();
    }
}

}  // namespace k2::systick

#if K2_STACKPROBE
#include "k2/core/stack_probe.hpp"
extern "C" K2_HOT void SysTick_Handler() { k2::systick::tick(); k2::stackprobe::sample(); }   // the PSP probe's 1 kHz sample
#else
extern "C" K2_HOT void SysTick_Handler() { k2::systick::tick(); }
#endif
