// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UFCS runtime tuner (k2/fastcharge/ufcs_tune.hpp): the default tuner is today's wire format byte for byte, every
// field has a range, and each frame-format field changes the frame the way its name says. CRC values
// are from an independent bitwise python reference (poly, init and bit order as parameters).
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/fastcharge/ufcs.hpp"
#include "k2/fastcharge/ufcs_tune.hpp"
#include "test_util.h"

using namespace k2::ufcs;

static std::size_t ctl(const Tuner& t, std::uint8_t cmd, std::uint8_t id, std::array<std::uint8_t, 16>& b) {
    return encode_control(b, cmd, id, t);
}

static void test_default_is_todays_wire_format() {
    const Tuner d;
    CHECK_EQ(is_default(d), true);
    std::array<std::uint8_t, 16> b{};
    CHECK_EQ(ctl(d, kCtlGetOutputCap, 0, b), 5);
    const std::array<std::uint8_t, 5> opener{0xAA, 0x20, 0x08, 0x06, 0x95};
    CHECK_EQ(std::equal(opener.begin(), opener.end(), b.begin()), true);
    // The overloads without a tuner are the default tuner.
    std::array<std::uint8_t, 16> c{};
    CHECK_EQ(encode_control(c, kCtlGetOutputCap, 0), 5);
    CHECK_EQ(std::equal(b.begin(), b.begin() + 5, c.begin()), true);
    // The request frame is unchanged too.
    std::array<std::uint8_t, 16> r0{}, r1{};
    CHECK_EQ(encode_request(r0, 3, 1, 9000, 2000), encode_request(r1, 3, 1, 9000, 2000, d));
    CHECK_EQ(std::equal(r0.begin(), r0.begin() + 14, r1.begin()), true);
    CHECK_EQ(d.addr, kAddrSource);   // the receiver address of every frame we send
    CHECK_EQ(d.version, 1);
    CHECK_EQ(d.opener, kCtlPing);    // the standard's first message (8.2.3: Ping, then Get_Output_Capabilities)
    CHECK_EQ(d.keepalive, 0);
    CHECK_EQ(d.retry_ms, 20);
    CHECK_EQ(d.listen_ms, 500);
    CHECK_EQ(d.stop_bits, 1);        // 8N1, physical layer of the standard
    CHECK_EQ(d.baud, 115200u);
    CHECK_EQ(d.delay_ms, 10);
    CHECK_EQ(d.dm, 0);
    CHECK_EQ(d.dp, 0);
}

static void test_header_fields() {
    Tuner t;
    std::array<std::uint8_t, 16> b{};
    t.addr = 2;
    t.version = 2;
    CHECK_EQ(ctl(t, kCtlGetOutputCap, 0, b), 5);
    CHECK_EQ(b[1], 0x40);   // receiver address in [15:13]
    CHECK_EQ(b[2], 0x10);   // protocol version in [8:3]
    CHECK_EQ(b[4], 0x30);
    t = Tuner{};
    t.version = 0;
    ctl(t, kCtlGetOutputCap, 5, b);
    CHECK_EQ(b[1], 0x2A);   // id 5 in [12:9], no version
    CHECK_EQ(b[2], 0x00);
    t = Tuner{};
    t.version = 3;
    ctl(t, kCtlGetOutputCap, 0, b);
    CHECK_EQ(b[2], 0x18);
    t = Tuner{};
    t.ctl_type = 3;
    ctl(t, kCtlGetOutputCap, 0, b);
    CHECK_EQ(b[2], 0x0B);   // type bits [2:0]
    t = Tuner{};
    t.data_type = 2;
    std::array<std::uint8_t, 16> p{};
    const std::array<std::uint8_t, 1> pl{0x01};
    CHECK_EQ(encode_data(p, kDataOutputCap, pl, 0, t), 7);
    CHECK_EQ(p[2], 0x0A);
}

static void test_crc_variants() {
    const std::array<std::uint8_t, 3> body{0x20, 0x08, 0x06};
    const std::array<std::uint8_t, 4> with_sync{0xAA, 0x20, 0x08, 0x06};
    Tuner t;
    CHECK_EQ(crc8(body, t), 0x95);
    t.crc_init = 0xFF;
    CHECK_EQ(crc8(body, t), 0x1D);
    t = Tuner{};
    t.crc_lsb = true;
    CHECK_EQ(crc8(body, t), 0xF9);
    t = Tuner{};
    t.crc_poly = 0x07;
    CHECK_EQ(crc8(body, t), 0xF9);
    CHECK_EQ(crc8(with_sync, Tuner{}), 0x6F);

    // CRC over the sync byte too: the frame carries the CRC of AA 20 08 06.
    t = Tuner{};
    t.crc_sync = true;
    std::array<std::uint8_t, 16> b{};
    CHECK_EQ(ctl(t, kCtlGetOutputCap, 0, b), 5);
    CHECK_EQ(b[4], 0x6F);
}

