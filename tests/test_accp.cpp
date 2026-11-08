// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Huawei ACCP: CRC-8, register read/write frames and responses, FCP / SCP register maths.
#include <array>

#include "k2/fastcharge/accp.hpp"
#include "test_util.h"

using namespace k2;

#define CHECK(c) CHECK_EQ(!!(c), 1)

static void test_crc8_vectors() {
    // poly 0x39, init 0, MSB first, no final xor; vectors computed independently.
    const std::uint8_t a[] = {0x0C, 0x21};
    const std::uint8_t b[] = {0x0B, 0x2C, 0x5A};
    const std::uint8_t c[] = {0x08};
    CHECK_EQ(accp::crc8(a), 0xB0);
    CHECK_EQ(accp::crc8(b), 0x34);
    CHECK_EQ(accp::crc8(c), 0xF1);
    CHECK_EQ(accp::crc8({}), 0x00);
}

static void test_frames() {
    const auto r = accp::read_frame(0x21);
    CHECK(r[0] == 0x0C && r[1] == 0x21 && r[2] == 0xB0);
    const auto w = accp::write_frame(0x2C, 0x5A);
    CHECK(w[0] == 0x0B && w[1] == 0x2C && w[2] == 0x5A && w[3] == 0x34);
}

static void test_parse_read() {
    std::array<std::uint8_t, 3> rx = {0x08, 0x12, 0};
    rx[2] = accp::crc8({rx.data(), 2});
    CHECK_EQ(accp::parse_read(rx).value(), 0x12);
    rx[2] ^= 1;
    CHECK_EQ(accp::parse_read(rx).has_value(), false);   // bad CRC
    rx[2] ^= 1;
    rx[0] = 0x09;
    CHECK_EQ(accp::parse_read(rx).has_value(), false);   // not an ACK
}

static void test_parse_write() {
    std::array<std::uint8_t, 2> rx = {0x08, 0};
    rx[1] = accp::crc8({rx.data(), 1});
    CHECK(accp::parse_write_ack(rx));
    // The spec quotes CRC8 = 0xD2 for the write ack, which its own algorithm does not produce
    // (0xF1): both are accepted.
    rx[1] = 0xD2;
    CHECK(accp::parse_write_ack(rx));
    rx[1] = 0x00;
    CHECK(!accp::parse_write_ack(rx));
    rx = {0x07, 0xF1};
    CHECK(!accp::parse_write_ack(rx));
}

static void test_fcp_registers() {
    CHECK_EQ(accp::fcp_vout_code(5000).value(), 50);
    CHECK_EQ(accp::fcp_vout_code(9000).value(), 90);
    CHECK_EQ(accp::fcp_vout_code(12000).value(), 120);
    CHECK_EQ(accp::fcp_vout_code(10000).has_value(), false);
    CHECK_EQ(accp::kFcpIoutCode, 20);
    CHECK_EQ(accp::kRegFcpCaps, 0x21);
    CHECK_EQ(accp::kRegFcpVoutEn, 0x2B);
    CHECK_EQ(accp::kRegFcpVout, 0x2C);
    CHECK_EQ(accp::kRegFcpIout, 0x2D);
}

static void test_scp_registers() {
    CHECK(accp::scp_supported(0x10));
    CHECK(!accp::scp_supported(0xEF));
    CHECK(accp::scp_type1_valid(0x00));
    CHECK(!accp::scp_type1_valid(0x10));
    CHECK_EQ(accp::be16(0x13, 0x88), 5000);
    CHECK_EQ(accp::hi8(5000), 0x13);
    CHECK_EQ(accp::lo8(5000), 0x88);
    CHECK_EQ(accp::scp_vread8_mv(100), 4000);   // reg * 10 + 3000
    CHECK_EQ(accp::scp_iread8_ma(40), 2000);                    // reg * 50
    CHECK_EQ(accp::scp_vset8(5000), 200);                       // (mV - 3000) / 10
    CHECK_EQ(accp::scp_vset8(3000), 0);
    // step decode: exponent bits [7:6] -> 1, 10, 100, 1000; mantissa bits [5:0]
    CHECK_EQ(accp::scp_step_decode(0x05), 5);
    CHECK_EQ(accp::scp_step_decode(0x45), 50);
    CHECK_EQ(accp::scp_step_decode(0x85), 500);
    CHECK_EQ(accp::scp_step_decode(0xC5), 5000);
    CHECK_EQ(accp::kScpInitMv, 5000);
    CHECK_EQ(accp::kScpInitMa, 3500);
    CHECK_EQ(accp::clamp_scp_mv(2000, 36000), 3000);
    CHECK_EQ(accp::clamp_scp_mv(20000, 36000), 11000);
    CHECK_EQ(accp::clamp_scp_mv(9000, 8000), 8000);
    CHECK_EQ(accp::clamp_scp_mv(9000, 0), 0);
}

