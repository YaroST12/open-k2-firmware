// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// `debug cc-probe`: a passive measurement of what a FUSB302 SWITCHES0 value does to a CC line while a charger
// waits for a sink (Rp on CC, no Rd from us, VBUS 0). Pure rules, host-tested in tests/test_cc_probe.cpp; the wire
// sequence (snapshot, apply, sample, restore) lives in src/drivers/fusb302.cpp (fusb302::cc_probe).
//
// SWITCHES0 bit layout (the same names as k2/pd/pd_phy.hpp, inferred from the chip's register map):
//   bit0 PDWN1 (Rd on CC1), bit1 PDWN2, bit2 MEAS_CC1, bit3 MEAS_CC2, bit4 VCONN_CC1, bit5 VCONN_CC2,
//   bit6 PU_EN1, bit7 PU_EN2 (the chip's own Rp pull-ups, which the K2 never drives).
// The knob only closes a VCONN switch while PC13 is low. PC13 high is an Ra on the CC1 net, not a rail switch
// (k2/pd/emk_ra.hpp), and the 2026-11-11 runs showed ~2.87 V on a bare CC1 with PC13 low, so the chip's VCONN switch
// does source the pin. What it does to the CC line is the measurement (docs/K2_PORTS_SPEC.md, "The CC front end").
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/pd/pd_phy.hpp"

