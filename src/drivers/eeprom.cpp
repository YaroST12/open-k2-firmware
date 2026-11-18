// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lab access to a 24Cxx EEPROM, see k2/drivers/eeprom.hpp. Compiled with K2_EEPROM_READ=1 and/or K2_EEPROM_CAL_CLEAR=1.
#include "k2/drivers/eeprom.hpp"

#include <bit>

#include "k2/core/log.hpp"
#include "k2/drivers/i2c.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::eeprom {

#if K2_EEPROM_READ
namespace {
CalInfo g_calibration;

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

void check_calibration(power::Chip chip) {
    std::uint8_t bytes[kCalLen] = {};
    const Addr a{kCalDev7, 1, {kCalWord, 0}};
    const bool present = read(1, a, bytes) == Err::Ok;       // one fixed 12-byte read, a bounded number of I2C bits
    g_calibration = classify_cal(chip, present, std::span<const std::uint8_t, kCalLen>(bytes));
    if (g_calibration.state == CalState::Corrupt)
        K2LOG("[CAL] stored trims corrupt v=%d iz=%d ig=%d\n",
              static_cast<int>(g_calibration.voltage_gain), static_cast<int>(g_calibration.current_zero),
              static_cast<int>(g_calibration.current_gain));
}

CalInfo calibration() { return g_calibration; }
#endif  // K2_EEPROM_READ

#if K2_EEPROM_CAL_CLEAR
namespace {
// The chip of the repair: the INA bus, device kCalDev7, one-byte word address. Nothing here takes an address, a length or a
// value from outside; write() is the only place a data byte leaves for an EEPROM, and cal_clear() hands it kCalWord and kCalZero.
struct CalChip {
    void (*service)();
    bool read(std::uint8_t word, std::span<std::uint8_t> out) {
        return i2c::read_word(i2c::Bus::Ina, kCalDev7, {&word, 1}, out);
    }
    bool write(std::uint8_t word, std::span<const std::uint8_t> data) {
        return i2c::write(i2c::Bus::Ina, kCalDev7, word, data);   // [S dev+W word data... P]: one page write inside the 16-byte page
    }
    // Acknowledge polling: the part NACKs its address until the internal write cycle ends. millis() counts whole ms, so a
    // difference above kCalWriteMs means more than kCalWriteMs really elapsed, and the last probe came right before the test.
    bool wait_ready() {
        const std::uint32_t t0 = systick::millis();
        for (;;) {
            if (i2c::probe(i2c::Bus::Ina, kCalDev7)) return true;
            if (systick::millis() - t0 > kCalWriteMs) return false;
            systick::delay_us(200);
            if (service) service();
        }
    }
};
}  // namespace

CalResult cal_clear_ina(std::span<const std::uint8_t, kCalLen> expected, void (*service)()) {
    CalChip c{service};
    return cal_clear(c, expected);
}
#endif  // K2_EEPROM_CAL_CLEAR

}  // namespace k2::eeprom
