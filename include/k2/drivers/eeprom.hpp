// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lab access to an external 24Cxx I2C EEPROM (8-bit address 0xA0 = 7-bit 0x50..0x57).
// K2_EEPROM_READ=1: a read-only dump. The bus layer offers an address-only probe and a random-address read, nothing that sends a
// data byte to the chip. K2_EEPROM_CAL_CLEAR=1 (default 0, one unit's repair) adds exactly one write: cal_clear_ina() below.
// The addressing rules and the repair sequence are pure code in k2/drivers/eeprom_calc.hpp (host-tested).
#pragma once
#include <cstdint>
#include <span>

#include "k2/core/build_config.h"
#include "k2/drivers/eeprom_calc.hpp"

namespace k2::eeprom {

// bus: 0 = the FUSB302 bus (PB10/PB11), 1 = the power-monitor bus (PB8/PB9). Err::Nack when the bus argument is unknown.
// Probe: address-only writes to 0x50..0x57, then four 1-byte-address reads of 4 bytes at word address 0 from the lowest device
// that answered (the host tells a one-byte chip from a two-byte chip by whether the windows repeat).
Err probe(std::uint8_t bus, Probe &out);

// Random-address read of out.size() bytes at the address `a` (from eeprom::locate). Err::Nack = the chip did not answer.
Err read(std::uint8_t bus, const Addr &a, std::span<std::uint8_t> out);

#if K2_EEPROM_CAL_CLEAR
// The calibration repair (see cal_clear in eeprom_calc.hpp): on the INA bus, device 0x50, writes zeros to word 0x04..0x0F only
// when those 12 bytes read back equal to `expected`. Blocks up to kCalWriteMs while the part's write cycle ends, calling
// `service` (when set) about every 200 us so the caller can keep its own polling alive. No other write exists.
CalResult cal_clear_ina(std::span<const std::uint8_t, kCalLen> expected, void (*service)());
#endif

}  // namespace k2::eeprom
