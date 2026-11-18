// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The external 24C16 settings EEPROM on the power-monitor bus (8-bit address 0xA0 = 7-bit 0x50..0x57).
// Always built: the settings store (store_read / store_write), one 16-byte page per slot inside 0x500..0x7DF; a write anywhere
// else is refused before the bus is touched. Lab parts behind switches:
//   K2_EEPROM_READ=1   the dump (probe / read on either bus);
//   K2_EEPROM_WRITE=1  page_cas(): the compare-and-swap page write a backup restore uses (any page, old bytes must match);
//   K2_EEPROM_CAL_CLEAR=1 (default 0, one unit's repair) cal_clear_ina().
// The addressing rules, the page write sequence and the slot format are pure code in k2/drivers/eeprom_calc.hpp (host-tested).
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

// The settings store. store_read: the slot's payload length (payload in out), or -1 when the slot is blank, damaged or the
// chip does not answer. store_write: writes the slot only when its page differs (PageErr::Same = nothing written); blocks up to
// kCalWriteMs for the write cycle, calling service (when set) about every 200 us.
int store_read(Slot s, std::span<std::uint8_t> out);
PageErr store_write(Slot s, std::span<const std::uint8_t> payload, void (*service)());

#if K2_EEPROM_WRITE
// The restore's page write: off must be an aligned page (0x000..0x7F0); written only when the page reads back equal to expected
// (Mismatch otherwise, nothing written) and differs from next (Same).
PageResult page_cas(std::uint16_t off, const Page &expected, const Page &next, void (*service)());
#endif

#if K2_EEPROM_CAL_CLEAR
// The calibration repair (see cal_clear in eeprom_calc.hpp): on the INA bus, device 0x50, writes zeros to word 0x04..0x0F only
// when those 12 bytes read back equal to `expected`. Blocks up to kCalWriteMs while the part's write cycle ends, calling
// `service` (when set) about every 200 us so the caller can keep its own polling alive.
CalResult cal_clear_ina(std::span<const std::uint8_t, kCalLen> expected, void (*service)());
#endif

}  // namespace k2::eeprom
