// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Read-only lab dump of a 24Cxx EEPROM, see k2/drivers/eeprom.hpp. Compiled only with K2_EEPROM_READ=1.
#include "k2/drivers/eeprom.hpp"

#include <bit>

#include "k2/drivers/i2c.hpp"

namespace k2::eeprom {

namespace {
bool bus_of(std::uint8_t bus, i2c::Bus &out) {
    if (bus > 1) return false;
    out = bus ? i2c::Bus::Ina : i2c::Bus::Fusb;
    return true;
}
}  // namespace

Err probe(std::uint8_t bus, Probe &out) {
    i2c::Bus b;
    if (!bus_of(bus, b)) return Err::Nack;
    out = {};
    for (std::uint8_t i = 0; i < kSlots; i++)
        if (i2c::probe(b, kBase7 + i)) out.ack_mask |= std::uint8_t(1u << i);
    if (!out.ack_mask) return Err::Ok;
    out.dev7 = std::uint8_t(kBase7 + std::countr_zero(out.ack_mask));
    const std::uint8_t zero = 0;
    for (std::size_t r = 0; r < kProbeReads; r++)
        i2c::read_word(b, out.dev7, {&zero, 1}, std::span<std::uint8_t>(out.sample + r * kProbeBytes, kProbeBytes));
    return Err::Ok;
}

Err read(std::uint8_t bus, const Addr &a, std::span<std::uint8_t> out) {
    i2c::Bus b;
    if (!bus_of(bus, b)) return Err::Nack;
    return i2c::read_word(b, a.dev7, {a.word, a.nword}, out) ? Err::Ok : Err::Nack;
}

}  // namespace k2::eeprom
