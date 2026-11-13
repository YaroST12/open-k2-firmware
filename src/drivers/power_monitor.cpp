// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Power monitor driver, see k2/drivers/power_monitor.hpp.
#include "k2/core/hot.h"
#include "k2/core/log.hpp"
#include "k2/drivers/power_monitor.hpp"

#include "k2/drivers/i2c.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::power {

namespace {

// Register map shared by the three chips for the registers used here.
enum Reg : std::uint8_t {
    kConfig = 0x00,
    kAdcConfig = 0x01,    // INA228 / INA237
    kIna226Shunt = 0x01,
    kIna226Bus = 0x02,
    kVshunt = 0x04,       // INA228 / INA237
    kVbus = 0x05,
    kMfgId = 0x3E,        // INA228/INA237 manufacturer ID (INA237 reads 0x5449 "TI")
    kDieId = 0x3F,        // INA228: 0x228x
    kIna226DieId = 0xFF,  // INA226: 0x226x
};

constexpr std::uint16_t kIdTi = 0x5449;

Chip g_chip = Chip::None;
std::uint16_t g_die_id;
Average g_avg_v, g_avg_i;
std::int32_t g_fast_raw;    // VBUS of the latest sample, not averaged
std::int32_t g_raw_ma;      // forward current of the latest sample, not averaged, no dead band
bool g_fast_valid;
smoothing::Readout g_disp;  // One Euro filters behind read_display()
std::uint32_t g_last_probe_ms;
std::uint32_t g_init_ms;
bool g_fast;   // transition capture's fast VBUS mode is on (INA226: 0x0007 instead of 0x4407)

K2_HOT bool read16(std::uint8_t reg, std::uint16_t& out) {
    std::uint8_t b[2];
    if (!i2c::read(i2c::Bus::Ina, kI2cAddr, reg, b)) return false;
    out = static_cast<std::uint16_t>(b[0] << 8 | b[1]);
    return true;
}

K2_HOT bool write16(std::uint8_t reg, std::uint16_t val) {
    const std::uint8_t b[2] = {std::uint8_t(val >> 8), std::uint8_t(val)};
    return i2c::write(i2c::Bus::Ina, kI2cAddr, reg, b);
}

// Big-endian 2- or 3-byte register.
K2_HOT bool read_reg(Chip chip, std::uint8_t reg, std::uint32_t& out) {
    std::uint8_t b[3];
    const std::size_t n = static_cast<std::size_t>(reg_bytes(chip));
    if (!i2c::read(i2c::Bus::Ina, kI2cAddr, reg, {b, n})) return false;
    out = n == 3 ? std::uint32_t{b[0]} << 16 | std::uint32_t{b[1]} << 8 | b[2]
                 : std::uint32_t{b[0]} << 8 | b[1];
    return true;
}

bool chip_setup(Chip chip) {
    switch (chip) {
    case Chip::Ina228:
        return write16(kConfig, 0x0010)        // ADCRANGE = 1, +-40.96 mV
            && write16(kAdcConfig, 0xBFF8);    // continuous VBUS+VSHUNT, 1052 us, no averaging
    case Chip::Ina237:
        if (!write16(kConfig, 0x8000)) return false;  // reset
        systick::delay_ms(10);
        return write16(kConfig, 0x0010)        // ADCRANGE = 1
            && write16(kAdcConfig, 0xB492);
    case Chip::Ina226:
        return write16(kConfig, 0x8000)        // reset
            && write16(kConfig, 0x4407);       // 16 averages, continuous
    default:
        return false;
    }
}

bool detect() {
    std::uint16_t id;
    Chip found = Chip::None;
    if (read16(kDieId, id) && (id >> 4) == 0x0228) found = Chip::Ina228;
    else if (read16(kIna226DieId, id) && (id >> 4) == 0x0226) found = Chip::Ina226;
    else if (read16(kMfgId, id) && id == kIdTi) found = Chip::Ina237;
    if (found == Chip::None || !chip_setup(found)) return false;

    g_chip = found;
    g_fast = false;
    g_die_id = id;
    g_avg_v.reset();
    g_avg_i.reset();
    g_disp.reset();
    g_fast_valid = false;
    K2LOG("[PWR] detected %s id=%X\n", name(found), static_cast<unsigned>(id));
    return true;
}

constexpr std::uint16_t kIna226Normal = 0x4407;   // 16 averages, 140 us, continuous shunt + bus
constexpr std::uint16_t kIna226Fast = 0x0007;     // 1 average, 140 us, continuous shunt + bus

}  // namespace

bool init() {
    g_chip = Chip::None;
    g_init_ms = g_last_probe_ms = systick::millis();
    if (detect()) return true;
    K2LOG("[PWR] no INA228/INA226/INA237 at 0x40\n");
    return false;
}

K2_HOT Chip chip() { return g_chip; }

K2_HOT std::int32_t max_vbus_mv() { return max_vbus_mv(g_chip); }

