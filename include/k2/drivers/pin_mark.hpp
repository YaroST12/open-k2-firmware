// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The pin mark: one backup register (DR21, offset 0x68 of the BPR block) holding the boot Rd pin this firmware had when it
// reset (pure encoding in k2/core/bootmode.hpp). It tells a plain boot after a warm reset whether the K2 was charger-powered (a
// pin to keep) or HID-powered (nothing to keep). Not DR1..DR3 / DR11..DR18, not the boot trace's, not the
// session record's DR22..DR37. Owns its PWR/BPR clock and write-protect enables (idempotent: they may already be on).
#pragma once
#include <cstdint>

namespace k2::pinmark {

std::uint16_t read();                  // the raw word; 0 or garbage = no pin (bootmode::pin_from_mark decides)
void write(std::uint16_t raw);         // bootmode::pin_mark(pin)

}  // namespace k2::pinmark