namespace k2::ccprobe {

constexpr std::uint16_t kMsMin = 10, kMsMax = 5000;   // hold time
constexpr std::uint16_t kModesKeep = 0xFFFF;           // `modes`: leave the PC13/14/15 pin modes as they are
constexpr std::int32_t kVbusLimitMv = 1000;            // the probe runs below it and aborts above it
constexpr std::uint8_t kStatus0VbusOk = 1u << 7;       // STATUS0: VBUS above the chip's ~4 V comparator
constexpr std::uint8_t kSw0PullUps = 0xC0;             // PU_EN1 | PU_EN2
constexpr std::uint8_t kSw0Measure = pd::phy::kMeasCc1 | pd::phy::kMeasCc2;
constexpr std::uint8_t kSw0Vconn = pd::phy::kVconnCc1 | pd::phy::kVconnCc2;
constexpr std::size_t kMaxSamples = 4;

struct Args {
    std::uint8_t sw0 = 0;               // raw SWITCHES0
    std::uint8_t pc = 0;                // PC13/14/15 levels: bit0 PC13, bit1 PC14, bit2 PC15 (as `debug cc-matrix`)
    std::uint16_t ms = 0;               // hold time, kMsMin..kMsMax
    std::uint16_t modes = kModesKeep;   // PC13/14/15 CRH nibbles (bits 0-3 / 4-7 / 8-11) or kModesKeep
};

enum class ArgError : std::uint8_t { None, Pc, Ms, Modes, PullUp, VconnRail, VconnPdwn };

// A CRH nibble this knob may write: the pin modes the firmware uses (analog, push-pull, floating, open drain,
// input with pull, alternate push-pull).
constexpr bool mode_ok(unsigned nibble) {
    return nibble == 0x0 || nibble == 0x3 || nibble == 0x4 || nibble == 0x7 || nibble == 0x8 || nibble == 0xB;
}

constexpr ArgError check_args(const Args& a) {
    if (a.pc > 7) return ArgError::Pc;
    if (a.ms < kMsMin || a.ms > kMsMax) return ArgError::Ms;
    if (a.modes != kModesKeep) {
        if (a.modes > 0xFFF) return ArgError::Modes;
        for (unsigned i = 0; i < 3; i++)
            if (!mode_ok((a.modes >> (4 * i)) & 0xF)) return ArgError::Modes;
    }
    if (a.sw0 & kSw0PullUps) return ArgError::PullUp;
    if (a.sw0 & kSw0Vconn) {
        // The rail control must be a driven low: PC13 level bit clear and, when the modes are written, still an output.
        if (a.pc & 1) return ArgError::VconnRail;
        if (a.modes != kModesKeep) {
            const unsigned m = a.modes & 0xF;
            if (m != 0x3 && m != 0x7) return ArgError::VconnRail;
        }
        // Never a pull-down and the VCONN switch on the same line.
        if ((a.sw0 & pd::phy::kVconnCc1) && (a.sw0 & pd::phy::kPdwn1)) return ArgError::VconnPdwn;
        if ((a.sw0 & pd::phy::kVconnCc2) && (a.sw0 & pd::phy::kPdwn2)) return ArgError::VconnPdwn;
    }
    return ArgError::None;
}

// What the driver knows about the K2's state right now.
struct Ctx {
    bool attached = false;        // an active CC line, or a sink inline
    bool contract = false;        // a PD contract or a sink policy state other than idle
    bool rd = false;              // any Rd on a CC line: the PD engine's, a boot-mode pin, or the chip's SWITCHES0 now
    bool engine_running = false;  // identify / probe / re-read / trigger prepare running
    bool vconn_on = false;        // the VCONN experiment holds the rail
    std::int32_t vbus_mv = -1;    // -1 = unknown
};

// Bus: the chip did not answer (never returned by refusal(); the driver reports it the same way).
enum class Refusal : std::uint8_t { None, Attached, Contract, Rd, Running, VbusUnknown, VbusHigh, Bus };

constexpr Refusal refusal(const Ctx& c) {
    if (c.attached) return Refusal::Attached;
    if (c.contract) return Refusal::Contract;
    if (c.rd) return Refusal::Rd;
    if (c.engine_running || c.vconn_on) return Refusal::Running;
    if (c.vbus_mv < 0) return Refusal::VbusUnknown;
    if (c.vbus_mv >= kVbusLimitMv) return Refusal::VbusHigh;
    return Refusal::None;
}

// During the hold: a charger turned VBUS on. The power monitor's reading (-1 = none) or the chip's own VBUS flag.
constexpr bool vbus_abort(std::int32_t vbus_mv, std::uint8_t status0) {
    return vbus_mv > kVbusLimitMv || (status0 & kStatus0VbusOk) != 0;
}

// Sample times after the apply: +1, +10 and +100 ms (only those before the end), then the end of the hold.
struct Plan {
    std::array<std::uint16_t, kMaxSamples> at_ms{};
    std::uint8_t n = 0;
};

constexpr Plan plan(std::uint16_t ms) {
    Plan p;
    for (const std::uint16_t t : {std::uint16_t(1), std::uint16_t(10), std::uint16_t(100)})
        if (t < ms) p.at_ms[p.n++] = t;
    p.at_ms[p.n++] = ms;
    return p;
}

// SWITCHES0 for measuring line 1 or 2: the requested value with exactly that line's measure switch.
constexpr std::uint8_t measure_sw0(std::uint8_t sw0, int line) {
    return std::uint8_t((sw0 & ~kSw0Measure) | (line == 1 ? pd::phy::kMeasCc1 : pd::phy::kMeasCc2));
}

// One sample. cc1_raw / cc2_raw: the ADC's newest 12-bit sweep (0xFFFF = none). vbus_mv: the power monitor (0xFFFF =
// none). st0_cc1 / st0_cc2: the chip's STATUS0 with the measure switch on that line (bits 1:0 BC_LVL, bit 5 COMP).
struct Sample {
    std::uint32_t t_us = 0;   // microseconds after the requested SWITCHES0 was written
    std::uint16_t cc1_raw = 0xFFFF, cc2_raw = 0xFFFF, vbus_mv = 0xFFFF;
    std::uint8_t st0_cc1 = 0, st0_cc2 = 0;
};

constexpr std::uint8_t kFlagAborted = 1;    // VBUS rose: the last sample is the abort sample, the hold ended early
constexpr std::uint8_t kFlagRestored = 2;   // SWITCHES0, SWITCHES1 and the PC13/14/15 levels read back as the snapshot

struct Result {
    std::uint8_t flags = 0;
    std::uint8_t n = 0;
    std::uint8_t sw0 = 0, sw1 = 0;   // the snapshot taken before the apply (what was restored)
    std::uint8_t pc = 0;             // PC13/14/15 levels of the snapshot
    std::uint16_t modes = 0;         // PC13/14/15 CRH nibbles of the snapshot
    std::array<Sample, kMaxSamples> s{};
};

}  // namespace k2::ccprobe