K2_HOT void poll() {
    if (g_chip == Chip::None) {
        const std::uint32_t now = systick::millis();
        if (now - g_last_probe_ms >= probe_period_ms(now - g_init_ms)) {
            g_last_probe_ms = now;
            detect();
        }
        return;
    }
    const bool ina226 = g_chip == Chip::Ina226;
    std::uint32_t reg_i, reg_v;
    if (!read_reg(g_chip, ina226 ? kIna226Shunt : kVshunt, reg_i) ||
        !read_reg(g_chip, ina226 ? kIna226Bus : kVbus, reg_v))
        return;  // a failed sample is skipped; the window keeps the old values
    g_avg_i.push(align_shunt(g_chip, reg_i));
    g_avg_v.push(align_vbus(g_chip, reg_v));
    g_fast_raw = align_vbus(g_chip, reg_v);
    g_raw_ma = forward_ma(raw_to_ma(g_chip, align_shunt(g_chip, reg_i)));
    g_disp.push(raw_to_mv(g_chip, g_fast_raw), g_raw_ma, systick::millis());
    g_fast_valid = true;
}

K2_HOT std::optional<Metrics> sample_now() {
    if (g_chip == Chip::None) return std::nullopt;
    const bool ina226 = g_chip == Chip::Ina226;
    std::uint32_t reg_i, reg_v;
    if (!read_reg(g_chip, ina226 ? kIna226Shunt : kVshunt, reg_i) ||
        !read_reg(g_chip, ina226 ? kIna226Bus : kVbus, reg_v))
        return std::nullopt;
    Metrics m;
    m.die_id = g_die_id;
    m.vbus_mv = raw_to_mv(g_chip, align_vbus(g_chip, reg_v));
    m.current_ma = forward_ma(raw_to_ma(g_chip, align_shunt(g_chip, reg_i)));
    m.flow = flow_of(m.current_ma);
    m.power_mw = mw(m.vbus_mv, m.current_ma);
    return m;
}

K2_HOT bool fast_begin() {
    if (g_chip == Chip::None) return false;
    if (g_chip == Chip::Ina226 && !g_fast && !write16(kConfig, kIna226Fast)) return false;
    g_fast = true;
    return true;
}

K2_HOT void fast_end() {
    if (!g_fast) return;
    g_fast = false;
    if (g_chip == Chip::Ina226) write16(kConfig, kIna226Normal);
}

bool fast_active() { return g_fast; }

K2_HOT bool fast_vbus_mv(std::int32_t& mv) {
    if (g_chip == Chip::None) return false;
    std::uint32_t reg_v;
    if (!read_reg(g_chip, g_chip == Chip::Ina226 ? kIna226Bus : kVbus, reg_v)) return false;
    mv = raw_to_mv(g_chip, align_vbus(g_chip, reg_v));
    return true;
}

K2_HOT void restart_average() {
    if (g_chip == Chip::None) return;
    const bool ina226 = g_chip == Chip::Ina226;
    std::uint32_t reg_i, reg_v;
    if (!read_reg(g_chip, ina226 ? kIna226Shunt : kVshunt, reg_i) ||
        !read_reg(g_chip, ina226 ? kIna226Bus : kVbus, reg_v))
        return;
    g_avg_i.restart(align_shunt(g_chip, reg_i));
    g_avg_v.restart(align_vbus(g_chip, reg_v));
    g_fast_raw = align_vbus(g_chip, reg_v);
    g_raw_ma = forward_ma(raw_to_ma(g_chip, align_shunt(g_chip, reg_i)));
    g_disp.reset();   // the display restarts from this direct reading too
    g_disp.push(raw_to_mv(g_chip, g_fast_raw), g_raw_ma, systick::millis());
    g_fast_valid = true;
}

K2_HOT std::optional<std::int32_t> vbus_fast_mv() {
    if (g_chip == Chip::None || !g_fast_valid) return std::nullopt;
    return raw_to_mv(g_chip, g_fast_raw);
}

K2_HOT std::optional<Metrics> read() {
    if (g_chip == Chip::None || g_avg_v.count() == 0) return std::nullopt;
    Metrics m;
    m.die_id = g_die_id;
    m.vbus_mv = raw_to_mv(g_chip, g_avg_v.mean());
    m.current_ma = apply_deadband(forward_ma(raw_to_ma(g_chip, g_avg_i.mean())));
    m.flow = flow_of(m.current_ma);
    m.power_mw = mw(m.vbus_mv, m.current_ma);
    return m;
}

namespace {
K2_HOT Metrics metrics_of(std::int32_t vbus_mv, std::int32_t forward_current_ma) {
    Metrics m;
    m.die_id = g_die_id;
    m.vbus_mv = vbus_mv;
    m.current_ma = apply_deadband(forward_current_ma);
    m.flow = flow_of(m.current_ma);
    m.power_mw = mw(m.vbus_mv, m.current_ma);
    return m;
}
}  // namespace

K2_HOT std::optional<Metrics> read_display() {
    if (g_chip == Chip::None || !g_fast_valid) return std::nullopt;
    return metrics_of(g_disp.vbus_mv(), g_disp.current_ma());
}

K2_HOT std::optional<Metrics> read_raw() {
    if (g_chip == Chip::None || !g_fast_valid) return std::nullopt;
    return metrics_of(raw_to_mv(g_chip, g_fast_raw), g_raw_ma);
}

K2_HOT void set_smoothing(smoothing::Preset preset) { g_disp.set(preset); }

smoothing::Preset smoothing_preset() { return g_disp.preset(); }

}  // namespace k2::power