// The limit registers 0x90..0x97: 0x92/0x93 min/max voltage, 0x94/0x95 min/max current, step-encoded (mV / mA).
static void test_scp_caps() {
    // 3..20 V, 0.5..6 A: 0x92 = 0xC3 (3 * 1000), 0x93 = 0xD4 (20 * 1000), 0x94 = 0x85 (5 * 100), 0x95 = 0xBC (60 * 100)
    const std::array<std::uint8_t, 8> r20 = {0x00, 0x00, 0xC3, 0xD4, 0x85, 0xBC, 0x00, 0x00};
    const auto c = accp::scp_caps_decode(r20);
    CHECK(c.valid);
    CHECK_EQ(c.max_mv, 20000);
    CHECK_EQ(c.min_mv, 3000);
    CHECK_EQ(c.max_ma, 6000);
    CHECK_EQ(c.min_ma, 500);

    // Read from two adapters on hardware (2026-11): a 3.3..12 V / 2.5 A USB-A port and the Anker Prime A2343 (5.5 V / 5 A,
    // the adapter whose 8-bit set registers are pinned in test_fc_engine).
    const auto v12 = accp::scp_caps_decode({0x99, 0x99, 0xA1, 0xCC, 0x5E, 0x99, 0x14, 0x32});
    CHECK(v12.valid);
    CHECK_EQ(v12.min_mv, 3300);
    CHECK_EQ(v12.max_mv, 12000);
    CHECK_EQ(v12.max_ma, 2500);
    const auto anker = accp::scp_caps_decode({0x00, 0x00, 0xA2, 0xB7, 0x85, 0xB2, 0x00, 0x00});
    CHECK(anker.valid);
    CHECK_EQ(anker.max_mv, 5500);
    CHECK_EQ(anker.max_ma, 5000);

    // 11 V 6 A (the 66 W tier): 0x93 = 11 * 1000 = 0xCB
    const std::array<std::uint8_t, 8> r11 = {0x00, 0x00, 0xC3, 0xCB, 0x85, 0xBC, 0x00, 0x00};
    CHECK_EQ(accp::scp_caps_decode(r11).max_mv, 11000);

    // all zero, all 0xFF (a floating line), max below min, no current: not a capability list
    CHECK(!accp::scp_caps_decode(std::array<std::uint8_t, 8>{}).valid);
    CHECK(!accp::scp_caps_decode(std::array<std::uint8_t, 8>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}).valid);
    CHECK(!accp::scp_caps_decode(std::array<std::uint8_t, 8>{0, 0, 0xCB, 0xC3, 0x85, 0xBC, 0, 0}).valid);   // min 11 V > max 3 V
    CHECK(!accp::scp_caps_decode(std::array<std::uint8_t, 8>{0, 0, 0xC3, 0xCB, 0x85, 0x00, 0, 0}).valid);   // 0 mA
    CHECK(!accp::scp_caps_decode(std::array<std::uint8_t, 8>{0, 0, 0x03, 0x0B, 0x85, 0xBC, 0, 0}).valid);   // 11 mV: not volts

    // the target clamp rises to the adapter's own maximum, never past the ceiling
    CHECK_EQ(accp::clamp_scp_mv(20000, 36000, 20000), 20000);
    CHECK_EQ(accp::clamp_scp_mv(20000, 12000, 20000), 12000);
    CHECK_EQ(accp::clamp_scp_mv(20000, 36000, 5500), 5500);    // a lower adapter maximum lowers it
    CHECK_EQ(accp::clamp_scp_mv(20000, 36000), 11000);          // no capability list: the old 11 V cap
}

int main() {
    test_scp_caps();
    test_crc8_vectors();
    test_frames();
    test_parse_read();
    test_parse_write();
    test_fcp_registers();
    test_scp_registers();
    return test_report("test_accp");
}
