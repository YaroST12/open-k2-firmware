// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The silent-cable-chip probe: with a cable alone in the port (no charger, no Rd of ours, VBUS dark) and the e-marker
// not answering, sense the chip's pull-down (Ra, ~1 kohm) on the cable's supply pin. A chipless cable has none; a
// cable whose chip is there but silent has one on the line the K2's VCONN would power. Three phases of three raw ADC
// samples each (2, 10 and 100 ms after the phase starts):
//   idle   : nothing driven, the snapshot pin levels; the CC2 net idles at ~1.7 V (PLUS-key coupling) and an Ra on it collapses it
//   VCONN1 : the chip's VCONN switch on CC1 only (PC13 low): CC1 reads ~2.87 V bare, ~2.64 V through a cable Ra
//   VCONN2 : the same on CC2: ~3.27 V bare, ~2.64 V through a cable Ra
// This header is the pure part (plan, refusals, guards, wire result); the register work is fusb302::ra_probe() in
// src/drivers/fusb302.cpp and the verdict is k2/pd/ra_class.hpp. Host-tested in tests/test_ra_probe.cpp. The levels
// come from docs/K2_PORTS_SPEC.md ("The CC front end", measured 2026-11-11) and are tunable constants, not facts.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/pd/cc_probe.hpp"
#include "k2/pd/pd_phy.hpp"

