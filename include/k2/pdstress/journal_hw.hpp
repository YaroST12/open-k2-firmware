// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD Stress journal's file: /ps/run.rec on the littlefs volume (src/pdstress/journal_hw.cpp). One file, replaced atomically by
// Fs::write_file (written to a .tmp, then renamed), so a power loss in the middle of a write leaves the old record. A write holds the
// main loop 5 to 250 ms: the session only calls it at a sequence boundary on a quiet link (pdstress/journal.hpp). The host tests bind
// the same seam to memory (tests/ui_stubs.cpp).
#pragma once

#include "k2/pdstress/journal.hpp"

namespace k2::pdstress::journal_hw {

inline constexpr const char* kDir = "/ps";
inline constexpr const char* kFile = "/ps/run.rec";

// The record store; one object for the life of the firmware. A write slower than 150 ms is named in the log.
journal::Store& store();

}  // namespace k2::pdstress::journal_hw
