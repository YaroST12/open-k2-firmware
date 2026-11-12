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
    case Id::Shell: return "shell";
    case Id::Msc: return "msc";
    case Id::Remote: return "remote";
    case Id::Capture: return "capture";
    case Id::Fctrig: return "fctrig";
    case Id::Transcap: return "transcap";
    case Id::Service: return "service";
    case Id::Buttons: return "buttons";
    case Id::Fusb302: return "fusb302";
    case Id::Power: return "power";
    case Id::PowerSource: return "psource";
    case Id::Dpsniff: return "dpsniff";
    case Id::Pdtrace: return "pdtrace";
    case Id::DiagLive: return "diaglive";
    case Id::Adc: return "adc";
    case Id::LoopGuard: return "loopguard";
    case Id::Accel: return "accel";
    case Id::Fastcharge: return "fastcharge";
    case Id::Boottrace: return "boottrace";
    case Id::Session: return "session";
    case Id::RdComeback: return "rdcomeback";
    case Id::Handback: return "handback";
    case Id::Rescan: return "rescan";
    case Id::Survey: return "survey";
    case Id::Bbpersist: return "bbpersist";
    case Id::ReportCard: return "reportcard";
    case Id::BootScreen: return "bootscreen";
    case Id::UiTick: return "ui";
    case Id::Stack: return "stack";
    case Id::Heartbeat: return "heartbeat";
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
