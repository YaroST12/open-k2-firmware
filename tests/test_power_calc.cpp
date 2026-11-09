// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/drivers/power_calc.hpp"
#include <stdint.h>
#include "k2/ui/ui_calc.hpp"
#include "test_util.h"

using namespace k2::power;

static void test_vbus_known_values() {
    /* INA226: 1.25 mV/LSB, 10.0 V = 8000 LSB = 0x1F40 */
    CHECK_EQ(align_vbus(Chip::Ina226, 0x1F40), 1024000);
    CHECK_EQ(raw_to_mv(Chip::Ina226, align_vbus(Chip::Ina226, 0x1F40)), 10000);
    /* INA237: 3.125 mV/LSB, 10.0 V = 3200 LSB = 0x0C80 */
    CHECK_EQ(raw_to_mv(Chip::Ina237, align_vbus(Chip::Ina237, 0x0C80)), 10000);
    /* INA228: 195.3125 uV/LSB, 10.0 V = 51200 LSB in bits 23..4 of the 24-bit register */
    CHECK_EQ(align_vbus(Chip::Ina228, 51200u << 4), 204800);
    CHECK_EQ(raw_to_mv(Chip::Ina228, align_vbus(Chip::Ina228, 51200u << 4)), 10000);
    /* 5 V on the INA226 */
    CHECK_EQ(raw_to_mv(Chip::Ina226, align_vbus(Chip::Ina226, 4000)), 5000);
}

static void test_current_known_values() {
    /* INA226: 2.0 A = 2400 LSB (6 mV at 2.5 uV/LSB) */
    CHECK_EQ(raw_to_ma(Chip::Ina226, align_shunt(Chip::Ina226, 2400)), 2000);
    /* two's complement 16-bit: -2400 */
    CHECK_EQ(align_shunt(Chip::Ina226, 0xF6A0), -2400 * 128);
    CHECK_EQ(raw_to_ma(Chip::Ina226, align_shunt(Chip::Ina226, 0xF6A0)), -2000);
    /* INA237: 4800 LSB = 2.0 A */
    CHECK_EQ(raw_to_ma(Chip::Ina237, align_shunt(Chip::Ina237, 4800)), 2000);
    /* zero load */
    CHECK_EQ(raw_to_ma(Chip::Ina226, align_shunt(Chip::Ina226, 0)), 0);
}

static void test_ina228_sign_extension() {
    /* +76800 LSB of 78.125 nV = 6 mV = 2.0 A; value sits in bits 23..4 */
    int32_t pos = align_shunt(Chip::Ina228, 76800u << 4);
    CHECK_EQ(pos, 307200);
    CHECK_EQ(raw_to_ma(Chip::Ina228, pos), 2000);
    /* -76800 as 20-bit two's complement, then placed in bits 23..4 */
    uint32_t reg = (static_cast<uint32_t>(-76800) & 0xFFFFF) << 4;
    int32_t neg = align_shunt(Chip::Ina228, reg);
    CHECK_EQ(neg, -307200);
    CHECK_EQ(raw_to_ma(Chip::Ina228, neg), -2000);
    /* the reserved low nibble is ignored */
    CHECK_EQ(align_shunt(Chip::Ina228, reg | 0xF), -307200);
    /* smallest negative step */
    CHECK_EQ(align_shunt(Chip::Ina228, 0xFFFFF0), -4);
}

static void test_clamp() {
    /* INA226 clamps at +-26.66 A */
    CHECK_EQ(raw_to_ma(Chip::Ina226, 100000000), 26660);
    CHECK_EQ(raw_to_ma(Chip::Ina226, -100000000), -26660);
    /* INA228 / INA237 clamp at +-54 A */
    CHECK_EQ(raw_to_ma(Chip::Ina228, 2000000000), 54000);
    CHECK_EQ(raw_to_ma(Chip::Ina228, -2000000000), -54000);
    CHECK_EQ(raw_to_ma(Chip::Ina237, 2000000000), 54000);
    /* just inside the INA226 limit passes through: 26.0 A = 31200 LSB */
    CHECK_EQ(raw_to_ma(Chip::Ina226, align_shunt(Chip::Ina226, 31200)), 26000);
}

static void test_power() {
    CHECK_EQ(mw(5000, 2000), 10000);
    CHECK_EQ(mw(20000, -1500), -30000);
    CHECK_EQ(mw(5000, 0), 0);
}

