// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// VIVO FlashCharge serial frames: the frame checksum, the canned 14-frame start-up sequence and the
// voltage frame encoder.
#include <array>
#include <cstdint>

#include "k2/fastcharge/vivo.hpp"
#include "test_util.h"

using namespace k2::vivo;

static void test_checksum() {
    // sum of the bytes plus 0x56, truncated to 8 bits, over sync, length and data
    const std::array<std::uint8_t, 3> a{0xAA, 0x01, 0x04};
    CHECK_EQ(checksum(a), 0x05);
    CHECK_EQ(checksum({}), 0x56);
}

static void test_canned_sequence() {
    // 14 frames in the order the charger expects them
    CHECK_EQ(kSequence.size(), 14);
    const std::uint8_t cmds[14] = {0x04, 0x55, 0x02, 0x16, 0x01, 0x1E, 0x03, 0x19, 0x1E, 0x1F, 0x16, 0xA6, 0x20, 0x0D};
    for (std::size_t i = 0; i < kSequence.size(); i++) {
        const auto& f = kSequence[i];
        CHECK_EQ(f.bytes[0], kSync);
        CHECK_EQ(f.len, f.bytes[1] + 3);
        CHECK_EQ(f.bytes[2], cmds[i]);
        // every frame carries the frame checksum except the 0x55 one, which is sent verbatim
        if (i != 1) CHECK_EQ(checksum({f.bytes.data(), f.len - 1u}), f.bytes[f.len - 1u]);
    }
    CHECK_EQ(kSequence[1].len, 6);                 // aa 03 55 00 00 08, checksum 0x08 (algorithm gives 0x58)
    CHECK_EQ(kSequence[1].bytes[5], 0x08);
    CHECK_EQ(checksum({kSequence[1].bytes.data(), 5}), 0x58);
    CHECK_EQ(kSequence[6].len, 10);                // the canned voltage frame: 2000 mA, 5000 mV
    CHECK_EQ(kSequence[6].bytes[3] | kSequence[6].bytes[4] << 8, 2000);
    CHECK_EQ(kSequence[6].bytes[5] | kSequence[6].bytes[6] << 8, 5000);
    CHECK_EQ(kSequence[11].bytes[2], kKeepaliveCmd);
    // the keepalive and notice-ack constants
    CHECK_EQ(kKeepalive.len, 4);
    CHECK_EQ(kKeepalive.bytes[2], 0xA6);
    CHECK_EQ(kNoticeAck.len, 6);
    CHECK_EQ(kNoticeAck.bytes[2], 0x08);
    CHECK_EQ(kNoticeAck.bytes[3], 0x55);
    CHECK_EQ(kNoticeAck.bytes[5], 0x60);
}

static void test_voltage_frame() {
    std::array<std::uint8_t, 16> b{};
    const std::size_t n = encode_voltage(b, 6000, 7000);
    CHECK_EQ(n, 10);
    const std::array<std::uint8_t, 10> want{0xAA, 0x07, 0x03, 0x70, 0x17, 0x58, 0x1B, 0x00, 0x00, 0};
    for (std::size_t i = 0; i < 9; i++) CHECK_EQ(b[i], want[i]);
    CHECK_EQ(b[9], checksum({b.data(), 9}));
    // 2000 mA / 5000 mV reproduces the canned 5 V frame (sequence entry 6)
    encode_voltage(b, 2000, 5000);
    for (std::size_t i = 0; i < 10; i++) CHECK_EQ(b[i], kSequence[6].bytes[i]);
    CHECK_EQ(b[9], 0x7C);
    std::array<std::uint8_t, 8> small{};
    CHECK_EQ(encode_voltage(small, 6000, 7000), 0);
}

static void test_decode_ack() {
    std::array<std::uint8_t, 8> f{0xAA, 0x02, kReplyCmd, 0x03, 0};
    f[4] = checksum({f.data(), 4});
    Reply r;
    std::size_t used = 0;
    CHECK_EQ(decode({f.data(), 5}, r, used) == Parse::Ok, 1);
    CHECK_EQ(used, 5);
    CHECK_EQ(r.cmd, kReplyCmd);
    CHECK_EQ(r.echo, 3);
    CHECK_EQ(acks(r, 3), 1);
    CHECK_EQ(acks(r, 4), 0);
    CHECK_EQ(decode({f.data(), 4}, r, used) == Parse::Need, 1);
    f[4] ^= 1;
    CHECK_EQ(decode({f.data(), 5}, r, used) == Parse::Bad, 1);
    CHECK_EQ(used, 1);
    // leading noise is skipped
    std::array<std::uint8_t, 8> g{0x00, 0xAA, 0x01, 0x55, 0};
    g[4] = checksum({g.data() + 1, 3});
    CHECK_EQ(decode({g.data(), 5}, r, used) == Parse::Ok, 1);
    CHECK_EQ(used, 5);
    CHECK_EQ(r.cmd, 0x55);   // the sink-reset notice AA 01 55
}

int main() {
    test_checksum();
    test_canned_sequence();
    test_voltage_frame();
    test_decode_ack();
    return test_report("test_vivo_frame");
}
