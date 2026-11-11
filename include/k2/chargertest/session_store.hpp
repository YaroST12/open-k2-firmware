// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Backup-register store for the PD session record (pure layout in k2/chargertest/session.hpp).
//
// Backup registers (BPR) keep their content through SYSRESETREQ, IWDG and NRST resets, and through a
// supply dip as long as VBAT/VDD does not go away (no battery here: a real power loss clears them,
// the record then fails its CRC and is ignored). Owns the PWR/BPR clocks and the write-protect bit.
#pragma once
#include "k2/chargertest/session.hpp"

namespace k2::session {

// Enables the BPR, loads and logs the previous record ([SESS] line), counts this boot. Call after
// usb::init so the line reaches the log ring.
void init();

// Main loop: samples the PD driver (caps, contract, Identify progress) and writes the record when it
// changed, at most every kMinGapMs.
void poll();

// The backup domain kept the previous boot's record through this reset (a valid record in DR22..DR37 now). Every boot writes
// one (init), so this is true after a warm reset or a short VBUS dark and false after a real power loss. Measured 2026-11-11
// (runs/2026-11-11-iq4-bridge): a 1 s dark kept it while .noinit SRAM lost all 32 probe words. Side-effect free apart from the
// BPR clock enable; callable before init() (the Rd intent marker asks it before the Rd is decided).
bool backup_alive();

// The record found at boot, if it was valid.
const std::optional<Record>& previous();

// Boots counted since the backup registers last lost power (the report cards stamp it).
std::uint16_t boots();

// Zeroes this store's own registers (DR22..DR37, not the boot
// trace's) and stops poll() from writing them again. For the factory wipe, just before DFU.
void wipe();

inline constexpr std::uint32_t kMinGapMs = 100;

}  // namespace k2::session