static void test_decode_follows_the_tuner() {
    Tuner t;
    t.addr = 3;
    t.version = 0;
    t.crc_sync = true;
    t.opener = 1;
    std::array<std::uint8_t, 16> b{};
    const std::size_t n = ctl(t, 0x0A, 7, b);
    Frame f;
    std::size_t used = 0;
    CHECK_EQ(decode(std::span<const std::uint8_t>(b.data(), n), f, used, nullptr, t), Parse::Ok);
    CHECK_EQ(f.cmd, 0x0A);
    CHECK_EQ(f.id, 7);
    CHECK_EQ(f.data, false);
    // The default decoder rejects a frame that carries the other CRC.
    Why why = Why::None;
    CHECK_EQ(decode(std::span<const std::uint8_t>(b.data(), n), f, used, &why), Parse::Bad);
    CHECK_EQ(why, Why::Crc);
    // Data type bits of a tuned data frame.
    Tuner d;
    d.data_type = 2;
    std::array<std::uint8_t, 16> p{};
    const std::array<std::uint8_t, 2> pl{0xDE, 0xAD};
    const std::size_t m = encode_data(p, kDataOutputCap, pl, 1, d);
    CHECK_EQ(decode(std::span<const std::uint8_t>(p.data(), m), f, used, nullptr, d), Parse::Ok);
    CHECK_EQ(f.data, true);
    CHECK_EQ(f.payload.size(), 2u);
    CHECK_EQ(decode(std::span<const std::uint8_t>(p.data(), m), f, used, &why), Parse::Bad);   // type 2 is not a type of the default
    CHECK_EQ(why, Why::Type);
}

static void test_fields_have_ranges_and_names() {
    Tuner t;
    Field f;
    CHECK_EQ(find_field("stop", f), true);
    CHECK_EQ(f, Field::StopBits);
    CHECK_EQ(find_field("nonsense", f), false);
    CHECK_EQ(set(t, Field::StopBits, 1), true);
    CHECK_EQ(t.stop_bits, 1);
    CHECK_EQ(set(t, Field::StopBits, 3), false);   // 1 or 2 only
    CHECK_EQ(t.stop_bits, 1);
    CHECK_EQ(set(t, Field::Addr, 8), false);       // 3 bits
    CHECK_EQ(set(t, Field::Addr, 7), true);
    CHECK_EQ(set(t, Field::Version, 64), false);   // 6 bits
    CHECK_EQ(set(t, Field::Version, 63), true);
    CHECK_EQ(find_field("addr", f), true);
    CHECK_EQ(f, Field::Addr);
    CHECK_EQ(find_field("version", f), true);
    CHECK_EQ(f, Field::Version);
    CHECK_EQ(find_field("ver", f), false);   // the old "ver" meant the address and "field" the version: both names are gone
    CHECK_EQ(find_field("field", f), false);
    CHECK_EQ(set(t, Field::Baud, 57600), true);
    CHECK_EQ(t.baud, 57600u);
    CHECK_EQ(set(t, Field::Baud, 100), false);
    CHECK_EQ(set(t, Field::DelayMs, 201), false);   // the delay blocks the main loop
    CHECK_EQ(set(t, Field::DelayMs, 200), true);
    CHECK_EQ(set(t, Field::Dm, 3), false);
    CHECK_EQ(set(t, Field::CrcPoly, 0), false);     // no polynomial, no CRC
    CHECK_EQ(set(t, Field::ListenMs, 49), false);
    CHECK_EQ(set(t, Field::RetryMs, 0), false);
    CHECK_EQ(is_default(t), false);
    // get() reads back what set() stored, for every field.
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        Tuner u;
        const auto fid = static_cast<Field>(i);
        CHECK_EQ(get(u, fid), kFields[i].def);
        CHECK_EQ(set(u, fid, kFields[i].hi), true);
        CHECK_EQ(get(u, fid), kFields[i].hi);
        CHECK_EQ(set(u, fid, kFields[i].hi + 1), false);
        CHECK_EQ(get(u, fid), kFields[i].hi);
        if (kFields[i].lo > 0) CHECK_EQ(set(u, fid, kFields[i].lo - 1), false);
        // every name finds its own field
        Field back;
        CHECK_EQ(find_field(kFields[i].name, back), true);
        CHECK_EQ(back, fid);
    }
}

static void test_open_interval_schedule() {
    const Tuner d;
    // The first three opener retries at the retry interval, then the keep-alive pace.
    CHECK_EQ(open_interval_ms(d, 1), 20u);
    CHECK_EQ(open_interval_ms(d, 2), 20u);
    CHECK_EQ(open_interval_ms(d, 3), 100u);
    CHECK_EQ(open_interval_ms(d, 9), 100u);
    Tuner t;
    t.retry_ms = 50;
    CHECK_EQ(open_interval_ms(t, 1), 50u);
    CHECK_EQ(open_interval_ms(t, 3), 100u);
}

int main() {
    test_default_is_todays_wire_format();
    test_header_fields();
    test_crc_variants();
    test_decode_follows_the_tuner();
    test_fields_have_ranges_and_names();
    test_open_interval_schedule();
    return test_report("test_ufcs_tune");
}
