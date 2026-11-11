// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Black Box diagnosis on the K2 (k2/pd/diag.hpp is the pure analyser). Every main-loop pass it reads the records
// pdtrace wrote since the last pass (a cursor on the live ring, so it keeps counting after the 8 KB ring wraps), adds
// the fresh power-monitor sample (power::read_raw()) to its power window and moves its clock. The screen, the remote
// (Cmd 0x72) and the CDC shell (`diag`) read the same report.
//
// PASSIVE: reading records and samples only; nothing here transmits or touches CC, VBUS or D+/D-.
#pragma once
#include <cstdint>
#include <span>

#include "k2/pd/diag.hpp"

namespace k2::pd::diag::live {

// Main loop (slow_tasks, after power::poll()): the new ring records, one power sample into the window, the clock.
void poll(std::uint32_t now_ms);

// The report of the session worth showing (the open one, else the last finished one), refreshed at most every
// kRefreshMs; a device twin whose identity matches the sink's (the device library, k2/core/storage.hpp) gives the need.
inline constexpr std::uint32_t kRefreshMs = 250;
const Report& report();

// What the report is about: no session yet, one open, or the last finished one.
enum class State : std::uint8_t { None = 0, Open = 1, Last = 2 };
State state();
std::uint16_t sessions();          // sessions finished since boot
std::uint32_t session_ms();        // the shown session's length so far (or in all)

// The one-line verdict ("PD link healthy, power short"); "No PD session yet" before any.
void verdict_text(std::span<char> out);

}  // namespace k2::pd::diag::live
