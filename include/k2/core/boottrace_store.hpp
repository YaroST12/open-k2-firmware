// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Boot power trace driver (pure layout in k2/core/boottrace.hpp).
//
// From the first moment after the clocks, every 50 ms for the first 3 s: the power monitor's VBUS, the
// auxiliary rail and the CC1 / CC2 levels from the ADC, and the PLUS pin. Each sample goes into RAM
// (printed as "[BOOT] t=..ms vbus=.." lines once the log is up) and into the backup registers (the last
// seven samples), so a restart that keeps the backup supply prints "[SESS] last boot trace ..." at the
// next boot. Owns the backup registers DR4..DR10, DR19, DR20 and DR38..DR42 (BPR clock and write
// enable are shared with k2::session). Uses the ADC and power-monitor APIs, and the FUSB302's
// self_power_move() for the key-mode fallback below.
//
// Key-mode fallback: with no VBUS 600 ms after reset, no Rp on the pinned line and an Rp on the other one, the single Rd moves
// to that line, once (bootmode::moved_pin). Never Rd on both lines: that pair latches a source's Rp off (hardware 2026-11-06).
#pragma once
#include <cstddef>

#include "k2/core/bootinfo.hpp"
#include "k2/core/bootmode.hpp"
#include "k2/core/boottrace.hpp"

namespace k2::boottrace {

// After i2c::init() (and fusb302::self_power_early in a self-powered mode): starts the ADC scan and
// the power monitor, loads the previous trace from the backup registers, hooks the sampler into
// systick::delay_ms() and takes the first sample. Silent on the log (it is not up yet).
void init(bootmode::Mode mode);

// Sampler; called from the delay hook and the main loop. Cheap when no sample is due.
void poll();

// Main loop: prints up to two queued "[BOOT] t=.." lines per call.
void flush_log();

// Once the log is up: "[SESS] last boot trace ..." lines for the trace the previous boot left.
void report_previous();

// This boot's samples (one per kPeriodMs, up to kSpanMs), kept in RAM for the whole run: `bootinfo` reads them. sample_at(i)
// is the sample taken at i * kPeriodMs; out of range gives an empty one.
std::size_t sample_count();
Sample sample_at(std::size_t i);

// The key mode's one-time Rd move as the rule saw it (the inputs at that moment); moved = false when it did not happen.
bootinfo::Move move_record();

}  // namespace k2::boottrace