static void test_average() {
    Average a;
    CHECK_EQ(a.mean(), 0);
    a.push(100);
    a.push(200);
    CHECK_EQ(a.mean(), 150);
    /* fill the window with 1000, then it must forget the earlier samples */
    for (std::size_t i = 0; i < kAvgLen; i++) a.push(1000);
    CHECK_EQ(a.mean(), 1000);
    /* window of 128: replace half with 0 */
    for (std::size_t i = 0; i < kAvgLen / 2; i++) a.push(0);
    CHECK_EQ(a.mean(), 500);
    /* negatives and no overflow with large raw counts */
    a.reset();
    for (std::size_t i = 0; i < kAvgLen; i++) a.push(-2000000000);
    CHECK_EQ(a.mean(), -2000000000);
    /* a step: the full window lags it by ~2.5 s (128 x 20 ms); restart() drops the old samples (START on the C135:
       VBUS stepped to 4.9 V, the average still read 0.5 V and refused the second press) */
    a.reset();
    for (std::size_t i = 0; i < kAvgLen; i++) a.push(20);
    a.restart(4900);
    CHECK_EQ(a.mean(), 4900);
    CHECK_EQ(a.count(), 1u);
    a.push(5000);
    CHECK_EQ(a.mean(), 4950);
}

static void test_names() {
    CHECK_STR(name(Chip::Ina228), "INA228");
    CHECK_STR(name(Chip::Ina226), "INA226");
    CHECK_STR(name(Chip::Ina237), "INA237");
    CHECK_STR(name(Chip::None), "NONE");
    CHECK_EQ(reg_bytes(Chip::Ina228), 3);
    CHECK_EQ(reg_bytes(Chip::Ina226), 2);
}

static void test_vbus_ceiling() {
    /* datasheet absolute ratings: INA226 36 V, INA228 / INA237 85 V */
    CHECK_EQ(max_vbus_mv(Chip::Ina226), 36000);
    CHECK_EQ(max_vbus_mv(Chip::Ina228), 85000);
    CHECK_EQ(max_vbus_mv(Chip::Ina237), 85000);
    CHECK_EQ(max_vbus_mv(Chip::None), 0);
    /* PD request gate: 36 V on the INA226, 48 V EPR on the others */
    CHECK_EQ(max_request_mv(Chip::Ina226), 36000);
    CHECK_EQ(max_request_mv(Chip::Ina228), 48000);
    CHECK_EQ(max_request_mv(Chip::Ina237), 48000);
    CHECK_EQ(max_request_mv(Chip::None), 0);
}

static void test_shunt_is_3_mohm() {
    /* Shunt resistance implied by the 2.0 A counts and the datasheet LSB: 6 mV / 2 A = 3 mOhm */
    CHECK_EQ(implied_shunt_uohm(Chip::Ina228), 3000);
    CHECK_EQ(implied_shunt_uohm(Chip::Ina237), 3000);
    CHECK_EQ(implied_shunt_uohm(Chip::Ina226), 3000);
    CHECK_EQ(implied_shunt_uohm(Chip::None), 0);
    CHECK_EQ(kShuntUohm, 3000);
}

static void test_abs_sat() {
    static_assert(abs_sat(-5) == 5 && abs_sat(7) == 7 && abs_sat(0) == 0);
    CHECK_EQ(abs_sat(INT32_MIN), INT32_MAX);
    CHECK_EQ(abs_sat(INT32_MIN + 1), INT32_MAX);
    CHECK_EQ(abs_sat(INT32_MAX), INT32_MAX);
}

static void test_model_string() {
    char buf[8];
    CHECK_EQ(model_badge(Chip::Ina228), '8');
    CHECK_EQ(model_badge(Chip::Ina237), '7');
    CHECK_EQ(model_badge(Chip::Ina226), '6');
    CHECK_EQ(model_badge(Chip::None), '?');
    CHECK_STR(format_model(buf, Chip::Ina226, 55), "N6.55");
    CHECK_STR(format_model(buf, Chip::Ina237, 5), "N7.05");
    CHECK_STR(format_model(buf, Chip::Ina228, 123), "N8.99");   /* two digits only */
    CHECK_STR(format_model(buf, Chip::None, 0), "N?.00");
    /* a short buffer is cut, never overrun */
    char small[4];
    CHECK_STR(format_model(small, Chip::Ina226, 55), "N6.");
}

