// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The battery efficiency recorder on the K2 (src/battery/battery_hw.cpp): the one Service, its SRAM checkpoint in .noinit
// (kept by a warm reset, lost by a dark), the littlefs volume and the power monitor's latest sample. The host tests bind the
// same seam to a RAM volume and the stubbed monitor (tests/ui_stubs.cpp).
#pragma once

#include <cstdint>

#include "k2/battery/battery.hpp"

namespace k2::battery::hw {

// Once at boot, after the volume is mounted: resumes a run a warm reset interrupted, closes one a cold boot cut (its phase
// marked Lost) and logs what it found ("[BATT] boot ...").
void init(bool warm_reset);
// Every main-loop slow pass: the power monitor's latest sample into the run; the record write only when `may_write` (no
// charger test, PD Stress run or other port owner, and a quiet link).
void poll(std::uint32_t now_ms, bool may_write);
Service& service();
bool monitor_online();     // the power monitor answers: a run can start

}  // namespace k2::battery::hw
