// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Main-loop stage meter: which poll a main-loop stall happened in, and when. src/core/main_loop.cpp marks the start of each
// stage it runs (mark(next) also ends the stage before it; Idle covers the wfi between passes); the meter keeps the longest
// stage since reset() with its start (ms since the reset) and how far the millisecond clock moved meanwhile. Time comes from
// the caller as the core cycle counter (k2::dpdm::cycles(), runs with interrupts masked) and systick::millis() (does not:
// SysTick waits), so a stage whose real time is far longer than its millisecond advance ran with interrupts masked (a flash
// erase or write). Interrupt handlers count in the stage they interrupted. Instrumentation only: it changes no behaviour.
// The guarded SuperVOOC trigger (k2/fastcharge/svooc_trig.hpp) resets it at each wake and reports the worst stage.
#pragma once
#include <cstdint>
#include "k2/core/strpack.hpp"

namespace k2::stage {

enum class Id : std::uint8_t {
    None, Idle, Usb, Shell, Msc, Remote, Capture, Fctrig, Transcap, Service, Buttons, Fusb302, Power, PowerSource, Dpsniff,
    Pdtrace, DiagLive, Adc, LoopGuard, Accel, Fastcharge, Boottrace, Session, RdComeback, Handback, Rescan, Survey,
    Bbpersist, ReportCard, BootScreen, UiTick, Stack, Heartbeat,
    Count_
};

constexpr const char* name(Id id) {
    switch (id) {
    case Id::None: return "none";
    case Id::Idle: return "idle";
    case Id::Usb: return "usb";
    case Id::Shell: return K2_P("shell");
    case Id::Msc: return "msc";
    case Id::Remote: return K2_P("remote");
    case Id::Capture: return K2_P("capture");
    case Id::Fctrig: return K2_P("fctrig");
    case Id::Transcap: return K2_P("transcap");
    case Id::Service: return K2_P("service");
    case Id::Buttons: return K2_P("buttons");
    case Id::Fusb302: return K2_P("fusb302");
    case Id::Power: return K2_P("power");
    case Id::PowerSource: return K2_P("psource");
    case Id::Dpsniff: return K2_P("dpsniff");
    case Id::Pdtrace: return K2_P("pdtrace");
    case Id::DiagLive: return K2_P("diaglive");
    case Id::Adc: return "adc";
    case Id::LoopGuard: return K2_P("loopguard");
    case Id::Accel: return K2_P("accel");
    case Id::Fastcharge: return K2_P("fastcharge");
    case Id::Boottrace: return K2_P("boottrace");
    case Id::Session: return K2_P("session");
    case Id::RdComeback: return K2_P("rdcomeback");
    case Id::Handback: return K2_P("handback");
    case Id::Rescan: return K2_P("rescan");
    case Id::Survey: return K2_P("survey");
    case Id::Bbpersist: return K2_P("bbpersist");
    case Id::ReportCard: return K2_P("reportcard");
    case Id::BootScreen: return K2_P("bootscreen");
    case Id::UiTick: return "ui";
    case Id::Stack: return K2_P("stack");
    case Id::Heartbeat: return K2_P("heartbeat");
    case Id::Count_: break;
    }
    return "?";
}

inline constexpr std::uint32_t kCyclesPerUs = 240;

struct Worst {
    Id id = Id::None;
    std::uint32_t us = 0;          // its length (cycle counter)
    std::uint32_t at_ms = 0;       // its start, ms since reset() (0 if it began before)
    std::uint32_t ticked_ms = 0;   // how far millis() moved during it
};

class Meter {
public:
    void reset(std::uint32_t cyc, std::uint32_t ms) {
        worst_ = {};
        win_ms_ = ms;
        if (cur_ != Id::None) { start_cyc_ = cyc; start_ms_ = ms; }
    }
    void mark(Id next, std::uint32_t cyc, std::uint32_t ms) {
        if (cur_ != Id::None) {
            const std::uint32_t us = (cyc - start_cyc_) / kCyclesPerUs;
            if (us > worst_.us) worst_ = {cur_, us, start_ms_ - win_ms_ < 0x80000000u ? start_ms_ - win_ms_ : 0u, ms - start_ms_};
        }
        cur_ = next;
        start_cyc_ = cyc;
        start_ms_ = ms;
    }
    Worst worst() const { return worst_; }

private:
    Id cur_ = Id::None;
    std::uint32_t start_cyc_ = 0, start_ms_ = 0, win_ms_ = 0;
    Worst worst_{};
};

inline Meter meter;

}  // namespace k2::stage