static void test_deadband_edges() {
    CHECK_EQ(kDeadbandMa, 10);
    /* inside the band: zero, idle, both signs */
    for (int32_t ma : {0, 1, -1, 2, -2, 9, -9}) {
        CHECK_EQ(apply_deadband(ma), 0);
        CHECK_EQ(static_cast<int>(flow_of(ma)), static_cast<int>(Flow::Idle));
    }
    /* the edge is inclusive: 10 mA is already a real current */
    CHECK_EQ(apply_deadband(10), 10);
    CHECK_EQ(apply_deadband(-10), -10);
    CHECK_EQ(apply_deadband(11), 11);
    CHECK_EQ(apply_deadband(-11), -11);
    CHECK_EQ(apply_deadband(2000), 2000);
    CHECK_EQ(apply_deadband(-2000), -2000);
}

static void test_flow_direction() {
    CHECK_EQ(static_cast<int>(flow_of(10)), static_cast<int>(Flow::Forward));
    CHECK_EQ(static_cast<int>(flow_of(11)), static_cast<int>(Flow::Forward));
    CHECK_EQ(static_cast<int>(flow_of(3000)), static_cast<int>(Flow::Forward));
    CHECK_EQ(static_cast<int>(flow_of(-10)), static_cast<int>(Flow::Reverse));
    CHECK_EQ(static_cast<int>(flow_of(-11)), static_cast<int>(Flow::Reverse));
    CHECK_EQ(static_cast<int>(flow_of(-3000)), static_cast<int>(Flow::Reverse));
    /* the INA offset seen on the bench (-2 mA) is idle, and its power is 0 W */
    CHECK_EQ(mw(20000, apply_deadband(-2)), 0);
}

static void test_magnitude() {
    CHECK_EQ(magnitude(0), 0);
    CHECK_EQ(magnitude(1520), 1520);
    CHECK_EQ(magnitude(-1520), 1520);
    CHECK_EQ(magnitude(INT32_MIN), 2147483648u);
}

static void test_forward_sign() {
    // Measured live: 100 W PD charger into the K2 input, laptop on the output, 19.7 V ~1.9 A. The INA
    // reports -1892 mA there, and that is normal charging, so it must read as forward.
    CHECK_EQ(forward_ma(-1892), 1892);
    CHECK_EQ(flow_of(forward_ma(-1892)) == Flow::Forward, 1);
    CHECK_EQ(forward_ma(1500), -1500);                 // power fed back into the input side
    CHECK_EQ(flow_of(forward_ma(1500)) == Flow::Reverse, 1);
    CHECK_EQ(forward_ma(0), 0);
    CHECK_EQ(mw(19700, forward_ma(-1892)) > 0, 1);     // positive power, so the energy meter counts
    k2::EnergyMeter e;
    e.reset(0);
    for (uint32_t t = 0; t <= 3600000; t += 1000) e.update(t, mw(19700, forward_ma(-1892)));
    CHECK_EQ(e.mwh() > 37000 && e.mwh() < 37500, 1);   // 37.3 W for an hour = 37.3 Wh
}

// A meter missed at the first probe (the VBUS rail of a key-mode boot is still ramping) is probed again every 100 ms for the first
// six seconds, then once a second: found within a few frames of the rail coming up, quiet after.
static void test_probe_period() {
    CHECK_EQ(probe_period_ms(0), 100u);
    CHECK_EQ(probe_period_ms(kFastProbeWindowMs - 1), 100u);
    CHECK_EQ(probe_period_ms(kFastProbeWindowMs), 1000u);
    CHECK_EQ(probe_period_ms(60000), 1000u);
    CHECK_EQ(kFastProbeWindowMs >= 3000u + 1000u, true);   // covers the bring-up (3 s) and the slow init (~1 s)
}

int main() {
    test_probe_period();
    test_deadband_edges();
    test_flow_direction();
    test_magnitude();
    test_vbus_known_values();
    test_current_known_values();
    test_ina228_sign_extension();
    test_clamp();
    test_power();
    test_average();
    test_names();
    test_vbus_ceiling();
    test_shunt_is_3_mohm();
    test_model_string();
    test_abs_sat();
    test_forward_sign();
    return test_report("test_power_calc");
}
