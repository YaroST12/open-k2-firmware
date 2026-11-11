// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The loop probe on the K2 (src/loopprobe/loop_probe_hw.cpp): the Env over the drivers and the last result on the littlefs
// volume, /lp/last.rec, replaced atomically by Fs::write_file. The host tests bind the same seams to memory and fixed readings
// (tests/ui_stubs.cpp).
#pragma once

#include "k2/loopprobe/loop_probe.hpp"

namespace k2::loopprobe::hw {

inline constexpr const char* kDir = "/lp";
inline constexpr const char* kFile = "/lp/last.rec";

// The drivers behind the runner: power monitor, ADC, USB lines, FUSB302 (link snapshot, e-marker sniff, the CC hold),
// the backlight and the D+/D- switch network. One object for the life of the firmware.
Env& env();
// The last result's file. A write holds the main loop 5 to 250 ms; the runner only writes at a step boundary.
Store& store();

// The stored file as bytes, for the remote's chunked read (Cmd::LoopProbe): its size (0 when there is none or it is larger
// than a record file can be) and up to out.size() bytes from `off`, returning how many were read.
std::uint16_t file_len();
std::size_t file_read(std::uint16_t off, std::span<std::uint8_t> out);

}  // namespace k2::loopprobe::hw
