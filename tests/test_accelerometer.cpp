// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "test_util.h"
#include "k2/drivers/accelerometer.hpp"
#include "k2/drivers/i2c.hpp"
#include <algorithm>
#include <array>

namespace {
std::array<uint8_t, 256> registers;
unsigned calls, writes, fail_at;
void reset() { registers = {}; registers[0x0F] = 0x33; calls = writes = fail_at = 0; }
void axis(unsigned reg, int16_t value) {
    registers[reg] = static_cast<uint8_t>(value);
    registers[reg + 1] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
}
}

namespace k2::i2c {
bool read(Bus bus, uint8_t dev, uint8_t reg, std::span<uint8_t> out) {
    CHECK_EQ(static_cast<int>(bus), static_cast<int>(Bus::Ina));
    CHECK_EQ(dev, 0x18);
    if (out.size() == 6) CHECK_EQ(reg, 0xA8);
    if (++calls == fail_at) { std::fill(out.begin(), out.end(), 0xFF); return false; }
    for (size_t i = 0; i < out.size(); ++i) out[i] = registers[(reg & 0x7F) + i];
    return true;
}
bool write(Bus bus, uint8_t dev, uint8_t reg, std::span<const uint8_t> data) {
    CHECK_EQ(static_cast<int>(bus), static_cast<int>(Bus::Ina));
    CHECK_EQ(dev, 0x18);
    ++writes;
    if (++calls == fail_at) return false;
    for (size_t i = 0; i < data.size(); ++i) registers[reg + i] = data[i];
    return true;
}
}

static void test_identity_configuration_and_sample() {
    reset();
    k2::accelerometer::Driver d;
    CHECK_EQ(d.init(0), true);
    CHECK_EQ(registers[0x20], 0x27); // 10Hz, normal mode, all axes
    CHECK_EQ(registers[0x23], 0x90); // +/-4g plus BDU for coherent burst reads
    axis(0x28, -8000); axis(0x2A, 0); axis(0x2C, 0);
    CHECK_EQ(d.poll(99), false);
    CHECK_EQ(d.poll(100), true);
    const auto g = d.read();
    CHECK_EQ(g.has_value(), true);
    if (g) { CHECK_EQ(g->x, 1000); CHECK_EQ(g->y, 0); CHECK_EQ(g->z, 0); }
}

static void test_sc7a20_identity_and_unknown_devices() {
    reset();
    registers[0x0F] = 0x11;
    k2::accelerometer::Driver sc7a20;
    CHECK_EQ(sc7a20.init(0), true);
    CHECK_EQ(sc7a20.online(), true);
    CHECK_EQ(registers[0x20], 0x27);
    CHECK_EQ(registers[0x23], 0x90);
    axis(0x28, -8000); axis(0x2A, 4032); axis(0x2C, -4032);
    CHECK_EQ(sc7a20.poll(100), true);
    const auto g = sc7a20.read();
    CHECK_EQ(g.has_value(), true);
    if (g) { CHECK_EQ(g->x, 1000); CHECK_EQ(g->y, -504); CHECK_EQ(g->z, -504); }

    for (uint8_t identity : {0x00, 0x10, 0x12, 0x32, 0x34, 0xFF}) {
        reset();
        registers[0x0F] = identity;
        k2::accelerometer::Driver unknown;
        CHECK_EQ(unknown.init(0), false);
        CHECK_EQ(unknown.online(), false);
        CHECK_EQ(unknown.read().has_value(), false);
        CHECK_EQ(writes, 0);
        CHECK_EQ(calls, 1);
    }
}

static void test_wrong_id_and_failures() {
    reset(); registers[0x0F] = 0;
    k2::accelerometer::Driver wrong;
    CHECK_EQ(wrong.init(0), false);
    CHECK_EQ(writes, 0);
    CHECK_EQ(wrong.poll(999), false);
    registers[0x0F] = 0x33;
    CHECK_EQ(wrong.poll(1000), true);
    CHECK_EQ(wrong.online(), true);

    for (unsigned fail = 1; fail <= 5; ++fail) {
        reset(); fail_at = fail;
        k2::accelerometer::Driver d;
        CHECK_EQ(d.init(0), false);
        CHECK_EQ(d.online(), false);
        CHECK_EQ(d.read().has_value(), false);
    }
    reset();
    k2::accelerometer::Driver d;
    CHECK_EQ(d.init(0), true);
    axis(0x2A, -8000);
    CHECK_EQ(d.poll(100), true);
    CHECK_EQ(d.read().has_value(), true);
    fail_at = calls + 1;
    CHECK_EQ(d.poll(200), true);
    CHECK_EQ(d.read().has_value(), false);
    CHECK_EQ(d.online(), false);
    const unsigned before = calls;
    CHECK_EQ(d.poll(1199), false);
    CHECK_EQ(calls, before);
    CHECK_EQ(d.poll(1200), true);
    CHECK_EQ(d.online(), true);
}

static void test_axis_orientation_and_wrap() {
    reset();
    k2::accelerometer::Driver d;
    const uint32_t start = 0xFFFFFF80u;
    CHECK_EQ(d.init(start), true);
    // Orientation to MADCTL table: -sensor Y=normal, +sensor X=quarter turn 1.
    const int16_t raw[][2] = {{0, -8000}, {8000, 0}, {0, 8000}, {-8000, 0}};
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        axis(0x28, raw[rotation][0]); axis(0x2A, raw[rotation][1]);
        CHECK_EQ(d.poll(start + (rotation + 1) * 100), true);
        const auto sample = d.read();
        CHECK_EQ(sample.has_value(), true);
        if (sample) {
            const auto result = k2::orientation::classify(*sample);
            CHECK_EQ(result.has_value(), true);
            if (result) CHECK_EQ(*result, rotation);
        }
    }
}

int main() {
    test_identity_configuration_and_sample();
    test_sc7a20_identity_and_unknown_devices();
    test_wrong_id_and_failures();
    test_axis_orientation_and_wrap();
    return test_report("test_accelerometer");
}
