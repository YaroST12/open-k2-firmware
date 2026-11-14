// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The loop probe's Host report on the K2 (src/loopprobe/host_report_hw.cpp, built with K2_HOST_REPORT=1 only): the
// sequencer (k2/loopprobe/host_report.hpp) over the FUSB302's loop-host line (fusb302::LoopHost), the USB enumeration tap
// (usb::set_tap) and the journal on the littlefs volume, /lp/host.rec.
//
// Flow: arm() checks the loop (the start refusal) and saves the journal as Armed. The user then replugs the host end of the
// loop: the K2 reboots on the host's VBUS, poll()'s first call reads the journal (boot_decide), records the enumeration
// from that boot on, waits out the transient refusals (the host enumerating, the CC attach) and takes the line: the
// host's caps are acked, the sink asks PDO 1, the questions follow. The journal is saved before the DR_Swap and at the end.
// A reboot inside the run resumes it (at most kMaxBoots boots). The line stays owned after the run until the host goes
// away: letting Auto-CRC drop under a live contract would make the host Hard Reset, which cuts VBUS.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/loopprobe/host_report.hpp"

namespace k2::loopprobe::host::hw {

inline constexpr const char* kFile = "/lp/host.rec";
inline constexpr std::uint32_t kStartWaitMs = 6000;    // the transient refusals get this long after boot
inline constexpr std::uint32_t kUsbQuietMs = 1500;     // no enumeration event for this long = the host settled

// Main loop, every pass (after loopguard::poll). The first call reads the journal.
void poll(std::uint32_t now);
// The user's arm: Refuse::None = saved as Armed (replug the host now); anything else = refused, nothing saved.
Refuse arm();
// Ends an armed or running session (End::Cancelled); the line stays owned until the host goes away.
void cancel();
// The latest record (the file's at boot, the live one during a run) and whether a session is armed or running.
const Report& report();
bool active();
bool running();

// The stored file as bytes, for the remote's chunked read (Cmd::LoopProbe, selector 1).
std::uint16_t file_len();
std::size_t file_read(std::uint16_t off, std::span<std::uint8_t> out);

}  // namespace k2::loopprobe::host::hw