namespace k2::raprobe {

constexpr std::uint8_t kPhaseIdle = 0, kPhaseVconn1 = 1, kPhaseVconn2 = 2, kPhases = 3;
constexpr std::uint8_t kSamplesPerPhase = 3;
constexpr std::size_t kMaxSamples = std::size_t(kPhases) * kSamplesPerPhase;
// Sample times inside a phase. The ADC's newest sweep is up to ~15 ms old, so the 2 ms sample can still be the sweep from
// before the phase began (a transient); 10 and 100 ms are the settled values.
constexpr std::array<std::uint16_t, kSamplesPerPhase> kSampleMs = {2, 10, 100};
constexpr std::uint16_t kSettleMs = 30;     // between the two VCONN phases, switch open and pins back, for the pin to discharge
constexpr std::uint8_t kVconnPcDefault = 0; // PC13/PC14/PC15 levels during the VCONN phases: the condition of the 2026-11-11 numbers

constexpr std::int32_t kVbusLimitMv = ccprobe::kVbusLimitMv;   // the probe runs below it and aborts above it
constexpr std::uint8_t kStatus0VbusOk = ccprobe::kStatus0VbusOk;

// ADC counts (12 bit, 3.3 V full scale, no divider: the pins read the net directly).
constexpr std::uint16_t kNoRaw = 0xFFFF;
constexpr std::int32_t raw_to_mv(std::uint16_t raw) { return raw == kNoRaw ? -1 : std::int32_t(std::uint32_t(raw) * 3300u / 4096u); }

// The idle levels that tell a source from a cable (a cable alone, Rd-less). CC1 has no bias: it idles near 0 and a cable
// Ra keeps it there. CC2 idles at the PLUS-coupling bias (1.67-1.71 V measured) or, with a cable Ra on it, near 0. A level
// anywhere else is an Rp (an open Rp reads the ADC ceiling, an Rp over a cable Ra 0.4-1.4 V): VCONN must not go on it.
constexpr std::int32_t kSourceLowMaxMv = 400;     // at or above on CC1, or in 400..kBiasMinMv-1 on CC2: a source
constexpr std::int32_t kBiasMinMv = 1400;         // the CC2 bias band
constexpr std::int32_t kBiasMaxMv = 2000;

constexpr bool source_level(std::int32_t cc1_mv, std::int32_t cc2_mv) {
    if (cc1_mv >= kSourceLowMaxMv) return true;
    if (cc2_mv >= kSourceLowMaxMv && (cc2_mv < kBiasMinMv || cc2_mv > kBiasMaxMv)) return true;
    return false;
}

struct Args {
    std::uint8_t vconn_pc = kVconnPcDefault;   // PC13/14/15 levels for the VCONN phases: bit0 PC13, bit1 PC14, bit2 PC15
};

enum class ArgError : std::uint8_t { None, Pc, VconnRail };

constexpr ArgError check_args(const Args &a) {
    if (a.vconn_pc > 7) return ArgError::Pc;
    if (a.vconn_pc & 1) return ArgError::VconnRail;   // PC13 high is an Ra on the CC1 net: never together with VCONN
    return ArgError::None;
}

// What the driver knows about the K2's state right now. Not `active_cc`: an empty port latches active_cc = 2 on its CC2 bias.
struct Ctx {
    bool inline_sink = false;     // a sink downstream
    bool contract = false;        // a PD contract or a sink policy state other than idle
    bool rd = false;              // any Rd on a CC line: the PD engine's, a boot-mode pin, or the chip's SWITCHES0 now
    bool ra_mode = false;         // the Ra mode (PC13 held high) is on
    bool engine_running = false;  // identify / probe / re-read / trigger prepare running
    bool vconn_on = false;        // the VCONN experiment holds the rail
    bool vbus_flag = false;       // the chip's VBUS comparator sees VBUS
    std::int32_t vbus_mv = -1;    // the power monitor, -1 = unknown
};

// None..Bus are decided before anything is driven; SourceLevel after the idle phase, before any VCONN.
enum class Refusal : std::uint8_t { None, Inline, Contract, Rd, RaMode, Running, VbusUnknown, VbusHigh, Bus, SourceLevel };

constexpr Refusal refusal(const Ctx &c) {
    if (c.inline_sink) return Refusal::Inline;
    if (c.contract) return Refusal::Contract;
    if (c.rd) return Refusal::Rd;
    if (c.ra_mode) return Refusal::RaMode;
    if (c.engine_running || c.vconn_on) return Refusal::Running;
    if (c.vbus_mv < 0) return Refusal::VbusUnknown;
    if (c.vbus_mv >= kVbusLimitMv || c.vbus_flag) return Refusal::VbusHigh;
    return Refusal::None;
}

constexpr bool vbus_abort(std::int32_t vbus_mv, std::uint8_t status0) { return ccprobe::vbus_abort(vbus_mv, status0); }

// The phases: SWITCHES0 and the PC13/14/15 levels each one runs under.
constexpr std::uint8_t phase_sw0(std::uint8_t phase, std::uint8_t snapshot_sw0) {
    return phase == kPhaseVconn1 ? std::uint8_t(pd::phy::kVconnCc1) : phase == kPhaseVconn2 ? std::uint8_t(pd::phy::kVconnCc2) : snapshot_sw0;
}
constexpr std::uint8_t phase_pc(std::uint8_t phase, std::uint8_t snapshot_pc, std::uint8_t vconn_pc) {
    return phase == kPhaseIdle ? snapshot_pc : vconn_pc;
}
constexpr std::size_t sample_index(std::uint8_t phase, std::uint8_t i) { return std::size_t(phase) * kSamplesPerPhase + i; }
// Whole probe length: three phases of the last sample time and one settle gap, between the two VCONN phases (the idle phase
// changes nothing, so VCONN1 starts straight after it; VCONN2 starts from a line the VCONN1 phase just drove).
constexpr std::uint32_t total_ms() { return std::uint32_t(kPhases) * kSampleMs[kSamplesPerPhase - 1] + kSettleMs; }

struct Sample {
    std::uint16_t cc1_raw = kNoRaw, cc2_raw = kNoRaw;
};

constexpr std::uint8_t kFlagAborted = 1;    // VBUS rose: the VCONN switch went off at once, the phases after it were not run
constexpr std::uint8_t kFlagRestored = 2;   // SWITCHES0, SWITCHES1 and the PC13/14/15 levels read back as the snapshot

// The probe's reply. `s` holds the samples in phase order (idle x3, VCONN1 x3, VCONN2 x3), n of them taken. A refusal
// carries only `refusal` (and, for SourceLevel, the idle samples that named it).
struct Result {
    std::uint8_t refusal = 0;        // Refusal
    std::uint8_t flags = 0;
    std::uint8_t n = 0;
    std::uint8_t sw0 = 0, sw1 = 0;   // the snapshot taken before the first change (what was restored)
    std::uint8_t pc = 0;             // PC13/14/15 levels of the snapshot
    std::uint8_t vconn_pc = 0;       // the levels the VCONN phases ran under
    std::uint16_t vbus_max_mv = kNoRaw;   // highest VBUS reading seen (monitor), kNoRaw = none
    std::array<Sample, kMaxSamples> s{};
};

}  // namespace k2::raprobe
