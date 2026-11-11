// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UFCS wire frames against what real sources send (reverse-engineered): encode, decode, CRC-8 (poly 0x29),
// the header fields (receiver address, message number, protocol version, message type), the control and
// data command codes, Output_Capabilities decode and Request build. The byte vectors of the "FH22" tests
// are a wire capture of a real source (a Baseus FH22 charger power bank), not hand-built.
#include <array>
#include <cstdint>

#include "k2/fastcharge/ufcs.hpp"
#include "test_util.h"

using namespace k2::ufcs;

static void test_crc() {
    const std::array<std::uint8_t, 3> a{0x20, 0x08, 0x00};
    CHECK_EQ(crc8(a), 0x63);
    const std::array<std::uint8_t, 9> chk{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(crc8(chk), 0x64);
    CHECK_EQ(crc8({}), 0);
}

// 8.2.3 (tables 14 and 15): the numbers on the wire. Soft_Reset is 4 and Accept 3: sending 4 for an
// Accept is a Soft_Reset, which is what made the first FH22 session fail.
static void test_codes() {
    CHECK_EQ(kCtlPing, 0);
    CHECK_EQ(kCtlAck, 1);
    CHECK_EQ(kCtlNack, 2);
    CHECK_EQ(kCtlAccept, 3);
    CHECK_EQ(kCtlSoftReset, 4);
    CHECK_EQ(kCtlPowerReady, 5);
    CHECK_EQ(kCtlGetOutputCap, 6);
    CHECK_EQ(kCtlGetSourceInfo, 7);
    CHECK_EQ(kCtlGetSinkInfo, 8);
    CHECK_EQ(kCtlGetCableInfo, 9);
    CHECK_EQ(kCtlGetDeviceInfo, 0xA);
    CHECK_EQ(kCtlGetErrorInfo, 0xB);
    CHECK_EQ(kCtlDetectCableInfo, 0xC);
    CHECK_EQ(kCtlStartCableDetect, 0xD);
    CHECK_EQ(kCtlEndCableDetect, 0xE);
    CHECK_EQ(kCtlExit, 0xF);
    CHECK_EQ(kCtlGetSinkInfoExt, 0x10);
    CHECK_EQ(kDataOutputCap, 1);
    CHECK_EQ(kDataRequest, 2);
    CHECK_EQ(kDataRefuse, 9);
    CHECK_EQ(kAddrSource, 1);
    CHECK_EQ(kAddrSink, 2);
    CHECK_EQ(kAddrCable, 3);
}

static void test_control_frame() {
    std::array<std::uint8_t, 16> buf{};
    CHECK_EQ(encode_control(buf, kCtlPing, 0), 5);
    CHECK_EQ(buf[0], 0xAA);
    CHECK_EQ(buf[1], 0x20);   // receiver address 001 (source) in [15:13]
    CHECK_EQ(buf[2], 0x08);   // protocol version 1 in [8:3]
    CHECK_EQ(buf[3], 0x00);
    CHECK_EQ(buf[4], 0x63);
    CHECK_EQ(encode_control(buf, kCtlGetOutputCap, 0), 5);
    CHECK_EQ(buf[4], 0x95);
    // message number lands in header bits 12:9
    CHECK_EQ(encode_control(buf, kCtlAck, 5), 5);
    CHECK_EQ(buf[1], 0x2A);
    CHECK_EQ(buf[2], 0x08);
    std::array<std::uint8_t, 4> small{};
    CHECK_EQ(encode_control(small, kCtlPing, 0), 0);   // does not fit
}

// The first FH22 session, byte for byte. Sink to source frames carry the source address (1), source to
// sink frames the sink address (2); the ACK repeats the message number of what it answers.
static std::size_t frame_to(std::span<std::uint8_t> out, std::uint8_t addr, std::uint8_t cmd, std::uint8_t id) {
    Tuner t;
    t.addr = addr;
    return encode_control(out, cmd, id, t);
}

static void check_bytes(std::span<const std::uint8_t> got, std::initializer_list<std::uint8_t> want) {
    CHECK_EQ(got.size(), want.size());
    std::size_t i = 0;
    for (const std::uint8_t b : want) {
        if (i < got.size()) CHECK_EQ(got[i], b);
        i++;
    }
}

static void test_fh22_frames() {
    std::array<std::uint8_t, 8> b{};
    std::size_t n = frame_to(b, kAddrSource, kCtlPing, 0);                  // us: Ping
    check_bytes({b.data(), n}, {0xAA, 0x20, 0x08, 0x00, 0x63});
    n = frame_to(b, kAddrSink, kCtlAck, 0);                                 // FH22: ACK of message 0
    check_bytes({b.data(), n}, {0xAA, 0x40, 0x08, 0x01, 0xDF});
    n = frame_to(b, kAddrSource, kCtlGetOutputCap, 1);                      // us: Get_Output_Capabilities, message 1
    check_bytes({b.data(), n}, {0xAA, 0x22, 0x08, 0x06, 0xD6});
    n = frame_to(b, kAddrSink, kCtlAck, 1);                                 // FH22: ACK of message 1
    check_bytes({b.data(), n}, {0xAA, 0x42, 0x08, 0x01, 0x9C});
    n = frame_to(b, kAddrSource, kCtlAck, 0);                               // us: ACK of its Output_Capabilities (message 0)
    check_bytes({b.data(), n}, {0xAA, 0x20, 0x08, 0x01, 0x4A});
    n = frame_to(b, kAddrSink, kCtlAck, 2);                                 // FH22: ACK of our Request (message 2)
    check_bytes({b.data(), n}, {0xAA, 0x44, 0x08, 0x01, 0x59});
    n = frame_to(b, kAddrSink, kCtlAccept, 1);                              // FH22: Accept (its message 1)
    check_bytes({b.data(), n}, {0xAA, 0x42, 0x08, 0x03, 0xCE});

    // decode names the header fields
    Frame f;
    std::size_t used = 0;
    const std::array<std::uint8_t, 5> accept{0xAA, 0x42, 0x08, 0x03, 0xCE};
    CHECK_EQ(decode(accept, f, used) == Parse::Ok, 1);
    CHECK_EQ(f.data, 0);
    CHECK_EQ(f.cmd, kCtlAccept);
    CHECK_EQ(f.id, 1);
    CHECK_EQ(f.addr, kAddrSink);
    CHECK_EQ(f.ver, 1);
}

// The FH22's Output_Capabilities (length 16, two output modes):
//   18 02 26 01 54 01 2C 64   mode 1: current step 50 mA (100b), voltage step 10 mV, max 5.50 V (0x0226 = 550),
//                             min 3.40 V (0x0154 = 340), max 3.00 A (0x012C = 300), min 1.00 A (0x64 = 100)
//   28 04 4C 02 26 01 90 64   mode 2: same steps, max 11.00 V (0x044C = 1100), min 5.50 V, max 4.00 A, min 1.00 A
static const std::array<std::uint8_t, 22> kFh22Caps{0xAA, 0x40, 0x09, 0x01, 0x10, 0x18, 0x02, 0x26, 0x01, 0x54, 0x01,
                                                    0x2C, 0x64, 0x28, 0x04, 0x4C, 0x02, 0x26, 0x01, 0x90, 0x64, 0xDD};

static void test_fh22_output_capabilities() {
    Frame f;
    std::size_t used = 0;
    CHECK_EQ(decode(kFh22Caps, f, used) == Parse::Ok, 1);
    CHECK_EQ(used, 22);
    CHECK_EQ(f.data, 1);
    CHECK_EQ(f.cmd, kDataOutputCap);
    CHECK_EQ(f.id, 0);
    CHECK_EQ(f.addr, kAddrSink);
    CHECK_EQ(f.payload.size(), 16);
    std::array<Cap, kMaxCaps> caps{};
    CHECK_EQ(decode_caps(f.payload, caps), 2);
    CHECK_EQ(caps[0].pos, 1);
    CHECK_EQ(caps[0].min_mv, 3400);
    CHECK_EQ(caps[0].max_mv, 5500);
    CHECK_EQ(caps[0].min_ma, 1000);
    CHECK_EQ(caps[0].max_ma, 3000);
    CHECK_EQ(caps[0].step_ma, 50);
    CHECK_EQ(caps[0].step_mv, 10);
    CHECK_EQ(caps[1].pos, 2);
    CHECK_EQ(caps[1].min_mv, 5500);    // entry n starts where entry n-1 ends
    CHECK_EQ(caps[1].max_mv, 11000);
    CHECK_EQ(caps[1].min_ma, 1000);
    CHECK_EQ(caps[1].max_ma, 4000);
    CHECK_EQ(caps[1].step_ma, 50);
    CHECK_EQ(caps[1].step_mv, 10);

    // what we ask of it: 9 V lands in mode 2; current is held to 2 A (no cable info, table 36 worst case)
    Pick p;
    CHECK_EQ(pick({caps.data(), 2}, 9000, 36000, p), 1);
    CHECK_EQ(p.pos, 2);
    CHECK_EQ(p.mv, 9000);
    CHECK_EQ(p.ma, kNoCableMa);
    CHECK_EQ(pick({caps.data(), 2}, 5000, 36000, p), 1);
    CHECK_EQ(p.pos, 1);
    CHECK_EQ(p.mv, 5000);
    CHECK_EQ(p.ma, kNoCableMa);
    CHECK_EQ(pick({caps.data(), 2}, 5500, 36000, p), 1);   // a boundary belongs to the lower mode
    CHECK_EQ(p.pos, 1);
    CHECK_EQ(pick({caps.data(), 2}, 12000, 36000, p), 0);   // nothing offers 12 V
    // an explicit current ask (the 20 V 3 A charger-test row) is held to the mode's range
    CHECK_EQ(pick({caps.data(), 2}, 9000, 36000, p, 3000), 1);
    CHECK_EQ(p.ma, 3000);
    CHECK_EQ(pick({caps.data(), 2}, 9000, 36000, p, 6000), 1);
    CHECK_EQ(p.ma, 4000);   // mode 2 tops out at 4 A
    CHECK_EQ(pick({caps.data(), 2}, 5000, 36000, p, 200), 1);
    CHECK_EQ(p.ma, 1000);   // never below the mode's minimum
    // the request frame for the pick: mode 2, 9.00 V (0x0384), 2.00 A (0x00C8)
    std::array<std::uint8_t, 16> buf{};
    CHECK_EQ(pick({caps.data(), 2}, 9000, 36000, p), 1);
    const std::size_t n = encode_request(buf, 2, p.pos, p.mv, p.ma);
    CHECK_EQ(n, 14);
    check_bytes({buf.data(), 13}, {0xAA, 0x24, 0x09, 0x02, 0x08, 0x20, 0x00, 0x00, 0x00, 0x03, 0x84, 0x00, 0xC8});
    CHECK_EQ(decode({buf.data(), n}, f, used) == Parse::Ok, 1);   // the CRC is ours: it must verify
    CHECK_EQ(f.id, 2);
}

static void test_request_frame() {
    std::array<std::uint8_t, 24> buf{};
    const std::uint64_t w = request_word(1, 5000, 1000);
    CHECK_EQ(w == 0x10000000'01F40064ULL, 1);
    const std::size_t n = encode_request(buf, 5, 1, 5000, 1000);
    CHECK_EQ(n, 14);
    const std::array<std::uint8_t, 14> want{0xAA, 0x2A, 0x09, 0x02, 8, 0x10, 0, 0, 0, 0x01, 0xF4, 0x00, 0x64, 0xB6};
    for (std::size_t i = 0; i < want.size(); i++) CHECK_EQ(buf[i], want[i]);
}

static void test_refuse_frame() {
    // 8.2.4.9: Refuse names the refused message number, type and command, and a reason; 4 bytes.
    std::array<std::uint8_t, 16> buf{};
    const std::size_t n = encode_refuse(buf, 3, 2, 0, kCtlGetSinkInfo, kRefuseUnsupported);
    CHECK_EQ(n, 10);
    CHECK_EQ(buf[3], kDataRefuse);
    CHECK_EQ(buf[4], 4);
    CHECK_EQ(buf[5], 0x02);   // [31:28] reserved, [27:24] refused message number
    CHECK_EQ(buf[6], 0x00);   // [23:19] reserved, [18:16] refused message type (control)
    CHECK_EQ(buf[7], kCtlGetSinkInfo);
    CHECK_EQ(buf[8], kRefuseUnsupported);
    Frame f;
    std::size_t used = 0;
    CHECK_EQ(decode({buf.data(), n}, f, used) == Parse::Ok, 1);
    CHECK_EQ(f.cmd, kDataRefuse);
    CHECK_EQ(f.id, 3);
}

static void test_decode_roundtrip_and_errors() {
    std::array<std::uint8_t, 24> buf{};
    const std::size_t n = encode_request(buf, 3, 2, 9000, 2000);
    Frame f;
    std::size_t used = 0;
    CHECK_EQ(decode({buf.data(), n}, f, used) == Parse::Ok, 1);
    CHECK_EQ(used, n);
    CHECK_EQ(f.data, 1);
    CHECK_EQ(f.cmd, kDataRequest);
    CHECK_EQ(f.id, 3);
    CHECK_EQ(f.addr, kAddrSource);
    CHECK_EQ(f.ver, 1);
    CHECK_EQ(f.payload.size(), 8);
    // truncated: needs more
    CHECK_EQ(decode({buf.data(), n - 1}, f, used) == Parse::Need, 1);
    CHECK_EQ(decode({buf.data(), 3}, f, used) == Parse::Need, 1);
    // corrupted CRC
    buf[n - 1] ^= 1;
    CHECK_EQ(decode({buf.data(), n}, f, used) == Parse::Bad, 1);
    CHECK_EQ(used, 1);   // skip the sync byte and resync
    // leading garbage before the sync is skipped
    std::array<std::uint8_t, 8> g{0x00, 0x13};
    const std::size_t m = encode_control(std::span<std::uint8_t>(g).subspan(2), kCtlAccept, 1);
    CHECK_EQ(m, 5);
    CHECK_EQ(decode({g.data(), 7}, f, used) == Parse::Ok, 1);
    CHECK_EQ(used, 7);
    CHECK_EQ(f.data, 0);
    CHECK_EQ(f.cmd, kCtlAccept);
    // a data header whose length byte cannot fit the receive buffer is rejected at once (resync),
    // not left waiting for bytes the accumulator can never hold
    std::array<std::uint8_t, 24> big{};
    encode_request(big, 3, 2, 9000, 2000);
    big[4] = std::uint8_t(kFrameMax - 6 + 1);
    CHECK_EQ(decode({big.data(), 6}, f, used) == Parse::Bad, 1);
    CHECK_EQ(used, 1);
    big[4] = std::uint8_t(kFrameMax - 6);   // the largest frame that fits still waits for its bytes
    CHECK_EQ(decode({big.data(), 6}, f, used) == Parse::Need, 1);
}

// One Output_Capabilities entry as the standard lays it out (table 16): mode [63:60], current step
// [59:57], voltage step [56], max V [55:40], min V [39:24], max I [23:8], min I [7:0].
static std::array<std::uint8_t, 8> cap_entry(int pos, int max_mv, int min_mv, int max_ma, int min_ma, int ma_step_code = 4,
                                             int mv_step_bit = 0) {
    const std::uint64_t w = (std::uint64_t(pos) << 60) | (std::uint64_t(ma_step_code) << 57) | (std::uint64_t(mv_step_bit) << 56) |
                            (std::uint64_t(max_mv / 10) << 40) | (std::uint64_t(min_mv / 10) << 24) |
                            (std::uint64_t(max_ma / 10) << 8) | std::uint64_t(min_ma / 10);
    std::array<std::uint8_t, 8> b{};
    for (int i = 0; i < 8; i++) b[7 - i] = std::uint8_t(w >> (8 * i));
    return b;
}

static void test_caps_and_pick() {
    std::array<std::uint8_t, 16> pl{};
    const auto a = cap_entry(1, 11000, 5000, 3000, 500);
    const auto b = cap_entry(2, 21000, 11000, 5000, 500);
    for (int i = 0; i < 8; i++) { pl[i] = a[i]; pl[8 + i] = b[i]; }
    std::array<Cap, kMaxCaps> caps{};
    CHECK_EQ(decode_caps(pl, caps), 2);
    CHECK_EQ(caps[0].pos, 1);
    CHECK_EQ(caps[0].max_mv, 11000);
    CHECK_EQ(caps[0].min_mv, 5000);
    CHECK_EQ(caps[0].max_ma, 3000);
    CHECK_EQ(caps[0].min_ma, 500);
    CHECK_EQ(caps[1].pos, 2);
    CHECK_EQ(caps[1].max_mv, 21000);
    CHECK_EQ(caps[1].min_mv, 11000);
    // payload length not a multiple of 8, or empty: refused
    CHECK_EQ(decode_caps({pl.data(), 7}, caps), 0);
    CHECK_EQ(decode_caps({}, caps), 0);
    // reserved current-step codes (above 100b) make the entry unusable: it decodes with step 0 and is never picked
    const auto bad = cap_entry(1, 11000, 5000, 3000, 500, 5);
    std::array<Cap, kMaxCaps> bc{};
    CHECK_EQ(decode_caps(bad, bc), 1);
    CHECK_EQ(bc[0].step_ma, 0);
    Pick pp;
    CHECK_EQ(pick({bc.data(), 1}, 9000, 20000, pp), 0);

    Pick p;
    CHECK_EQ(pick({caps.data(), 2}, 9000, 20000, p), 1);
    CHECK_EQ(p.pos, 1);
    CHECK_EQ(p.mv, 9000);
    CHECK_EQ(p.ma, 2000);   // no cable info: 2 A
    CHECK_EQ(pick({caps.data(), 2}, 15000, 20000, p), 1);   // only entry 2 spans 15 V
    CHECK_EQ(p.pos, 2);
    CHECK_EQ(p.mv, 15000);
    CHECK_EQ(p.ma, 2000);
    CHECK_EQ(pick({caps.data(), 2}, 20000, 12000, p), 1);   // clamped to the ceiling, then to entry 2
    CHECK_EQ(p.pos, 2);
    CHECK_EQ(p.mv, 12000);
    CHECK_EQ(pick({caps.data(), 2}, 25000, 36000, p), 0);   // nothing spans 25 V
}

static void test_steps() {
    // 20 mV voltage step, 30 mA current step, min current 0.5 A (the standard's own example, 8.2.4.2)
    const auto e = cap_entry(1, 5500, 3400, 6000, 500, 2, 1);
    std::array<Cap, kMaxCaps> caps{};
    CHECK_EQ(decode_caps(e, caps), 1);
    CHECK_EQ(caps[0].step_mv, 20);
    CHECK_EQ(caps[0].step_ma, 30);
    Pick p;
    CHECK_EQ(pick({caps.data(), 1}, 5110, 36000, p), 1);
    CHECK_EQ(p.mv, 5100);   // rounded down to the 20 mV grid (5.1 V is 255 steps)
    CHECK_EQ(p.ma, 1980);   // 2 A is not on the 30 mA grid: 66 steps
    // an entry whose minimum current is above 2 A gets its minimum (no load on the K2)
    const auto hi = cap_entry(1, 5500, 3400, 6000, 2500, 4, 0);
    CHECK_EQ(decode_caps(hi, caps), 1);
    CHECK_EQ(pick({caps.data(), 1}, 5000, 36000, p), 1);
    CHECK_EQ(p.ma, 2500);
    // a step that rounds below the entry minimum is lifted back to it
    const auto lo = cap_entry(1, 5500, 3410, 3000, 500, 4, 1);
    CHECK_EQ(decode_caps(lo, caps), 1);
    CHECK_EQ(pick({caps.data(), 1}, 3410, 36000, p), 1);
    CHECK_EQ(p.mv, 3420);   // 3.41 V rounds down to 3.40 V on the 20 mV grid, below the 3.41 V minimum: next grid point
}

static void test_session_watchdog_period() {
    CHECK_EQ(kKeepaliveMs < kWatchdogMs, 1);
    CHECK_EQ(kWatchdogMs, 1000);   // the source's default watchdog (7.x): any message from the sink clears it
}

int main() {
    test_crc();
    test_codes();
    test_control_frame();
    test_fh22_frames();
    test_fh22_output_capabilities();
    test_request_frame();
    test_refuse_frame();
    test_decode_roundtrip_and_errors();
    test_caps_and_pick();
    test_steps();
    test_session_watchdog_period();
    return test_report("test_ufcs_frame");
}
