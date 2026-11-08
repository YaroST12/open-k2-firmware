// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Bit-banged I2C masters on GPIOB. Two buses, both plain register-level GPIO toggling:
//   Fusb: PB10 SCL / PB11 SDA, open drain with external pull-ups, ~400 kHz, clock stretching.
//   Ina:  PB8  SCL / PB9  SDA, push-pull, SDA turned into a pull-up input to read; ~200 kHz.
// Register accesses are one transaction each: [S addr+W reg {data|Sr addr+R data}* P].
#pragma once
#include <cstdint>
#include <span>

namespace k2::i2c {

enum class Bus : std::uint8_t { Fusb = 0, Ina = 1 };

// Configure the two buses' own pins (FUSB302 SCL/SDA/INT_N, INA SCL/SDA), then recover both
// buses with 9 clock pulses and a STOP so a slave stuck mid-byte releases SDA.
void init();

// Write `data` to consecutive registers starting at `reg`. False on any NACK.
bool write(Bus bus, std::uint8_t dev, std::uint8_t reg, std::span<const std::uint8_t> data);

// Read `out.size()` bytes starting at `reg` (repeated start). False on NACK of the address
// or register byte; the last byte is NACKed as the protocol requires.
bool read(Bus bus, std::uint8_t dev, std::uint8_t reg, std::span<std::uint8_t> out);

inline bool write_reg(Bus bus, std::uint8_t dev, std::uint8_t reg, std::uint8_t val) {
    return write(bus, dev, reg, {&val, 1});
}
inline bool read_reg(Bus bus, std::uint8_t dev, std::uint8_t reg, std::uint8_t& val) {
    return read(bus, dev, reg, {&val, 1});
}

}  // namespace k2::i2c
