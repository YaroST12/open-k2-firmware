// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Read-only lab dump of an external 24Cxx I2C EEPROM (8-bit address 0xA0 = 7-bit 0x50..0x57), built in with K2_EEPROM_READ=1.
// There is no write path: the bus layer offers an address-only probe and a random-address read, nothing that sends a data
// byte to the chip. The addressing rules are pure code in k2/drivers/eeprom_calc.hpp (host-tested).
#pragma once
#include <cstdint>
#include <span>

#include "k2/drivers/eeprom_calc.hpp"

namespace k2::eeprom {

// bus: 0 = the FUSB302 bus (PB10/PB11), 1 = the power-monitor bus (PB8/PB9). Err::Nack when the bus argument is unknown.
// Probe: address-only writes to 0x50..0x57, then four 1-byte-address reads of 4 bytes at word address 0 from the lowest device
// that answered (the host tells a one-byte chip from a two-byte chip by whether the windows repeat).
Err probe(std::uint8_t bus, Probe &out);

// Random-address read of out.size() bytes at the address `a` (from eeprom::locate). Err::Nack = the chip did not answer.
Err read(std::uint8_t bus, const Addr &a, std::span<std::uint8_t> out);

}  // namespace k2::eeprom
