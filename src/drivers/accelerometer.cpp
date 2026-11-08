// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/drivers/accelerometer.hpp"
#include "k2/drivers/i2c.hpp"

namespace k2::accelerometer {
namespace {
constexpr auto kBus = i2c::Bus::Ina;
constexpr uint8_t kAddress = 0x18;
constexpr uint8_t kWhoAmI = 0x0F, kLis2dh12Identity = 0x33, kSc7a20Identity = 0x11;
constexpr uint8_t kCtrl1 = 0x20, kCtrl4 = 0x23;
constexpr uint8_t kRateAxes = 0x27, kBduRange = 0x90;
constexpr uint8_t kOutputBurst = 0xA8; // OUT_X_L with I2C auto-increment bit 7

int16_t mg(uint8_t lo, uint8_t hi) {
    const uint16_t word = static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
    const int32_t signed_word = word & 0x8000 ? static_cast<int32_t>(word) - 65536 : word;
    return static_cast<int16_t>((signed_word / 64) * 8); // normal 10-bit, +/-4g: 8mg/LSB
}
}

bool Driver::init(uint32_t now_ms) {
    last_ms_ = now_ms;
    online_ = false;
    sample_.reset();
    uint8_t identity = 0, ctrl1 = 0, ctrl4 = 0;
    // Never configure a responding but unrelated part on the shared power-monitor bus.
    if (!i2c::read_reg(kBus, kAddress, kWhoAmI, identity) ||
        (identity != kLis2dh12Identity && identity != kSc7a20Identity)) return false;
    // CTRL1 0x27 (10 Hz, all axes), CTRL4 0x10 (+-4 g); BDU holds all six output bytes coherent.
    if (!i2c::write_reg(kBus, kAddress, kCtrl4, kBduRange) ||
        !i2c::write_reg(kBus, kAddress, kCtrl1, kRateAxes) ||
        !i2c::read_reg(kBus, kAddress, kCtrl4, ctrl4) || ctrl4 != kBduRange ||
        !i2c::read_reg(kBus, kAddress, kCtrl1, ctrl1) || ctrl1 != kRateAxes) return false;
    online_ = true;
    return true;
}
bool Driver::poll(uint32_t now_ms) {
    if (now_ms - last_ms_ < (online_ ? kPollMs : kRetryMs)) return false;
    if (!online_) { init(now_ms); return true; }
    last_ms_ = now_ms;
    uint8_t bytes[6]{};
    sample_.reset();
    if (!i2c::read(kBus, kAddress, kOutputBurst, bytes)) {
        online_ = false;
        return true;
    }
    // With the MADCTL setting of the LCD, the upright screen axes are -sensor X, -sensor Y.
    sample_ = orientation::Gravity{static_cast<int16_t>(-mg(bytes[0], bytes[1])),
                                  static_cast<int16_t>(-mg(bytes[2], bytes[3])),
                                  mg(bytes[4], bytes[5])};
    return true;
}

}  // namespace k2::accelerometer
