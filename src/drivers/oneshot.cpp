// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// One-shot microsecond timer, see k2/drivers/oneshot.hpp.
#include "k2/drivers/oneshot.hpp"

#include <cstddef>

#include "k2/core/critical.hpp"
#include "k2/core/hot.h"
#include "k2/drivers/regs.hpp"

namespace k2::oneshot {

void isr_body();

namespace {

// Basic timer register block (TMR6/TMR7 layout, STM32F1-style offsets).
struct Tmr {
    reg::R32 CTRL1, CTRL2;
    std::uint32_t r0;
    reg::R32 IDEN, ISTS, SWEVT;
    std::uint32_t r1[3];
    reg::R32 CVAL, DIV, PR;
    static constexpr std::uint32_t CTRL1_TMREN = 1u << 0;   // counter enable (cleared by hardware at the one-pulse overflow)
    static constexpr std::uint32_t CTRL1_OVFS = 1u << 2;    // only an overflow raises the update flag (not SWEVT)
    static constexpr std::uint32_t CTRL1_OCMEN = 1u << 3;   // one-pulse mode: stop at the overflow
    static constexpr std::uint32_t IDEN_OVFIEN = 1u << 0;
    static constexpr std::uint32_t ISTS_OVFIF = 1u << 0;
    static constexpr std::uint32_t SWEVT_OVFSWTR = 1u << 0;  // reload DIV/PR and clear the counter
};
static_assert(offsetof(Tmr, IDEN) == 0x0C && offsetof(Tmr, CVAL) == 0x24 && offsetof(Tmr, PR) == 0x2C);
inline Tmr& tmr7() { return reg::at<Tmr>(0x40001400); }

constexpr std::uint32_t kApb1EnTmr7 = 1u << 5;
constexpr unsigned kIrq = 55;                  // TMR7 global
constexpr std::uint32_t kTimerHz = 240'000'000;   // APB1 /2 = 120 MHz, timer clock x2 (src/drivers/crm.cpp)

bool g_ready;
Handler volatile g_handler;
volatile std::uint32_t g_fired;

void setup() {
    reg::crm().APB1EN |= kApb1EnTmr7;
    (void)reg::crm().APB1EN;   // the clock is on before the first register write
    Tmr& t = tmr7();
    t.CTRL1 = Tmr::CTRL1_OVFS | Tmr::CTRL1_OCMEN;
    t.DIV = kTimerHz / 1'000'000 - 1;   // 1 us per count
    t.IDEN = Tmr::IDEN_OVFIEN;
    t.ISTS = 0;
    reg::nvic().ISER[kIrq / 32] = 1u << (kIrq % 32);
    g_ready = true;
}

}  // namespace

void arm_us(std::uint32_t us, Handler h) {
    if (us < kMinUs) us = kMinUs;
    if (us > kMaxUs) us = kMaxUs;
    Critical cs;   // a main-loop arm and the interrupt's own re-arm never interleave
    if (!g_ready) setup();
    Tmr& t = tmr7();
    t.CTRL1 = Tmr::CTRL1_OVFS | Tmr::CTRL1_OCMEN;   // stopped
    g_handler = h;
    t.PR = us - 1;                  // overflow after `us` counts
    t.SWEVT = Tmr::SWEVT_OVFSWTR;   // load DIV/PR, counter to 0 (no flag: OVFS)
    t.ISTS = 0;
    reg::nvic().ICPR[kIrq / 32] = 1u << (kIrq % 32);
    t.CTRL1 = Tmr::CTRL1_OVFS | Tmr::CTRL1_OCMEN | Tmr::CTRL1_TMREN;
}

void cancel() {
    if (!g_ready) return;
    Critical cs;
    Tmr& t = tmr7();
    t.CTRL1 = Tmr::CTRL1_OVFS | Tmr::CTRL1_OCMEN;
    t.ISTS = 0;
    reg::nvic().ICPR[kIrq / 32] = 1u << (kIrq % 32);
    g_handler = nullptr;
}

std::uint32_t fired() { return g_fired; }

K2_HOT void isr_body() {
    Tmr& t = tmr7();
    if (!(t.ISTS & Tmr::ISTS_OVFIF)) return;
    t.ISTS = 0;
    g_fired = g_fired + 1;
    const Handler h = g_handler;
    if (h) h();   // it may arm the next shot
}

}  // namespace k2::oneshot

extern "C" K2_HOT void TMR7_GLOBAL_IRQHandler() { k2::oneshot::isr_body(); }
