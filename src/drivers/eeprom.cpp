// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The 24C16 settings EEPROM, see k2/drivers/eeprom.hpp. The store is always built; the lab parts follow their switches.
#include "k2/drivers/eeprom.hpp"

#include <bit>

#include "k2/drivers/i2c.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::eeprom {

#if K2_EEPROM_READ
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
#endif  // K2_EEPROM_READ

namespace {
// The chip of every write: the INA bus, select 0, one-byte word address, linear offsets 0x000..0x7FF (locate() gives the device
// address and word). Callers are page_put() and cal_clear() (pure, eeprom_calc.hpp), so a write is always inside one page.
struct InaChip {
    void (*service)();
    bool read(std::uint16_t off, std::span<std::uint8_t> out) {
        Addr a;
        if (!locate(1, 0, off, out.size(), a)) return false;
        return i2c::read_word(i2c::Bus::Ina, a.dev7, {a.word, 1}, out);
    }
    bool write(std::uint16_t off, std::span<const std::uint8_t> data) {
        Addr a;
        if (!locate(1, 0, off, data.size(), a) || (off % kPage) + data.size() > kPage) return false;
        return i2c::write(i2c::Bus::Ina, a.dev7, a.word[0], data);   // [S dev+W word data... P]: one page write
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

int store_read(Slot s, std::span<std::uint8_t> out) {
    InaChip c{nullptr};
    Page p{};
    if (!c.read(slot_off(s), std::span<std::uint8_t>(p))) return -1;
    return slot_decode(s, p, out);
}

PageErr store_write(Slot s, std::span<const std::uint8_t> payload, void (*service)()) {
    const std::uint16_t off = slot_off(s);
    if (!in_store(off)) return PageErr::Refused;
    InaChip c{service};
    return page_put(c, off, nullptr, slot_encode(s, payload)).err;
}

#if K2_EEPROM_WRITE
PageResult page_cas(std::uint16_t off, const Page &expected, const Page &next, void (*service)()) {
    InaChip c{service};
    return page_put(c, off, &expected, next);
}
#endif

#if K2_EEPROM_CAL_CLEAR
CalResult cal_clear_ina(std::span<const std::uint8_t, kCalLen> expected, void (*service)()) {
    InaChip c{service};
    return cal_clear(c, expected);
}
#endif  // K2_EEPROM_CAL_CLEAR

}  // namespace k2::eeprom
