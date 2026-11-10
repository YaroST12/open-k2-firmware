// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the remote-control framing and dispatcher (bytes in, bytes out, fake backend).
// Expected bytes are written out literally from the layout in remote_proto.hpp.
#include "test_util.h"
#include "k2/core/remote_proto.hpp"
#include "k2/chargertest/report_card_remote.hpp"
#include <initializer_list>
#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

using namespace k2::remote;

// The dispatcher refuses a Cards GET past kCardsChunks: it must be exactly the chunks of one card payload (a card that grows
// needs this bumped, or its last chunk is refused on the wire and `k2_remote cards show` reads a short card).
static_assert(kCardsChunks == k2::report_card::remote::kChunks);

#include "fake_remote_backend.hpp"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using Frame = std::array<uint8_t, kFrameSize>;

static std::vector<uint8_t> request(uint8_t cmd, uint8_t seq, std::initializer_list<uint8_t> payload) {
    std::vector<uint8_t> v = {kReqMagic0, kReqMagic1, cmd, seq, static_cast<uint8_t>(payload.size())};
    v.insert(v.end(), payload);
    v.resize(kFrameSize, 0);
    return v;
}

// Runs one request; returns whether a response was produced.
template <class B>
static bool run(B &b, const std::vector<uint8_t> &req, Frame &rsp) {
    rsp.fill(0xEE);
    return handle_request(req, b, rsp);
}

static void check_header(const Frame &r, uint8_t cmd, uint8_t seq, Status st, uint8_t len) {
    CHECK_EQ(r[0], 0xA6);
    CHECK_EQ(r[1], 0x4B);
    CHECK_EQ(r[2], cmd);
    CHECK_EQ(r[3], seq);
    CHECK_EQ(r[4], static_cast<uint8_t>(st));
    CHECK_EQ(r[5], len);
    for (size_t i = kRspHeader + len; i < kFrameSize; i++) CHECK_EQ(r[i], 0);   // zero padded
}

static void check_bytes(const Frame &r, size_t offset, std::initializer_list<uint8_t> want) {
    size_t i = offset;
    for (uint8_t w : want) CHECK_EQ(r[i++], w);
}

static void test_parse_valid() {
    auto req = request(0x02, 7, {2, 100, 0});
    ParseResult p = parse_request(req);
    CHECK_EQ(static_cast<int>(p.error), static_cast<int>(ParseError::None));
    CHECK_EQ(p.header_valid, 1);
    CHECK_EQ(p.cmd, 2);
    CHECK_EQ(p.seq, 7);
    CHECK_EQ(p.payload.size(), 3);
    CHECK_EQ(p.payload[0], 2);
    CHECK_EQ(p.payload[1], 100);
}

static void test_parse_bad_magic_and_short() {
    auto bad = request(0x01, 1, {});
    bad[0] = 0xFF; bad[1] = 0x55;                       // a bootloader frame
    CHECK_EQ(static_cast<int>(parse_request(bad).error), static_cast<int>(ParseError::BadMagic));
    CHECK_EQ(parse_request(bad).header_valid, 0);

    auto text = std::vector<uint8_t>{'[', 'S', 'Y', 'S', ']', ' ', 'h', 'i'};   // a log line echoed back
    CHECK_EQ(static_cast<int>(parse_request(text).error), static_cast<int>(ParseError::BadMagic));

    std::vector<uint8_t> tiny = {kReqMagic0, kReqMagic1, 0x01};
    CHECK_EQ(static_cast<int>(parse_request(tiny).error), static_cast<int>(ParseError::TooShort));
    CHECK_EQ(static_cast<int>(parse_request({}).error), static_cast<int>(ParseError::TooShort));

    Fake f; Frame r;
    CHECK_EQ(run(f, bad, r), 0);                         // no answer to something that is not ours
    CHECK_EQ(run(f, tiny, r), 0);
    CHECK_EQ(f.calls, 0);
}

static void test_truncated() {
    auto req = request(0x02, 9, {2, 100, 0});
    req.resize(kReqHeader + 2);                          // header says 3 payload bytes, only 2 arrived
    ParseResult p = parse_request(req);
    CHECK_EQ(static_cast<int>(p.error), static_cast<int>(ParseError::Truncated));
    CHECK_EQ(p.header_valid, 1);
    Fake f; Frame r;
    CHECK_EQ(run(f, req, r), 1);
    check_header(r, 0x02, 9, Status::BadLength, 0);
    CHECK_EQ(f.calls, 0);
}

static void test_oversize() {
    auto req = request(0x01, 3, {});
    req[4] = 60;                                         // 59 is the most a 64-byte report can carry
    CHECK_EQ(static_cast<int>(parse_request(req).error), static_cast<int>(ParseError::Oversize));
    Fake f; Frame r;
    CHECK_EQ(run(f, req, r), 1);
    check_header(r, 0x01, 3, Status::BadLength, 0);
    CHECK_EQ(f.calls, 0);

    req[4] = 59;                                         // exactly full is fine at parser level
    CHECK_EQ(static_cast<int>(parse_request(req).error), static_cast<int>(ParseError::None));
    CHECK_EQ(parse_request(req).payload.size(), 59);
}

static void test_unknown_command() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x7F, 42, {1, 2}), r), 1);
    check_header(r, 0x7F, 42, Status::UnknownCmd, 0);
    CHECK_EQ(run(f, request(0x00, 1, {}), r), 1);
    check_header(r, 0x00, 1, Status::UnknownCmd, 0);
    CHECK_EQ(f.calls, 0);
}

static void test_seq_echo() {
    Fake f; Frame r;
    for (int seq : {0, 1, 127, 128, 254, 255}) {
        CHECK_EQ(run(f, request(0x01, static_cast<uint8_t>(seq), {}), r), 1);
        CHECK_EQ(r[3], seq);
    }
}

static void test_ping() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x01, 5, {}), r), 1);
    check_header(r, 0x01, 5, Status::Ok, 32);
    check_bytes(r, kRspHeader, {1,  0x04, 0x03, 0x02, 0x01,
                                'a', 'b', 'c', '1', '2', '3', '4', 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                '1', '.', '0'});
    CHECK_EQ(run(f, request(0x01, 5, {1}), r), 1);      // PING takes no payload
    check_header(r, 0x01, 5, Status::BadLength, 0);
}

static void test_ping_long_git_string() {
    // "git describe --always --dirty" with a tag-less tree: 7-char hash + "-dirty" = 13, must not be cut.
    std::array<uint8_t, 64> buf{};
    PingInfo p{1, "cef22c4-dirty", "1.0"};
    size_t n = encode_ping(buf, p);
    CHECK_EQ(n, size_t(1 + 4 + kPingGitLen + 3));
    CHECK_EQ(std::string_view(reinterpret_cast<const char *>(&buf[5]), 13) == "cef22c4-dirty", true);
}

static void test_button() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x02, 1, {2, 100, 0}), r), 1);
    check_header(r, 0x02, 1, Status::Ok, 3);
    check_bytes(r, kRspHeader, {2, 100, 0});
    CHECK_EQ(f.last_btn, 2);
    CHECK_EQ(f.last_ms, 100);

    f = Fake{};
    CHECK_EQ(run(f, request(0x02, 2, {3, 0xB8, 0x0B}), r), 1);   // 3000 ms, little endian
    CHECK_EQ(f.last_ms, 3000);

    CHECK_EQ(run(f, request(0x02, 3, {4, 100, 0}), r), 1);       // no such button
    check_header(r, 0x02, 3, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x02, 4, {0, 29, 0}), r), 1);        // below the debounce margin
    check_header(r, 0x02, 4, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x02, 5, {0, 0x11, 0x27}), r), 1);   // 10001 ms
    check_header(r, 0x02, 5, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x02, 6, {0, 100}), r), 1);          // payload too short
    check_header(r, 0x02, 6, Status::BadLength, 0);
    CHECK_EQ(f.last_ms, 3000);                                   // none of the rejects reached the backend

    f.press_status = Status::Busy;
    CHECK_EQ(run(f, request(0x02, 7, {1, 100, 0}), r), 1);
    check_header(r, 0x02, 7, Status::Busy, 0);
}

static void test_state() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x03, 8, {}), r), 1);
    check_header(r, 0x03, 8, Status::Ok, 53);
    check_bytes(r, kRspHeader, {1, 2, 2, 4, 1, 3, 1, 1,
                                0x20, 0x4E, 0x00, 0x00,          // 20000 mV
                                0x24, 0xFA, 0xFF, 0xFF,          // -1500 mA
                                0x45, 0x23, 0x01, 0x00,          // 74565 mW
                                0x07, 0x00, 0x00, 0x00,          // 7 mWh
                                0x23, 0x01, 0x56, 0x04,          // D+ 0x0123, D- 0x0456
                                0x89, 0x07, 0xBC, 0x0A,          // CC1 0x0789, CC2 0x0ABC
                                0x45, 0x23,                      // aux 0x2345
                                0xE7, 0xFF,                      // -25 deci-degrees
                                6, 11,                           // verdict 6, flags (reading, NTC, MCU valid)
                                0x73, 0x01,                      // MCU 371 deci-degrees
                                0x05,                            // link: stale | inline
                                83, 0, 0, 0,                     // unplugged 83 s ago
                                0x02, 0x01, 0, 0,                // cable scanned 258 s ago
                                0x00, 0x00, 0x00, 0x0C});        // boot reset flags: POR | PIN (CTRLSTS bits 27, 26)
}

static void test_adc_raw() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x21, 4, {}), r), 1);
    check_header(r, 0x21, 4, Status::Ok, 35);
    CHECK_EQ(r[6], 1);
    CHECK_EQ(r[7], 0x00); CHECK_EQ(r[8], 0x01);                   // all[0] = 0x0100
    CHECK_EQ(r[7 + 18], 0x09); CHECK_EQ(r[8 + 18], 0x01);         // all[9] = 0x0109
    CHECK_EQ(r[27], 0x00); CHECK_EQ(r[28], 0x02);                 // scan[0] = 0x0200
    CHECK_EQ(r[39], 0x06); CHECK_EQ(r[40], 0x02);                 // scan[6] = 0x0206
    CHECK_EQ(run(f, request(0x21, 5, {1}), r), 1);
    check_header(r, 0x21, 5, Status::BadLength, 0);
}

static void test_lab() {
    Fake f;
    Frame r;
    { const auto q = request(0x51, 1, {0, 30, 100, 0, 1}); CHECK_EQ(run(f, q, r), 1); }        // duty 30, 100 ms, confirmed
    check_header(r, 0x51, 1, Status::Ok, 17);
    CHECK_EQ(f.lab_duty, 30);
    CHECK_EQ(f.lab_ms, 100);
    f.lab_duty = 0;
    { const auto q = request(0x51, 2, {0, 30, 100, 0, 0}); CHECK_EQ(run(f, q, r), 1); }        // not confirmed
    check_header(r, 0x51, 2, Status::NeedConfirm, 0);
    CHECK_EQ(f.lab_duty, 0);
    { const auto q = request(0x51, 3, {0, 30, 100}); CHECK_EQ(run(f, q, r), 1); }
    check_header(r, 0x51, 3, Status::BadLength, 0);
    { const auto q = request(0x51, 4, {9, 30, 100, 0, 1}); CHECK_EQ(run(f, q, r), 1); }
    check_header(r, 0x51, 4, Status::BadArg, 0);
    // sniff: arm answers state, reason, flags, edges (LE)
    { const auto q = request(0x51, 5, {1, 1}); CHECK_EQ(run(f, q, r), 1); }
    check_header(r, 0x51, 5, Status::Ok, 5);
    check_bytes(r, kRspHeader, {2, 3, 2, 0x23, 0x01});
    CHECK_EQ(f.sniff_sub, 1);
    { const auto q = request(0x51, 6, {1, 3}); CHECK_EQ(run(f, q, r), 1); }                  // selftest needs confirm
    check_header(r, 0x51, 6, Status::NeedConfirm, 0);
    { const auto q = request(0x51, 7, {1, 3, 1}); CHECK_EQ(run(f, q, r), 1); }
    check_header(r, 0x51, 7, Status::Ok, 5);
    CHECK_EQ(f.sniff_sub, 3);
    { const auto q = request(0x51, 8, {1, 4}); CHECK_EQ(run(f, q, r), 1); }
    check_header(r, 0x51, 8, Status::BadArg, 0);
    { const auto q = request(0x51, 9, {1}); CHECK_EQ(run(f, q, r), 1); }
    check_header(r, 0x51, 9, Status::BadLength, 0);
}

static void test_cable_r() {
    Fake f; Frame r;
    // Status with no payload and with op 0: read-only, no reset.
    CHECK_EQ(run(f, request(0x50, 1, {}), r), 1);
    check_header(r, 0x50, 1, Status::Ok, 16);
    check_bytes(r, kRspHeader, {3, 1, 0x64, 0x00, 0xEC, 0x13, 0x28, 0x00, 0x24, 0x13, 0x02, 0x08, 0x32, 0x00, 0x46, 0x00});
    CHECK_EQ(f.cable_kelvin, -1);
    CHECK_EQ(f.cable_resets, 0);
    CHECK_EQ(run(f, request(0x50, 2, {0}), r), 1);
    check_header(r, 0x50, 2, Status::Ok, 16);
    CHECK_EQ(f.cable_resets, 0);
    // Reset clears the reference, then reports the status.
    CHECK_EQ(run(f, request(0x50, 3, {1}), r), 1);
    check_header(r, 0x50, 3, Status::Ok, 16);
    CHECK_EQ(f.cable_resets, 1);
    // Mode ops: 2 selects the passive meter, 3 the Kelvin 8-wire mode; neither resets by itself here.
    CHECK_EQ(run(f, request(0x50, 6, {3}), r), 1);
    check_header(r, 0x50, 6, Status::Ok, 16);
    CHECK_EQ(f.cable_kelvin, 1);
    CHECK_EQ(run(f, request(0x50, 7, {2}), r), 1);
    check_header(r, 0x50, 7, Status::Ok, 16);
    CHECK_EQ(f.cable_kelvin, 0);
    CHECK_EQ(f.cable_resets, 1);
    // Unknown op and a long payload are refused without touching the backend.
    CHECK_EQ(run(f, request(0x50, 4, {4}), r), 1);
    check_header(r, 0x50, 4, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x50, 5, {1, 0}), r), 1);
    check_header(r, 0x50, 5, Status::BadLength, 0);
    CHECK_EQ(f.cable_resets, 1);
}

static void test_cards() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x62, 1, {0}), r), 1);                      // list
    check_header(r, 0x62, 1, Status::Ok, 2);
    check_bytes(r, kRspHeader, {0xC4, 0x33});
    CHECK_EQ(run(f, request(0x62, 2, {0, 0}), r), 1);                   // list from offset 0
    check_header(r, 0x62, 2, Status::Ok, 2);
    CHECK_EQ(run(f, request(0x62, 2, {0, 0, 0}), r), 1);
    check_header(r, 0x62, 2, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x62, 3, {1, 3, 0, 2}), r), 1);             // get card 3, chunk 2
    check_header(r, 0x62, 3, Status::Ok, 2);
    CHECK_EQ(int(f.cards_req.size()), 4); CHECK_EQ(int(f.cards_req[3]), 2);
    f.calls = 0;
    CHECK_EQ(run(f, request(0x62, 4, {1, 3, 0, 3}), r), 1);             // get card 3, chunk 3 (v4 has 4 chunks)
    check_header(r, 0x62, 4, Status::Ok, 2);
    CHECK_EQ(int(f.cards_req.size()), 4); CHECK_EQ(int(f.cards_req[3]), 3);
    f.calls = 0;
    CHECK_EQ(run(f, request(0x62, 4, {1, 3, 0, 4}), r), 1);             // format 7 payload: 196 bytes = chunks 0..4
    check_header(r, 0x62, 4, Status::Ok, 2);
    f.calls = 0;
    CHECK_EQ(run(f, request(0x62, 5, {1, 3, 0, 5}), r), 1);             // chunk 5 does not exist
    check_header(r, 0x62, 5, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x62, 5, {1, 3, 0}), r), 1);
    check_header(r, 0x62, 5, Status::BadLength, 0);
    // Delete needs the confirm byte.
    CHECK_EQ(run(f, request(0x62, 6, {3, 1, 0}), r), 1);
    check_header(r, 0x62, 6, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x62, 7, {3, 1, 0, 0}), r), 1);
    check_header(r, 0x62, 7, Status::BadArg, 0);
    CHECK_EQ(f.calls, 0);
    CHECK_EQ(run(f, request(0x62, 8, {3, 1, 0, 1}), r), 1);
    check_header(r, 0x62, 8, Status::Ok, 2);
    // Rename: 1..20 name bytes.
    CHECK_EQ(run(f, request(0x62, 9, {2, 1, 0}), r), 1);
    check_header(r, 0x62, 9, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x62, 10, {2, 1, 0, 'A', 'b'}), r), 1);
    check_header(r, 0x62, 10, Status::Ok, 2);
    {
        std::vector<uint8_t> v = {kReqMagic0, kReqMagic1, 0x62, 11, 23, 2, 1, 0};
        v.insert(v.end(), 20, 'x');
        v.resize(kFrameSize, 0);
        CHECK_EQ(run(f, v, r), 1);
        check_header(r, 0x62, 11, Status::Ok, 2);
        v[4] = 24; v.insert(v.begin() + 8, 'x');
        v.resize(kFrameSize, 0);
        CHECK_EQ(run(f, v, r), 1);
        check_header(r, 0x62, 11, Status::BadLength, 0);
    }
    CHECK_EQ(run(f, request(0x62, 12, {9}), r), 1);                     // unknown op
    check_header(r, 0x62, 12, Status::BadArg, 0);
}

static void test_emlib() {
    Fake f; Frame r;
    // Each op reaches the backend with its whole payload; the response is the backend's bytes.
    CHECK_EQ(run(f, request(0x61, 1, {0}), r), 1);
    check_header(r, 0x61, 1, Status::Ok, 3);
    check_bytes(r, kRspHeader, {0xE0, 0x11, 0x22});
    CHECK_EQ(int(f.em_req.size()), 1);
    CHECK_EQ(run(f, request(0x61, 2, {0, 30}), r), 1);                 // list from index 30
    CHECK_EQ(int(f.em_req.size()), 2); CHECK_EQ(int(f.em_req[1]), 30);
    CHECK_EQ(run(f, request(0x61, 3, {1, 0x34, 0x12}), r), 1);         // get 0x1234
    CHECK_EQ(int(f.em_req.size()), 3); CHECK_EQ(int(f.em_req[2]), 0x12);
    CHECK_EQ(run(f, request(0x61, 4, {2}), r), 1);                     // save the current cable
    // Delete needs the confirm byte.
    f.calls = 0;
    CHECK_EQ(run(f, request(0x61, 5, {3, 1, 0}), r), 1);
    check_header(r, 0x61, 5, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x61, 6, {3, 1, 0, 0}), r), 1);
    check_header(r, 0x61, 6, Status::BadArg, 0);
    CHECK_EQ(f.calls, 0);
    CHECK_EQ(run(f, request(0x61, 7, {3, 1, 0, 1}), r), 1);
    check_header(r, 0x61, 7, Status::Ok, 3);
    // Rename: 1..24 name bytes.
    CHECK_EQ(run(f, request(0x61, 8, {4, 1, 0}), r), 1);
    check_header(r, 0x61, 8, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x61, 9, {4, 1, 0, 'A', 'b'}), r), 1);
    check_header(r, 0x61, 9, Status::Ok, 3);
    CHECK_EQ(int(f.em_req.size()), 5);
    {
        std::vector<uint8_t> v = {kReqMagic0, kReqMagic1, 0x61, 10, 27, 4, 1, 0};
        v.insert(v.end(), 24, 'x');                                   // 24 chars fit, 25 do not
        v.resize(kFrameSize, 0);
        CHECK_EQ(run(f, v, r), 1);
        check_header(r, 0x61, 10, Status::Ok, 3);
        v[4] = 28; v[8 + 24] = 'y';
        CHECK_EQ(run(f, v, r), 1);
        check_header(r, 0x61, 10, Status::BadLength, 0);
    }
    // Spoof: status / disarm are two bytes, arm carries the number; unknown sub-op refused.
    CHECK_EQ(run(f, request(0x61, 11, {5, 0}), r), 1);
    check_header(r, 0x61, 11, Status::Ok, 3);
    CHECK_EQ(run(f, request(0x61, 12, {5, 1, 7, 0}), r), 1);
    check_header(r, 0x61, 12, Status::Ok, 3);
    CHECK_EQ(run(f, request(0x61, 13, {5, 1}), r), 1);
    check_header(r, 0x61, 13, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x61, 14, {5, 2, 0}), r), 1);
    check_header(r, 0x61, 14, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x61, 15, {5, 4}), r), 1);
    check_header(r, 0x61, 15, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x61, 16, {9}), r), 1);
    check_header(r, 0x61, 16, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x61, 17, {}), r), 1);
    check_header(r, 0x61, 17, Status::BadLength, 0);
    // Built-in spoof [5, 3, id]: ids 0..4 arm, the first id past the table is refused, a missing id is a length error.
    for (uint8_t id = 0; id < k2::emrec::kBuiltinCount; ++id) {
        CHECK_EQ(run(f, request(0x61, 30, {5, 3, id}), r), 1);
        check_header(r, 0x61, 30, Status::Ok, 3);
    }
    CHECK_EQ(run(f, request(0x61, 31, {5, 3, uint8_t(k2::emrec::kBuiltinCount)}), r), 1);
    check_header(r, 0x61, 31, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x61, 32, {5, 3}), r), 1);
    check_header(r, 0x61, 32, Status::BadLength, 0);
    // Learn [6, op, ...]: status/save are two bytes, entry carries an index, run carries the options.
    CHECK_EQ(run(f, request(0x61, 40, {6, 0}), r), 1);
    check_header(r, 0x61, 40, Status::Ok, 3);
    CHECK_EQ(run(f, request(0x61, 41, {6, 3}), r), 1);
    check_header(r, 0x61, 41, Status::Ok, 3);
    CHECK_EQ(run(f, request(0x61, 42, {6, 2, 5}), r), 1);
    check_header(r, 0x61, 42, Status::Ok, 3);
    CHECK_EQ(int(f.em_req.size()), 3); CHECK_EQ(int(f.em_req[2]), 5);
    CHECK_EQ(run(f, request(0x61, 43, {6, 2}), r), 1);                  // entry needs its index
    check_header(r, 0x61, 43, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x61, 44, {6, 0, 0}), r), 1);               // status takes nothing more
    check_header(r, 0x61, 44, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x61, 45, {6}), r), 1);
    check_header(r, 0x61, 45, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x61, 46, {6, 4}), r), 1);                  // no such learn op
    check_header(r, 0x61, 46, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x61, 47, {6, 1, 0, 0}), r), 1);            // run: no flags, no seeds
    check_header(r, 0x61, 47, Status::Ok, 3);
    CHECK_EQ(run(f, request(0x61, 48, {6, 1, 3, 0}), r), 1);            // run: sweep + confirm
    check_header(r, 0x61, 48, Status::Ok, 3);
    CHECK_EQ(run(f, request(0x61, 49, {6, 1, 1, 1, 0xD9, 0x22, 31, 0}), r), 1);   // one seed, no data objects
    check_header(r, 0x61, 49, Status::Ok, 3);
    CHECK_EQ(int(f.em_req.size()), 8);
    f.calls = 0;
    CHECK_EQ(run(f, request(0x61, 50, {6, 1, 4, 0}), r), 1);            // unknown flag bit
    check_header(r, 0x61, 50, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x61, 51, {6, 1, 0, 1}), r), 1);            // seed count without the seed
    check_header(r, 0x61, 51, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x61, 52, {6, 1, 0, 0, 0}), r), 1);         // trailing byte
    check_header(r, 0x61, 52, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x61, 53, {6, 1, 0, 5}), r), 1);            // more than four seeds
    check_header(r, 0x61, 53, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x61, 54, {6, 1}), r), 1);                  // run without options
    check_header(r, 0x61, 54, Status::BadArg, 0);
    CHECK_EQ(f.calls, 0);
    // Unmatched [7, i]: totals (255) and the four held entries; anything else is refused.
    CHECK_EQ(run(f, request(0x61, 60, {7, 255}), r), 1);
    check_header(r, 0x61, 60, Status::Ok, 3);
    CHECK_EQ(run(f, request(0x61, 61, {7, 0}), r), 1);
    check_header(r, 0x61, 61, Status::Ok, 3);
    CHECK_EQ(run(f, request(0x61, 62, {7, 3}), r), 1);
    check_header(r, 0x61, 62, Status::Ok, 3);
    f.calls = 0;
    CHECK_EQ(run(f, request(0x61, 63, {7, 4}), r), 1);
    check_header(r, 0x61, 63, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x61, 64, {7}), r), 1);
    check_header(r, 0x61, 64, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x61, 65, {7, 0, 0}), r), 1);
    check_header(r, 0x61, 65, Status::BadLength, 0);
    CHECK_EQ(f.calls, 0);
}

static void test_transcap() {
    Fake f; Frame r;
    // Argument-less ops: exactly the op byte.
    for (uint8_t op : {kTcStatus, kTcArm, kTcTrigger, kTcDisarm, kTcSave, kTcList, kTcClear}) {
        CHECK_EQ(run(f, request(0x71, 1, {op}), r), 1);
        check_header(r, 0x71, 1, Status::Ok, 2);
        check_bytes(r, kRspHeader, {0x7C, op});
        CHECK_EQ(f.tc_req.size(), 1u); CHECK_EQ(f.tc_req[0], op);
        CHECK_EQ(run(f, request(0x71, 2, {op, 0}), r), 1);
        check_header(r, 0x71, 2, Status::BadLength, 0);
    }
    // Live read: op + u32 offset; stored read: op + seq + offset.
    CHECK_EQ(run(f, request(0x71, 3, {kTcLive, 0x10, 0x20, 0, 0}), r), 1);
    check_header(r, 0x71, 3, Status::Ok, 2);
    CHECK_EQ(f.tc_req.size(), 5u); CHECK_EQ(f.tc_req[1], 0x10);
    CHECK_EQ(run(f, request(0x71, 4, {kTcLive, 0, 0}), r), 1);
    check_header(r, 0x71, 4, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x71, 5, {kTcStored, 1, 0, 0, 0, 0x40, 0, 0, 0}), r), 1);
    check_header(r, 0x71, 5, Status::Ok, 2);
    CHECK_EQ(f.tc_req.size(), 9u);
    CHECK_EQ(run(f, request(0x71, 6, {kTcStored, 1, 0, 0, 0}), r), 1);
    check_header(r, 0x71, 6, Status::BadLength, 0);
    // No op, unknown op.
    CHECK_EQ(run(f, request(0x71, 7, {}), r), 1);
    check_header(r, 0x71, 7, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x71, 8, {9}), r), 1);
    check_header(r, 0x71, 8, Status::BadArg, 0);
}
static void test_blackbox() {
    Fake f; Frame r;
    // Read: the cursor is a u32 (default 0); the backend's bytes come back as the payload.
    CHECK_EQ(run(f, request(0x70, 1, {0, 0x10, 0x20, 0x00, 0x00}), r), 1);
    check_header(r, 0x70, 1, Status::Ok, 5);
    check_bytes(r, kRspHeader, {0xA5, 0x78, 0x56, 0x34, 0x12});
    CHECK_EQ(f.bb_op, 0); CHECK_EQ(f.bb_arg, 0x2010u);
    CHECK_EQ(run(f, request(0x70, 2, {0}), r), 1);
    check_header(r, 0x70, 2, Status::Ok, 5);
    CHECK_EQ(f.bb_arg, 0u);
    // Clear, freeze (arg 1 / 0), status.
    CHECK_EQ(run(f, request(0x70, 3, {1}), r), 1);
    CHECK_EQ(f.bb_op, 1);
    CHECK_EQ(run(f, request(0x70, 4, {2, 1}), r), 1);
    CHECK_EQ(f.bb_op, 2); CHECK_EQ(f.bb_arg, 1u);
    CHECK_EQ(run(f, request(0x70, 5, {3}), r), 1);
    CHECK_EQ(f.bb_op, 3);
    // Captures: list / clear take no argument, read takes slot and a u16 offset (packed slot << 16 | offset).
    CHECK_EQ(run(f, request(0x70, 20, {4}), r), 1);
    CHECK_EQ(f.bb_op, 4);
    CHECK_EQ(run(f, request(0x70, 21, {5, 2, 0x34, 0x01}), r), 1);
    CHECK_EQ(f.bb_op, 5); CHECK_EQ(f.bb_arg, 0x20134u);
    CHECK_EQ(run(f, request(0x70, 22, {6}), r), 1);
    CHECK_EQ(f.bb_op, 6);
    // Bad lengths and an unknown op never reach the backend.
    const int calls = f.calls;
    CHECK_EQ(run(f, request(0x70, 23, {5, 2}), r), 1);
    check_header(r, 0x70, 23, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x70, 24, {4, 0}), r), 1);
    check_header(r, 0x70, 24, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x70, 6, {}), r), 1);
    check_header(r, 0x70, 6, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x70, 7, {0, 1, 2}), r), 1);
    check_header(r, 0x70, 7, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x70, 8, {2}), r), 1);
    check_header(r, 0x70, 8, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x70, 9, {1, 1}), r), 1);
    check_header(r, 0x70, 9, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x70, 10, {9}), r), 1);
    check_header(r, 0x70, 10, Status::BadArg, 0);
    CHECK_EQ(f.calls, calls);
}

static void test_fc() {
    {
        FcInfo info;
        info.num_results = 7;
        for (size_t k = 0; k < info.results.size(); k++) info.results[k] = {0, 1, 9000};
        info.results.back() = {2, 1, 6000};
        std::array<uint8_t, kMaxRspPayload> payload{};
        CHECK_EQ(encode_fc(payload, info), 38);
        CHECK_EQ(payload[9], 7);
        CHECK_EQ(payload[34], 2);
        CHECK_EQ(payload[36] | payload[37] << 8, 6000);
    }
    Fake f; Frame r;
    // Status is read-only and needs no confirmation.
    CHECK_EQ(run(f, request(0x32, 1, {0}), r), 1);
    check_header(r, 0x32, 1, Status::Ok, 10 + 8);
    check_bytes(r, kRspHeader, {2, 1, 0, 1, 3, 0x28, 0x23, 0xF6, 0x22, 2,
                                4, 1, 0x32, 0x23, 5, 2, 0x00, 0x00});
    CHECK_EQ(f.fc_calls, 0);
    // Reset: always allowed.
    CHECK_EQ(run(f, request(0x32, 2, {1}), r), 1);
    check_header(r, 0x32, 2, Status::Ok, 10 + 8);
    CHECK_EQ(f.fc_op, 1);
    // Scan without the confirm byte is refused before the backend sees it.
    f.fc_calls = 0;
    CHECK_EQ(run(f, request(0x32, 3, {2}), r), 1);
    check_header(r, 0x32, 3, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x32, 4, {2, 0}), r), 1);
    check_header(r, 0x32, 4, Status::BadArg, 0);
    CHECK_EQ(f.fc_calls, 0);
    CHECK_EQ(run(f, request(0x32, 5, {2, 1}), r), 1);
    check_header(r, 0x32, 5, Status::Ok, 10 + 8);
    CHECK_EQ(f.fc_op, 2); CHECK_EQ(f.fc_flags, 1);
    // Trigger: proto, mV (LE), confirm. Above 5 V without confirm is refused.
    f.fc_calls = 0;
    CHECK_EQ(run(f, request(0x32, 6, {3, 0, 0x28, 0x23, 0}), r), 1);   // QC2 9000 mV, no confirm
    check_header(r, 0x32, 6, Status::BadArg, 0);
    CHECK_EQ(f.fc_calls, 0);
    CHECK_EQ(run(f, request(0x32, 7, {3, 0, 0x28, 0x23, 1}), r), 1);
    check_header(r, 0x32, 7, Status::Ok, 10 + 8);
    CHECK_EQ(f.fc_op, 3); CHECK_EQ(f.fc_proto, 0); CHECK_EQ(f.fc_mv, 9000); CHECK_EQ(f.fc_flags, 1);
    // 5 V needs no confirm.
    CHECK_EQ(run(f, request(0x32, 8, {3, 6, 0x88, 0x13, 0}), r), 1);   // VOOC 5000 mV
    check_header(r, 0x32, 8, Status::Ok, 10 + 8);
    CHECK_EQ(f.fc_proto, 6); CHECK_EQ(f.fc_flags, 0);
    // Bad proto, bad op, bad lengths.
    f.fc_calls = 0;
    CHECK_EQ(run(f, request(0x32, 9, {3, 11, 0x88, 0x13, 1}), r), 1);
    check_header(r, 0x32, 9, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x32, 10, {7}), r), 1);
    check_header(r, 0x32, 10, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x32, 11, {3, 0, 0x28}), r), 1);
    check_header(r, 0x32, 11, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x32, 12, {}), r), 1);
    check_header(r, 0x32, 12, Status::BadLength, 0);
    CHECK_EQ(f.fc_calls, 0);
    // Flags: bit0 confirms the voltage, bit1 overrides the PD-contract / load guard. The override
    // alone is no confirmation; unknown bits are refused.
    CHECK_EQ(run(f, request(0x32, 20, {3, 0, 0x28, 0x23, 3}), r), 1);
    check_header(r, 0x32, 20, Status::Ok, 10 + 8);
    CHECK_EQ(f.fc_flags, 3);
    CHECK_EQ(run(f, request(0x32, 21, {2, 3}), r), 1);
    check_header(r, 0x32, 21, Status::Ok, 10 + 8);
    CHECK_EQ(f.fc_flags, 3);
    f.fc_calls = 0;
    CHECK_EQ(run(f, request(0x32, 22, {3, 0, 0x28, 0x23, 2}), r), 1);   // 9 V, override only
    check_header(r, 0x32, 22, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x32, 23, {2, 2}), r), 1);                  // scan, override only
    check_header(r, 0x32, 23, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x32, 24, {3, 0, 0x88, 0x13, 4}), r), 1);   // unknown bit
    check_header(r, 0x32, 24, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x32, 25, {3, 6, 0x88, 0x13, 2}), r), 1);   // 5 V with the override only: fine
    check_header(r, 0x32, 25, Status::Ok, 10 + 8);
    CHECK_EQ(f.fc_flags, 2);
    CHECK_EQ(f.fc_calls, 1);
    // A backend refusal is passed on.
    f.fc_status = Status::Busy;
    CHECK_EQ(run(f, request(0x32, 13, {3, 0, 0x88, 0x13, 0}), r), 1);
    check_header(r, 0x32, 13, Status::Busy, 0);
}

static void test_ripple() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x22, 6, {}), r), 1);                 // default sample code 0
    check_header(r, 0x22, 6, Status::Ok, 13);
    CHECK_EQ(f.last_code, 0);
    check_bytes(r, kRspHeader, {0, 0x01, 0x02, 0x03, 0x00,        // code, period 0x00030201 ns
                                0x00, 0x02,                       // 512 samples
                                0x10, 0x00, 0xF0, 0x0F, 0x00, 0x08});
    CHECK_EQ(run(f, request(0x22, 7, {5}), r), 1);
    check_header(r, 0x22, 7, Status::Ok, 13);
    CHECK_EQ(f.last_code, 5);
    CHECK_EQ(r[6], 5);
    f.last_code = 0xFF;
    CHECK_EQ(run(f, request(0x22, 8, {8}), r), 1);                // codes are 0..7
    check_header(r, 0x22, 8, Status::BadArg, 0);
    CHECK_EQ(f.last_code, 0xFF);
    CHECK_EQ(run(f, request(0x22, 9, {1, 2}), r), 1);
    check_header(r, 0x22, 9, Status::BadLength, 0);
    f.ripple_status = Status::Busy;                               // capture timed out
    CHECK_EQ(run(f, request(0x22, 10, {}), r), 1);
    check_header(r, 0x22, 10, Status::Busy, 0);
}

static void test_ripple_frame() {
    Frame r; r.fill(0xEE);
    const uint16_t s[3] = {0x0102, 0x0A0B, 0x0FFF};
    size_t n = build_ripple_frame(r, 9, 56, s);
    CHECK_EQ(n, 6 + 2 + 6);
    const uint8_t want[14] = {0xA6, 0x4B, 0x8B, 9, 0, 8, 56, 0, 0x02, 0x01, 0x0B, 0x0A, 0xFF, 0x0F};
    CHECK_EQ(memcmp(r.data(), want, 14), 0);
    CHECK_EQ(r[14], 0);                                           // zero padded
    // a full chunk is 28 samples; more are cut
    std::array<uint16_t, 40> big{};
    n = build_ripple_frame(r, 1, 500, big);
    CHECK_EQ(n, 64);
    CHECK_EQ(r[5], 58);
    CHECK_EQ(r[6], 500 & 0xFF); CHECK_EQ(r[7], 500 >> 8);
    CHECK_EQ(kRippleChunk, 28);
}

static void test_pdos() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x04, 1, {}), r), 1);                // nothing decoded yet
    check_header(r, 0x04, 1, Status::Ok, 4);
    check_bytes(r, kRspHeader, {0, 0, 0, 0});

    f.snap.valid = 1; f.snap.spec_rev = 2; f.snap.num_pdos = 2;
    f.snap.pdo[0] = {1, 0x0001912C, 5000, 5000, 3000, 15000};    // fixed 5 V 3 A
    f.snap.pdo[1] = {4, 0xC0A4C8C8, 3300, 11000, 5000, 55000};   // PPS
    CHECK_EQ(run(f, request(0x04, 2, {0}), r), 1);
    check_header(r, 0x04, 2, Status::Ok, 19);
    check_bytes(r, kRspHeader, {1, 2, 2, 0,
                                1, 0x2C, 0x91, 0x01, 0x00, 0x88, 0x13, 0x88, 0x13, 0xB8, 0x0B, 0x98, 0x3A, 0x00, 0x00});
    CHECK_EQ(run(f, request(0x04, 3, {1}), r), 1);
    check_header(r, 0x04, 3, Status::Ok, 19);
    check_bytes(r, kRspHeader, {1, 2, 2, 1, 4, 0xC8, 0xC8, 0xA4, 0xC0, 0xE4, 0x0C, 0xF8, 0x2A, 0x88, 0x13, 0xD8, 0xD6, 0x00, 0x00});
    CHECK_EQ(run(f, request(0x04, 4, {2}), r), 1);               // one past the end
    check_header(r, 0x04, 4, Status::BadArg, 0);
}

static void test_emarker() {
    Fake f; Frame r;
    f.em.valid = 1; f.em.is_active = 1; f.em.vid = 0x05AC; f.em.pid = 0x1234; f.em.xid = 0x11223344;
    f.em.hw_rev = 2; f.em.fw_rev = 3; f.em.max_a = 5; f.em.max_v = 50; f.em.max_w = 240; f.em.num_vdos = 2;
    f.em.vendor = "Apple"; f.em.speed = "USB4";
    f.em.vdo[0] = 0xAABBCCDD; f.em.vdo[1] = 0x01020304;

    CHECK_EQ(run(f, request(0x05, 1, {}), r), 1);
    check_header(r, 0x05, 1, Status::Ok, 17 + 6 + 4);
    check_bytes(r, kRspHeader, {1, 1, 0xAC, 0x05, 0x34, 0x12, 0x44, 0x33, 0x22, 0x11,
                                2, 3, 5, 50, 240, 0, 2,
                                'A', 'p', 'p', 'l', 'e', 0, 'U', 'S', 'B', '4'});
    CHECK_EQ(run(f, request(0x05, 2, {1}), r), 1);
    check_header(r, 0x05, 2, Status::Ok, 8);
    check_bytes(r, kRspHeader, {0xDD, 0xCC, 0xBB, 0xAA, 0x04, 0x03, 0x02, 0x01});
    f.em.sop = 1; f.em.spec_rev = 2; f.em.vdo_version = 1; f.em.latency_code = 3; f.em.termination = 3;
    f.em.flags = kEmFlagEpr | kEmFlagVdo2 | kEmFlagThunderbolt;
    f.em.max_temp_c = 60; f.em.shutdown_temp_c = 70; f.em.u3_power = 2; f.em.usb_lanes = 2;
    CHECK_EQ(run(f, request(0x05, 3, {2}), r), 1);
    check_header(r, 0x05, 3, Status::Ok, 10);
    check_bytes(r, kRspHeader, {1, 2, 1, 3, 3, 0x8C, 60, 70, 2, 2});
    CHECK_EQ(run(f, request(0x05, 4, {3}), r), 1);
    check_header(r, 0x05, 4, Status::BadArg, 0);
}

static void test_identity() {
    Fake f; Frame r;
    f.id.flags = kIdFlagEpr | kIdFlagCuktech; f.id.valid = 1;
    f.id.vid = 0x05AC; f.id.pid = 0x1234; f.id.bcd = 0x0100;
    f.id.num_svids = 2; f.id.svids[0] = 0xFF01; f.id.svids[1] = 0x8087;
    f.id.vendor = "Apple"; f.id.model = "iPad";
    CHECK_EQ(run(f, request(0x24, 1, {}), r), 1);
    check_header(r, 0x24, 1, Status::Ok, 9 + 4 + 6 + 4);
    check_bytes(r, kRspHeader, {5, 1, 0xAC, 0x05, 0x34, 0x12, 0x00, 0x01, 2, 0x01, 0xFF, 0x87, 0x80,
                                'A', 'p', 'p', 'l', 'e', 0, 'i', 'P', 'a', 'd'});
    CHECK_EQ(run(f, request(0x24, 2, {0}), r), 1);
    check_header(r, 0x24, 2, Status::BadLength, 0);
}

// DFU_ENTER (0x30) needs the confirm magic "DFU!" (44 46 55 21) as its whole payload.
static void test_dfu_enter() {
    Fake f; Frame r;
    CHECK_EQ(k2::remote::kDfuMagic, 0x21554644u);
    CHECK_EQ(run(f, request(0x30, 1, {0x44, 0x46, 0x55, 0x21}), r), 1);
    check_header(r, 0x30, 1, Status::Ok, 0);
    CHECK_EQ(f.dfus, 1);
    // no payload, short, long, wrong magic, byte-swapped magic: refused, backend not called
    CHECK_EQ(run(f, request(0x30, 2, {}), r), 1);
    check_header(r, 0x30, 2, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x30, 3, {0x44, 0x46, 0x55}), r), 1);
    check_header(r, 0x30, 3, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x30, 4, {0x44, 0x46, 0x55, 0x21, 0, 0}), r), 1);
    check_header(r, 0x30, 4, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x30, 5, {0x44, 0x46, 0x55, 0x22}), r), 1);
    check_header(r, 0x30, 5, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x30, 6, {0x21, 0x55, 0x46, 0x44}), r), 1);
    check_header(r, 0x30, 6, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x30, 7, {0, 0, 0, 0}), r), 1);
    check_header(r, 0x30, 7, Status::BadArg, 0);
    CHECK_EQ(f.dfus, 1);
    CHECK_EQ(f.dfu_mode, 0);                 // the 4-byte form means the system reset
    // optional 5th byte: reset mode 0 = sys, 1 = iwdg, anything else refused
    CHECK_EQ(run(f, request(0x30, 9, {0x44, 0x46, 0x55, 0x21, 1}), r), 1);
    check_header(r, 0x30, 9, Status::Ok, 0);
    CHECK_EQ(f.dfus, 2);
    CHECK_EQ(f.dfu_mode, 1);
    CHECK_EQ(run(f, request(0x30, 10, {0x44, 0x46, 0x55, 0x21, 0}), r), 1);
    check_header(r, 0x30, 10, Status::Ok, 0);
    CHECK_EQ(f.dfus, 3);
    CHECK_EQ(f.dfu_mode, 0);
    CHECK_EQ(run(f, request(0x30, 11, {0x44, 0x46, 0x55, 0x21, 2}), r), 1);
    check_header(r, 0x30, 11, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x30, 12, {0x44, 0x46, 0x55, 0x22, 1}), r), 1);
    check_header(r, 0x30, 12, Status::BadArg, 0);
    CHECK_EQ(f.dfus, 3);
    // REBOOT and the other commands never trigger it
    CHECK_EQ(run(f, request(0x08, 8, {}), r), 1);
    CHECK_EQ(f.dfus, 3);
}

// FACTORY_WIPE (0x33): magic "WIPE" (57 49 50 45, u32 0x45504957) + optional reset byte (0 sys, 1 iwdg).
// The reply carries the backend's wipe result: err(1) sectors(2) erased(2) verified(2) failed(2)
// first_fail(4) status(1), 14 bytes, always with Status::Ok once the request parsed.
static void test_factory_wipe() {
    Fake f; Frame r;
    CHECK_EQ(k2::remote::kFactoryWipeMagic, 0x45504957u);
    f.wipe_snap.err = 2; f.wipe_snap.sectors = 152; f.wipe_snap.erased = 152; f.wipe_snap.verified = 150;
    f.wipe_snap.failed = 2; f.wipe_snap.first_fail = 0x080FA000; f.wipe_snap.status = 6;
    CHECK_EQ(run(f, request(0x33, 1, {0x57, 0x49, 0x50, 0x45}), r), 1);
    check_header(r, 0x33, 1, Status::Ok, 14);
    check_bytes(r, kRspHeader, {2, 0x98, 0x00, 0x98, 0x00, 0x96, 0x00, 0x02, 0x00, 0x00, 0xA0, 0x0F, 0x08, 6});
    CHECK_EQ(f.wipes, 1);
    CHECK_EQ(f.wipe_mode, 0);                // the 4-byte form means the system reset
    CHECK_EQ(run(f, request(0x33, 2, {0x57, 0x49, 0x50, 0x45, 1}), r), 1);
    check_header(r, 0x33, 2, Status::Ok, 14);
    CHECK_EQ(f.wipes, 2);
    CHECK_EQ(f.wipe_mode, 1);
    // no payload, short, long, wrong magic (the DFU one included), bad reset byte: refused, backend not called
    CHECK_EQ(run(f, request(0x33, 3, {}), r), 1);
    check_header(r, 0x33, 3, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x33, 4, {0x57, 0x49, 0x50}), r), 1);
    check_header(r, 0x33, 4, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x33, 5, {0x57, 0x49, 0x50, 0x45, 0, 0}), r), 1);
    check_header(r, 0x33, 5, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x33, 6, {0x44, 0x46, 0x55, 0x21}), r), 1);
    check_header(r, 0x33, 6, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x33, 7, {0, 0, 0, 0}), r), 1);
    check_header(r, 0x33, 7, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x33, 8, {0x57, 0x49, 0x50, 0x45, 2}), r), 1);
    check_header(r, 0x33, 8, Status::BadArg, 0);
    CHECK_EQ(f.wipes, 2);
    // the wipe magic does not enter DFU
    CHECK_EQ(run(f, request(0x30, 9, {0x57, 0x49, 0x50, 0x45}), r), 1);
    check_header(r, 0x30, 9, Status::BadArg, 0);
    CHECK_EQ(f.dfus, 0);
    CHECK_EQ(f.wipes, 2);
}

// FLASH_DUMP (0x34): read-only window into the native device storage, 0x080E0000..0x08100000.
// req addr(4) len(1), len 1..48; rsp addr(4) len(1) data(len). The backend only copies memory.
static void test_flash_dump() {
    Fake f; Frame r;
    // first window of the native area: 16 bytes
    CHECK_EQ(run(f, request(0x34, 1, {0x00, 0x00, 0x0E, 0x08, 16}), r), 1);
    check_header(r, 0x34, 1, Status::Ok, 5 + 16);
    check_bytes(r, kRspHeader, {0x00, 0x00, 0x0E, 0x08, 16});
    for (int i = 0; i < 16; i++) CHECK_EQ(r[kRspHeader + 5 + i], static_cast<uint8_t>((0x080E0000 + i) ^ 0x5A));
    CHECK_EQ(f.dump_calls, 1);
    CHECK_EQ(f.dump_addr, 0x080E0000u);
    // the largest window, flush with the end of the flash
    CHECK_EQ(run(f, request(0x34, 2, {0xD0, 0xFF, 0x0F, 0x08, 48}), r), 1);
    check_header(r, 0x34, 2, Status::Ok, 5 + 48);
    check_bytes(r, kRspHeader, {0xD0, 0xFF, 0x0F, 0x08, 48});
    CHECK_EQ(f.dump_calls, 2);
    // one past the end, one below the area, zero and oversize lengths, a wrap-around: BadArg, nothing read
    CHECK_EQ(run(f, request(0x34, 3, {0xD1, 0xFF, 0x0F, 0x08, 48}), r), 1);
    check_header(r, 0x34, 3, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x34, 4, {0xFF, 0xFF, 0x0D, 0x08, 1}), r), 1);
    check_header(r, 0x34, 4, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x34, 5, {0x00, 0x00, 0x0E, 0x08, 0}), r), 1);
    check_header(r, 0x34, 5, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x34, 6, {0x00, 0x00, 0x0E, 0x08, 49}), r), 1);
    check_header(r, 0x34, 6, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x34, 7, {0xF0, 0xFF, 0xFF, 0xFF, 48}), r), 1);
    check_header(r, 0x34, 7, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x34, 8, {0x00, 0x30, 0x00, 0x08, 16}), r), 1);   // the bootloader/app area is not offered
    check_header(r, 0x34, 8, Status::BadArg, 0);
    // wrong payload sizes
    CHECK_EQ(run(f, request(0x34, 9, {}), r), 1);
    check_header(r, 0x34, 9, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x34, 10, {0x00, 0x00, 0x0E, 0x08}), r), 1);
    check_header(r, 0x34, 10, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x34, 11, {0x00, 0x00, 0x0E, 0x08, 16, 0}), r), 1);
    check_header(r, 0x34, 11, Status::BadLength, 0);
    CHECK_EQ(f.dump_calls, 2);
}

// BOOT_INFO (0x35): part 0 = the K2B1 text in 48-byte chunks (rendered at offset 0 only), part 1 = 9-byte power samples, 5 per reply.
// UFCS (0x52): status block, field get / set with range check, reset / probe / stop, blob read in chunks.
static void test_ufcs() {
    Fake f; Frame r;
    const auto &d1 = k2::ufcs::kFields[1];                 // any field with a real range
    f.ufcs_state = {2, 4, 2, 0, 0};
    f.ufcs_blob.resize(100);
    for (size_t i = 0; i < f.ufcs_blob.size(); i++) f.ufcs_blob[i] = uint8_t(i + 1);
    // status: state verdict class start blob_len(2) nfields
    CHECK_EQ(run(f, request(0x52, 1, {0}), r), 1);
    check_header(r, 0x52, 1, Status::Ok, 7);
    check_bytes(r, kRspHeader, {2, 4, 2, 0, 100, 0, uint8_t(k2::ufcs::kFieldCount)});
    // get: field value lo hi def (u32 le), 17 bytes
    f.ufcs_vals[1] = d1.def;
    CHECK_EQ(run(f, request(0x52, 2, {1, 1}), r), 1);
    check_header(r, 0x52, 2, Status::Ok, 17);
    CHECK_EQ(r[kRspHeader], 1);
    CHECK_EQ(r[kRspHeader + 1], uint8_t(d1.def));
    CHECK_EQ(r[kRspHeader + 5], uint8_t(d1.lo));
    CHECK_EQ(r[kRspHeader + 9], uint8_t(d1.hi));
    CHECK_EQ(r[kRspHeader + 13], uint8_t(d1.def));
    // get: bad field, bad length
    CHECK_EQ(run(f, request(0x52, 3, {1, uint8_t(k2::ufcs::kFieldCount)}), r), 1);
    check_header(r, 0x52, 3, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x52, 4, {1}), r), 1);
    check_header(r, 0x52, 4, Status::BadLength, 0);
    // set: in range is stored and echoed
    CHECK_EQ(run(f, request(0x52, 5, {2, 1, uint8_t(d1.hi), 0, 0, 0}), r), 1);
    check_header(r, 0x52, 5, Status::Ok, 17);
    CHECK_EQ(f.ufcs_vals[1], d1.hi);
    CHECK_EQ(r[kRspHeader + 1], uint8_t(d1.hi));
    // set: out of range (hi + 1 as u32) is BadArg and not stored
    const uint32_t bad = d1.hi + 1;
    CHECK_EQ(run(f, request(0x52, 6, {2, 1, uint8_t(bad), uint8_t(bad >> 8), uint8_t(bad >> 16), uint8_t(bad >> 24)}), r), 1);
    check_header(r, 0x52, 6, Status::BadArg, 0);
    CHECK_EQ(f.ufcs_vals[1], d1.hi);
    CHECK_EQ(run(f, request(0x52, 7, {2, 1, 0}), r), 1);
    check_header(r, 0x52, 7, Status::BadLength, 0);
    // set while the backend is busy
    f.ufcs_busy = Status::Busy;
    CHECK_EQ(run(f, request(0x52, 8, {2, 1, uint8_t(d1.lo), 0, 0, 0}), r), 1);
    check_header(r, 0x52, 8, Status::Busy, 0);
    f.ufcs_busy = Status::Ok;
    // ops: reset(3) probe(4) stop(5) reach the backend and answer with the status block
    CHECK_EQ(run(f, request(0x52, 9, {3}), r), 1);
    check_header(r, 0x52, 9, Status::Ok, 7);
    CHECK_EQ(run(f, request(0x52, 10, {4}), r), 1);
    CHECK_EQ(run(f, request(0x52, 11, {5}), r), 1);
    CHECK_EQ(f.ufcs_ops.size(), size_t(3));
    CHECK_EQ(f.ufcs_ops[0], 3); CHECK_EQ(f.ufcs_ops[1], 4); CHECK_EQ(f.ufcs_ops[2], 5);
    f.ufcs_busy = Status::Busy;
    CHECK_EQ(run(f, request(0x52, 12, {4}), r), 1);
    check_header(r, 0x52, 12, Status::Busy, 0);
    f.ufcs_busy = Status::Ok;
    CHECK_EQ(run(f, request(0x52, 13, {4, 0}), r), 1);
    check_header(r, 0x52, 13, Status::BadLength, 0);
    // unknown op
    CHECK_EQ(run(f, request(0x52, 14, {9}), r), 1);
    check_header(r, 0x52, 14, Status::BadArg, 0);
    // read: chunk 0 = 48 bytes, chunk at 96 = 4 bytes, the end offset is empty, past it BadArg
    CHECK_EQ(run(f, request(0x52, 15, {6, 0, 0}), r), 1);
    check_header(r, 0x52, 15, Status::Ok, 5 + 48);
    check_bytes(r, kRspHeader, {0, 0, 100, 0, 48, 1, 2});
    CHECK_EQ(r[kRspHeader + 5 + 47], 48);
    CHECK_EQ(run(f, request(0x52, 16, {6, 96, 0}), r), 1);
    check_header(r, 0x52, 16, Status::Ok, 5 + 4);
    check_bytes(r, kRspHeader, {96, 0, 100, 0, 4, 97, 98, 99, 100});
    CHECK_EQ(run(f, request(0x52, 17, {6, 100, 0}), r), 1);
    check_header(r, 0x52, 17, Status::Ok, 5);
    CHECK_EQ(run(f, request(0x52, 18, {6, 101, 0}), r), 1);
    check_header(r, 0x52, 18, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x52, 19, {6, 0}), r), 1);
    check_header(r, 0x52, 19, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x52, 20, {}), r), 1);
    check_header(r, 0x52, 20, Status::BadLength, 0);
    // encoder: a bad field encodes to nothing
    uint8_t tmp[32];
    CHECK_EQ(k2::remote::encode_ufcs_field(std::span<uint8_t>(tmp), uint8_t(k2::ufcs::kFieldCount), 0), size_t(0));
}

static void test_boot_info() {
    Fake f; Frame r;
    f.boot_line.assign(100, 'x');
    for (size_t i = 0; i < f.boot_line.size(); i++) f.boot_line[i] = char('a' + i % 26);
    // chunk 0: the text is rendered, header part off total n
    CHECK_EQ(run(f, request(0x35, 1, {0, 0, 0}), r), 1);
    check_header(r, 0x35, 1, Status::Ok, 6 + 48);
    check_bytes(r, kRspHeader, {0, 0, 0, 100, 0, 48});
    for (int i = 0; i < 48; i++) CHECK_EQ(r[kRspHeader + 6 + i], uint8_t('a' + i % 26));
    CHECK_EQ(f.boot_renders, 1);
    // chunk 2 is the short tail and does not re-render the text
    CHECK_EQ(run(f, request(0x35, 2, {0, 96, 0}), r), 1);
    check_header(r, 0x35, 2, Status::Ok, 6 + 4);
    check_bytes(r, kRspHeader, {0, 96, 0, 100, 0, 4});
    CHECK_EQ(r[kRspHeader + 6], uint8_t('a' + 96 % 26));
    CHECK_EQ(f.boot_renders, 1);
    // the end offset is an empty chunk; past it is BadArg
    CHECK_EQ(run(f, request(0x35, 3, {0, 100, 0}), r), 1);
    check_header(r, 0x35, 3, Status::Ok, 6);
    CHECK_EQ(run(f, request(0x35, 4, {0, 101, 0}), r), 1);
    check_header(r, 0x35, 4, Status::BadArg, 0);
    // samples: 7 kept, 5 in the first reply (9 bytes each: vbus cc1 cc2 aux u16 le, flags), 2 in the second
    for (unsigned i = 0; i < 7; i++) f.boot_samples.push_back({0, std::uint16_t(5000 + i), std::uint16_t(3300 + i), std::uint16_t(i), std::uint16_t(9000 + i), i == 3});
    CHECK_EQ(run(f, request(0x35, 5, {1, 0, 0}), r), 1);
    check_header(r, 0x35, 5, Status::Ok, 6 + 5 * 9);
    check_bytes(r, kRspHeader, {1, 0, 0, 7, 0, 5});
    check_bytes(r, kRspHeader + 6, {uint8_t(5000 & 255), uint8_t(5000 >> 8), uint8_t(3300 & 255), uint8_t(3300 >> 8), 0, 0,
                                    uint8_t(9000 & 255), uint8_t(9000 >> 8), 0});
    CHECK_EQ(r[kRspHeader + 6 + 3 * 9 + 8], 1);   // sample 3 had PLUS held
    CHECK_EQ(run(f, request(0x35, 6, {1, 5, 0}), r), 1);
    check_header(r, 0x35, 6, Status::Ok, 6 + 2 * 9);
    check_bytes(r, kRspHeader, {1, 5, 0, 7, 0, 2});
    CHECK_EQ(run(f, request(0x35, 7, {1, 8, 0}), r), 1);
    check_header(r, 0x35, 7, Status::BadArg, 0);
    // unknown part, wrong sizes
    CHECK_EQ(run(f, request(0x35, 8, {2, 0, 0}), r), 1);
    check_header(r, 0x35, 8, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x35, 9, {0, 0}), r), 1);
    check_header(r, 0x35, 9, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x35, 10, {0, 0, 0, 0}), r), 1);
    check_header(r, 0x35, 10, Status::BadLength, 0);
    // a full 240-byte text is read in five chunks
    f.boot_line.assign(240, 'q');
    size_t got = 0;
    for (unsigned off = 0; off < 240; off += 48) {
        CHECK_EQ(run(f, request(0x35, uint8_t(20 + off / 48), {0, uint8_t(off), uint8_t(off >> 8)}), r), 1);
        check_header(r, 0x35, uint8_t(20 + off / 48), Status::Ok, 6 + 48);
        got += r[kRspHeader + 5];
    }
    CHECK_EQ(got, 240u);
}

// DEBUG_FAULT (0x31): magic "BOOM" + kind(1), exactly 5 bytes; kinds 0 and 1 only.
static void test_debug_fault() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x31, 1, {'B', 'O', 'O', 'M', 0}), r), 1);
    check_header(r, 0x31, 1, Status::Ok, 0);
    CHECK_EQ(f.faults, 1);
    CHECK_EQ(f.fault_kind, 0);
    CHECK_EQ(run(f, request(0x31, 2, {'B', 'O', 'O', 'M', 1}), r), 1);
    check_header(r, 0x31, 2, Status::Ok, 0);
    CHECK_EQ(f.fault_kind, 1);
    CHECK_EQ(run(f, request(0x31, 3, {'B', 'O', 'O', 'M'}), r), 1);
    check_header(r, 0x31, 3, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x31, 4, {'B', 'O', 'O', 'X', 0}), r), 1);
    check_header(r, 0x31, 4, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x31, 5, {'B', 'O', 'O', 'M', 3}), r), 1);
    check_header(r, 0x31, 5, Status::BadArg, 0);
    CHECK_EQ(f.faults, 2);
    CHECK_EQ(run(f, request(0x31, 6, {'B', 'O', 'O', 'M', 2}), r), 1);   // 2 = stack overflow into the MPU guard
    check_header(r, 0x31, 6, Status::Ok, 0);
    CHECK_EQ(f.fault_kind, 2);
    CHECK_EQ(f.faults, 3);
}

static void test_action_page_reboot_log() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x06, 1, {2}), r), 1);
    check_header(r, 0x06, 1, Status::Ok, 1);
    CHECK_EQ(r[kRspHeader], 2);
    CHECK_EQ(f.last_action, 2);
    CHECK_EQ(run(f, request(0x06, 2, {}), r), 1);
    check_header(r, 0x06, 2, Status::BadLength, 0);
    CHECK_EQ(f.last_confirm, 0);
    CHECK_EQ(run(f, request(0x06, 5, {2, 1}), r), 1);   // optional second byte: confirm
    check_header(r, 0x06, 5, Status::Ok, 1);
    CHECK_EQ(f.last_confirm, 1);

    CHECK_EQ(run(f, request(0x07, 3, {3}), r), 1);
    check_header(r, 0x07, 3, Status::Ok, 1);
    CHECK_EQ(f.last_page, 3);
    f.page_status = Status::BadArg;
    CHECK_EQ(run(f, request(0x07, 4, {9}), r), 1);
    check_header(r, 0x07, 4, Status::BadArg, 0);

    CHECK_EQ(run(f, request(0x08, 5, {}), r), 1);
    check_header(r, 0x08, 5, Status::Ok, 0);
    CHECK_EQ(f.reboots, 1);

    CHECK_EQ(run(f, request(0x09, 6, {}), r), 1);                // query only
    check_header(r, 0x09, 6, Status::Ok, 1);
    CHECK_EQ(r[kRspHeader], 1);
    CHECK_EQ(run(f, request(0x09, 7, {0}), r), 1);
    CHECK_EQ(r[kRspHeader], 0);
    CHECK_EQ(f.log_on, 0);
}

static void test_build_response_pads_and_cuts() {
    Frame r; r.fill(0xEE);
    std::array<uint8_t, 70> big{};
    big.fill(0x55);
    size_t n = build_response(r, 1, 2, Status::Ok, big);
    CHECK_EQ(n, kFrameSize);                                     // 58 payload bytes survive
    CHECK_EQ(r[5], 58);
    CHECK_EQ(r[63], 0x55);
    n = build_response(r, 1, 2, Status::Ok);
    CHECK_EQ(n, kRspHeader);
    check_header(r, 1, 2, Status::Ok, 0);
}

static void test_screenshot() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x0A, 5, {}), r), 1);
    check_header(r, 0x0A, 5, Status::Ok, 7);
    const uint8_t want[7] = {240, 0, 240, 0, 4, 60, 1};
    CHECK_EQ(memcmp(&r[6], want, 7), 0);
    CHECK_EQ(f.shots, 1);
    f.shot_status = Status::Busy;
    CHECK_EQ(run(f, request(0x0A, 6, {}), r), 1);
    check_header(r, 0x0A, 6, Status::Busy, 0);
    CHECK_EQ(run(f, request(0x0A, 7, {1}), r), 1);
    check_header(r, 0x0A, 7, Status::BadLength, 0);
}

static void test_debug_pd_msg() {
    Fake f; Frame r;
    // SOP', header 0x4F | PD 3.0 | 1 object (0x104F), one VDO
    CHECK_EQ(run(f, request(0x23, 1, {1, 0x4F, 0x10, 0x01, 0x80, 0x00, 0xFF}), r), 1);
    check_header(r, 0x23, 1, Status::Ok, 0);
    CHECK_EQ(f.msg_sop, 1);
    CHECK_EQ(f.msg_header, 0x104F);
    CHECK_EQ(f.msg_len, 4);
    CHECK_EQ(f.msg_body[2], 0x00);
    CHECK_EQ(f.msg_body[3], 0xFF);
    // control message, no objects
    CHECK_EQ(run(f, request(0x23, 2, {0, 0x41, 0x00}), r), 1);
    check_header(r, 0x23, 2, Status::Ok, 0);
    CHECK_EQ(f.msg_len, 0);
    // body length must match the header's object count
    CHECK_EQ(run(f, request(0x23, 3, {0, 0x4F, 0x10, 1, 2, 3}), r), 1);
    check_header(r, 0x23, 3, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x23, 4, {0, 0x4F}), r), 1);
    check_header(r, 0x23, 4, Status::BadLength, 0);
    // unknown SOP kind
    CHECK_EQ(run(f, request(0x23, 5, {3, 0x41, 0x00}), r), 1);
    check_header(r, 0x23, 5, Status::BadArg, 0);
    f.msg_status = Status::Busy;
    CHECK_EQ(run(f, request(0x23, 6, {0, 0x41, 0x00}), r), 1);
    check_header(r, 0x23, 6, Status::Busy, 0);
}

static void test_debug_link() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x25, 3, {0}), r), 1);
    check_header(r, 0x25, 3, Status::Ok, 1);
    CHECK_EQ(f.link_arg, 0);
    CHECK_EQ(run(f, request(0x25, 4, {1}), r), 1);
    CHECK_EQ(f.link_arg, 1);
    CHECK_EQ(run(f, request(0x25, 5, {2}), r), 1);
    check_header(r, 0x25, 5, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x25, 6, {}), r), 1);
    check_header(r, 0x25, 6, Status::BadLength, 0);
}

static void test_debug_epr_info() {
    Fake f; Frame r;
    f.epr_info_snap.ok = 1; f.epr_info_snap.err = 0; f.epr_info_snap.state = 2; f.epr_info_snap.num_pdos = 9;
    f.epr_info_snap.pdos[7] = 0xA0000000u; f.epr_info_snap.pdos[8] = 0xCAFEF00Du;
    CHECK_EQ(run(f, request(0x28, 1, {2}), r), 1);
    check_header(r, 0x28, 1, Status::Ok, 4 + 9 * 4);
    CHECK_EQ(f.epr_info_op, 2);
    check_bytes(r, kRspHeader, {1, 0, 2, 9});
    check_bytes(r, kRspHeader + 4 + 7 * 4, {0x00, 0x00, 0x00, 0xA0, 0x0D, 0xF0, 0xFE, 0xCA});
    // no list: the header only, whatever the PDO array holds
    f.epr_info_snap.num_pdos = 0; f.epr_info_snap.state = 4; f.epr_info_snap.err = 6;
    CHECK_EQ(run(f, request(0x28, 2, {0}), r), 1);
    check_header(r, 0x28, 2, Status::Ok, 4);
    check_bytes(r, kRspHeader, {1, 6, 4, 0});
    CHECK_EQ(run(f, request(0x28, 3, {3}), r), 1);
    check_header(r, 0x28, 3, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x28, 4, {}), r), 1);
    check_header(r, 0x28, 4, Status::BadLength, 0);
}

static void test_debug_lcd() {
    Fake f; Frame r;
    f.lcd_snap.flags = 3; f.lcd_snap.transfers = 0x01020304; f.lcd_snap.stalls = 7;
    CHECK_EQ(run(f, request(0x29, 1, {}), r), 1);
    check_header(r, 0x29, 1, Status::Ok, 9);
    check_bytes(r, kRspHeader, {3, 0x04, 0x03, 0x02, 0x01, 7, 0, 0, 0});
    CHECK_EQ(run(f, request(0x29, 2, {1}), r), 1);
    check_header(r, 0x29, 2, Status::BadLength, 0);
}

static void test_debug_cc_probe() {
    Fake f; Frame r;
    // The experiment: sw0 0x28 (VCONN_CC2 + MEAS_CC2), PC13 low (pc 0), 500 ms, modes kept: a 4 byte payload.
    f.probe_res.flags = 2; f.probe_res.n = 2; f.probe_res.sw0 = 0x00; f.probe_res.sw1 = 0x20; f.probe_res.pc = 6; f.probe_res.modes = 0x333;
    f.probe_res.s[0] = {1234, 0x0102, 0x0304, 0x0506, 0x21, 0x22};
    f.probe_res.s[1] = {500001, 0xFFFF, 0x0010, 0xFFFF, 0x00, 0x03};
    CHECK_EQ(run(f, request(0x2A, 1, {0x28, 0, 0xF4, 0x01}), r), 1);
    check_header(r, 0x2A, 1, Status::Ok, 7 + 2 * 12);
    check_bytes(r, kRspHeader, {2, 2, 0x00, 0x20, 6, 0x33, 0x03});
    check_bytes(r, kRspHeader + 7, {0xD2, 0x04, 0, 0, 0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0x21, 0x22});
    check_bytes(r, kRspHeader + 19, {0x21, 0xA1, 0x07, 0x00, 0xFF, 0xFF, 0x10, 0x00, 0xFF, 0xFF, 0x00, 0x03});
    CHECK_EQ(f.probe_sw0, 0x28); CHECK_EQ(f.probe_pc, 0); CHECK_EQ(f.probe_ms, 500); CHECK_EQ(f.probe_modes, 0xFFFF);
    // A 6 byte payload carries the pin modes.
    CHECK_EQ(run(f, request(0x2A, 2, {0x08, 5, 0x0A, 0x00, 0x88, 0x03}), r), 1);
    check_header(r, 0x2A, 2, Status::Ok, 7 + 2 * 12);
    CHECK_EQ(f.probe_sw0, 0x08); CHECK_EQ(f.probe_pc, 5); CHECK_EQ(f.probe_ms, 10); CHECK_EQ(f.probe_modes, 0x388);
    // Four samples fit one frame.
    f.probe_res.n = 4;
    CHECK_EQ(run(f, request(0x2A, 3, {0x28, 0, 0xF4, 0x01}), r), 1);
    check_header(r, 0x2A, 3, Status::Ok, 7 + 4 * 12);
    f.probe_res.n = 2;
    // The pure rules refuse before the backend is called.
    const int calls = f.calls;
    CHECK_EQ(run(f, request(0x2A, 4, {0x20, 1, 0xF4, 0x01}), r), 1);      // VCONN_CC2 with PC13 high
    check_header(r, 0x2A, 4, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x2A, 5, {0x22, 0, 0xF4, 0x01}), r), 1);      // VCONN_CC2 + PDWN2
    check_header(r, 0x2A, 5, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x2A, 6, {0x28, 0, 0x09, 0x00}), r), 1);      // 9 ms
    check_header(r, 0x2A, 6, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x2A, 7, {0x28, 8, 0xF4, 0x01}), r), 1);      // PC level above 7
    check_header(r, 0x2A, 7, Status::BadArg, 0);
    CHECK_EQ(f.calls, calls);
    CHECK_EQ(run(f, request(0x2A, 8, {0x28, 0, 0xF4}), r), 1);            // length 3 or 5 or 0
    check_header(r, 0x2A, 8, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x2A, 9, {0x28, 0, 0xF4, 0x01, 0x33}), r), 1);
    check_header(r, 0x2A, 9, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x2A, 10, {}), r), 1);
    check_header(r, 0x2A, 10, Status::BadLength, 0);
    // The backend's refusal comes back as its status, with no payload.
    f.probe_status = Status::Busy;
    CHECK_EQ(run(f, request(0x2A, 11, {0x28, 0, 0xF4, 0x01}), r), 1);
    check_header(r, 0x2A, 11, Status::Busy, 0);
}

static void test_debug_fake_pdos() {
    Fake f; Frame r;
    CHECK_EQ(run(f, request(0x20, 3, {7}), r), 1);
    check_header(r, 0x20, 3, Status::Ok, 1);
    CHECK_EQ(r[6], 7);
    CHECK_EQ(f.fake_n, 7);
    CHECK_EQ(run(f, request(0x20, 4, {0}), r), 1);          // 0 clears the override
    check_header(r, 0x20, 4, Status::Ok, 1);
    CHECK_EQ(f.fake_n, 0);
    f.fake_n = -1;
    CHECK_EQ(run(f, request(0x20, 5, {12}), r), 1);         // more than MAX_PDOS
    check_header(r, 0x20, 5, Status::BadArg, 0);
    CHECK_EQ(f.fake_n, -1);
    CHECK_EQ(run(f, request(0x20, 6, {}), r), 1);
    check_header(r, 0x20, 6, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x20, 7, {1, 2}), r), 1);
    check_header(r, 0x20, 7, Status::BadLength, 0);
    f.fake_status = Status::Busy;
    CHECK_EQ(run(f, request(0x20, 8, {2}), r), 1);
    check_header(r, 0x20, 8, Status::Busy, 0);
}

static void test_stream_frame() {
    Frame r; r.fill(0xEE);
    const uint8_t rle[3] = {0x01, 0x34, 0x12};
    size_t n = build_stream_frame(r, 9, 4, kShotFlagLast, rle);
    CHECK_EQ(n, 11);
    const uint8_t want[11] = {0xA6, 0x4B, 0x8A, 9, 0, 5, 4, 1, 0x01, 0x34, 0x12};
    CHECK_EQ(memcmp(r.data(), want, 11), 0);
    CHECK_EQ(r[11], 0);                      // zero padded
    // Oversize chunks are cut to kShotChunk (56) bytes: the frame never exceeds 64 bytes.
    std::array<uint8_t, 80> big{};
    n = build_stream_frame(r, 1, 0, 0, big);
    CHECK_EQ(n, 64);
    CHECK_EQ(r[5], 58);
}

static void test_active_pd_commands() {
    Fake f; Frame r;
    // PD_REQUEST: [pos, flags, mv u16, ma u16] -> [err]
    CHECK_EQ(run(f, request(0x41, 1, {4, 1, 0x20, 0x4E, 0xB8, 0x0B}), r), 1);
    check_header(r, 0x41, 1, Status::Ok, 1);
    check_bytes(r, kRspHeader, {0});
    CHECK_EQ(f.rq_pos, 4); CHECK_EQ(f.rq_mv, 0x4E20); CHECK_EQ(f.rq_ma, 0x0BB8); CHECK_EQ(f.rq_confirm, true);
    CHECK_EQ(run(f, request(0x41, 2, {2, 0, 0, 0, 0, 0}), r), 1);
    CHECK_EQ(f.rq_confirm, false);                       // flag bit 0 only
    CHECK_EQ(f.rq_over_load, false);
    CHECK_EQ(run(f, request(0x41, 6, {6, 3, 0x7C, 0x15, 0xD0, 0x07}), r), 1);
    CHECK_EQ(f.rq_pos, 6); CHECK_EQ(f.rq_confirm, true); CHECK_EQ(f.rq_over_load, true);   // bits 0 + 1
    CHECK_EQ(run(f, request(0x41, 7, {6, 2, 0x7C, 0x15, 0xD0, 0x07}), r), 1);
    CHECK_EQ(f.rq_confirm, false); CHECK_EQ(f.rq_over_load, true);   // over-load alone: bit 1 only
    f.rq_result = {Status::NeedConfirm, 0};
    run(f, request(0x41, 3, {2, 0, 0, 0, 0, 0}), r);
    check_header(r, 0x41, 3, Status::NeedConfirm, 1);
    f.rq_result = {Status::BadArg, 3};
    run(f, request(0x41, 4, {9, 1, 0, 0, 0, 0}), r);
    check_header(r, 0x41, 4, Status::BadArg, 1);
    check_bytes(r, kRspHeader, {3});
    run(f, request(0x41, 5, {1, 0}), r);                 // short payload
    check_header(r, 0x41, 5, Status::BadLength, 0);
    // PD_RESET / PD_RELEASE take no payload
    run(f, request(0x42, 6, {}), r);
    check_header(r, 0x42, 6, Status::Ok, 1);
    run(f, request(0x44, 7, {}), r);
    check_header(r, 0x44, 7, Status::Ok, 1);
    CHECK_EQ(f.resets, 1); CHECK_EQ(f.releases, 1);
    run(f, request(0x42, 8, {1}), r);
    check_header(r, 0x42, 8, Status::BadLength, 0);
    CHECK_EQ(f.resets, 1);
    // PD_TRIGGER_STATUS
    run(f, request(0x43, 9, {}), r);
    check_header(r, 0x43, 9, Status::Ok, 13 + 4 + 1);   // [..., hard_resets u32], then appended source_hard_resets u32 and pin u8 (older clients read the first 13/17 bytes)
    check_bytes(r, kRspHeader, {5, 1, 1, 0, 4, 0x20, 0x4E, 0xB8, 0x0B, 4, 3, 2, 1, 8, 7, 6, 5});
    // USB_LINK
    run(f, request(0x46, 11, {}), r);
    check_header(r, 0x46, 11, Status::Ok, 9 + 19);
    check_bytes(r, kRspHeader, {5, 2, 3, 4, 1, 0x40, 0x9C, 0, 0, 'U', 'S', 'B', '4', ' ', '4', '0', 'G', ' ', 'T', 'B', 'T', '4', ' ', '(', 'n', 'e', 'g', ')'});
    run(f, request(0x46, 14, {1}), r);
    check_header(r, 0x46, 14, Status::BadLength, 0);
    // PD_MATCH
    run(f, request(0x45, 12, {}), r);
    check_header(r, 0x45, 12, Status::Ok, 21);
    check_bytes(r, kRspHeader, {1, 1, 0, 2, 3, 2, 2, 0xE0, 0x2E, 0xB8, 0x0B, 0xB8, 0x0B, 0x84, 0x03, 0x98, 0x3A, 0x64, 0x00, 3, 1});
    run(f, request(0x45, 13, {1}), r);
    check_header(r, 0x45, 13, Status::BadLength, 0);
    // IDENTIFY_DEVICE: [op] -> progress
    f.id_result.progress.state = 2; f.id_result.progress.step = 6; f.id_result.progress.ok_mask = 0x0B;
    f.id_result.progress.num_sink_pdos = 1; f.id_result.progress.revision = 0x31200000;
    f.id_result.progress.sink_pdos[0] = 0x0001912C;
    run(f, request(0x40, 10, {1}), r);
    check_header(r, 0x40, 10, Status::Ok, 14);
    CHECK_EQ(f.id_op, 1);
    check_bytes(r, kRspHeader, {2, 6, 0x0B, 0, 0, 1, 0x00, 0x00, 0x20, 0x31, 0x2C, 0x91, 0x01, 0x00});
    f.id_result.status = Status::NeedConfirm;
    run(f, request(0x40, 11, {1}), r);
    check_header(r, 0x40, 11, Status::NeedConfirm, 0);
    run(f, request(0x40, 12, {3}), r);
    check_header(r, 0x40, 12, Status::BadArg, 0);
    run(f, request(0x40, 13, {}), r);
    check_header(r, 0x40, 13, Status::BadLength, 0);
    // PD_REREAD: [op (0 status, 1 start + mask, 2 abort), mask] -> state step mask ok skipped pdos_before pdos_last caps_rx flags
    f.rr_result.info = {2, 5, 15, 8, 3, 6, 7, 1, 1};
    run(f, request(0x47, 20, {1, 15}), r);
    check_header(r, 0x47, 20, Status::Ok, 9);
    check_bytes(r, kRspHeader, {2, 5, 15, 8, 3, 6, 7, 1, 1});
    CHECK_EQ(f.rr_op, 1); CHECK_EQ(f.rr_mask, 15);
    run(f, request(0x47, 21, {0}), r);
    check_header(r, 0x47, 21, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 0);
    run(f, request(0x47, 22, {2}), r);
    check_header(r, 0x47, 22, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 2);
    f.rr_result.status = Status::Busy;   // refused: not in PD mode, no CC, inline or the sink is busy
    run(f, request(0x47, 23, {1, 8}), r);
    check_header(r, 0x47, 23, Status::Busy, 0);
    f.rr_result.status = Status::Ok;
    f.rr_calls = 0;
    run(f, request(0x47, 24, {1}), r);           // start without a mask
    check_header(r, 0x47, 24, Status::BadLength, 0);
    run(f, request(0x47, 25, {1, 0}), r);        // nothing to do
    check_header(r, 0x47, 25, Status::BadArg, 0);
    run(f, request(0x47, 26, {1, 32}), r);       // only five steps
    check_header(r, 0x47, 26, Status::BadArg, 0);
    run(f, request(0x47, 27, {9}), r);           // unknown op
    check_header(r, 0x47, 27, Status::BadArg, 0);
    // op 8 [8, ms/10] (lab): the Rd off for that many 10 ms steps (Ra kept), then back: the source sees a detach and a fresh attach
    run(f, request(0x47, 70, {8, 3}), r);
    check_header(r, 0x47, 70, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 8); CHECK_EQ(f.rr_mask, 3);
    run(f, request(0x47, 71, {8, 255}), r);
    check_header(r, 0x47, 71, Status::Ok, 9);
    CHECK_EQ(f.rr_mask, 255);
    run(f, request(0x47, 72, {8, 0}), r);        // no gap
    check_header(r, 0x47, 72, Status::BadArg, 0);
    run(f, request(0x47, 73, {8}), r);           // the gap is required
    check_header(r, 0x47, 73, Status::BadLength, 0);
    run(f, request(0x47, 74, {8, 1, 1}), r);
    check_header(r, 0x47, 74, Status::BadLength, 0);
    // op 6 [6, line] (lab): pin the Rd on CC1 / CC2 as a key-mode boot does, 0 releases; op 7 (lab): a Soft_Reset with the Rd kept
    run(f, request(0x47, 60, {6, 2}), r);
    check_header(r, 0x47, 60, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 6); CHECK_EQ(f.rr_mask, 2);
    run(f, request(0x47, 61, {6, 0}), r);
    check_header(r, 0x47, 61, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 6); CHECK_EQ(f.rr_mask, 0);
    run(f, request(0x47, 62, {6, 3}), r);        // only two lines
    check_header(r, 0x47, 62, Status::BadArg, 0);
    run(f, request(0x47, 63, {6}), r);           // the line is required
    check_header(r, 0x47, 63, Status::BadLength, 0);
    run(f, request(0x47, 64, {7}), r);
    check_header(r, 0x47, 64, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 7);
    run(f, request(0x47, 65, {7, 1}), r);        // no argument
    check_header(r, 0x47, 65, Status::BadLength, 0);
    // op 5 [5, on]: the Ra mode (PC13 high, Rd on CC2 alone); on is 0 or 1
    run(f, request(0x47, 50, {5, 1}), r);
    check_header(r, 0x47, 50, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 5); CHECK_EQ(f.rr_mask, 1);
    run(f, request(0x47, 51, {5, 0}), r);
    check_header(r, 0x47, 51, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 5); CHECK_EQ(f.rr_mask, 0);
    f.rr_result.status = Status::Busy;   // refused: sink inline, contract running, ...
    run(f, request(0x47, 52, {5, 1}), r);
    check_header(r, 0x47, 52, Status::Busy, 0);
    f.rr_result.status = Status::Ok;
    f.rr_calls = 0;
    run(f, request(0x47, 53, {5, 2}), r);        // on is a flag
    check_header(r, 0x47, 53, Status::BadArg, 0);
    run(f, request(0x47, 54, {5}), r);           // the flag is required
    check_header(r, 0x47, 54, Status::BadLength, 0);
    CHECK_EQ(f.rr_calls, 0);
    // op 4 [4, line]: the next run's attach scan may take only that CC line (0 = normal, 1, 2)
    run(f, request(0x47, 40, {4, 1}), r);
    check_header(r, 0x47, 40, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 4); CHECK_EQ(f.rr_mask, 1);
    run(f, request(0x47, 41, {4, 0}), r);
    check_header(r, 0x47, 41, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 4); CHECK_EQ(f.rr_mask, 0);
    f.rr_calls = 0;
    run(f, request(0x47, 42, {4, 3}), r);        // only two lines
    check_header(r, 0x47, 42, Status::BadArg, 0);
    run(f, request(0x47, 43, {4}), r);           // the line is required
    check_header(r, 0x47, 43, Status::BadLength, 0);
    CHECK_EQ(f.rr_calls, 0);
    // op 3 [3, mode]: the Get_Sink_Cap_Extended answer (0 silent, 1 Sink_Capabilities_Extended, 2 Not_Supported)
    f.rr_result.status = Status::Ok;
    run(f, request(0x47, 30, {3, 1}), r);
    check_header(r, 0x47, 30, Status::Ok, 9);
    CHECK_EQ(f.rr_op, 3); CHECK_EQ(f.rr_mask, 1);
    f.rr_calls = 0;
    run(f, request(0x47, 31, {3, 3}), r);        // only three modes
    check_header(r, 0x47, 31, Status::BadArg, 0);
    run(f, request(0x47, 32, {3}), r);           // the mode is required
    check_header(r, 0x47, 32, Status::BadLength, 0);
    run(f, request(0x47, 28, {0, 1}), r);        // status takes no mask
    check_header(r, 0x47, 28, Status::BadLength, 0);
    run(f, request(0x47, 29, {}), r);
    check_header(r, 0x47, 29, Status::BadLength, 0);
    CHECK_EQ(f.rr_calls, 0);                     // the backend is never reached by a bad request
}

static void test_store() {
    Fake f; Frame r;
    // list, save-emarker, save-log: bare op byte
    for (uint8_t op : {0, 1, 3}) {
        f.store_calls = 0;
        CHECK_EQ(run(f, request(0x60, 1, {op}), r), 1);
        check_header(r, 0x60, 1, Status::Ok, 8);
        check_bytes(r, kRspHeader, {0, 3, 5, 2, 0x02, 0x01, 2, 4});
        CHECK_EQ(f.store_calls, 1); CHECK_EQ(f.store_op, op);
    }
    // op 2 (erase of the native pages) is retired: BadArg, the backend is never called
    f.store_calls = 0;
    CHECK_EQ(run(f, request(0x60, 2, {2, 4, 1}), r), 1);
    check_header(r, 0x60, 2, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x60, 3, {2}), r), 1);
    check_header(r, 0x60, 3, Status::BadArg, 0);
    CHECK_EQ(f.store_calls, 0);
    // unknown op, bare list with a stray byte, empty payload
    CHECK_EQ(run(f, request(0x60, 9, {4}), r), 1);
    check_header(r, 0x60, 9, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x60, 10, {0, 0}), r), 1);
    check_header(r, 0x60, 10, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x60, 11, {}), r), 1);
    check_header(r, 0x60, 11, Status::BadLength, 0);
    CHECK_EQ(f.store_calls, 0);
}

static void test_charger_test_wire() {
    namespace ct = k2::charger_test;
    Fake f; Frame r;
    std::array<ct::Step, 3> steps{};
    steps[0] = {.group = ct::Group::Qc3, .algorithm = ct::Algorithm::Qc3, .mv = 20000, .measured_mv = 19800,
                .verdict = ct::Verdict::Pass, .continuous = true};
    steps[1] = {.algorithm = ct::Algorithm::Pps, .pos = 6, .mv = 9000, .ma = 1000, .verdict = ct::Verdict::Running,
                .continuous = true, .pdo = 0xC0DC3240};
    steps[2] = {.group = ct::Group::Afc, .algorithm = ct::Algorithm::Afc, .mv = 12000, .verdict = ct::Verdict::Fail,
                .reason = ct::Reason::Request, .detail = 7};
    f.test_results = {0x01020304, 0x05060708, 3, 0, 1, steps};
    CHECK_EQ(run(f, request(0x0B, 9, {}), r), 1);
    check_header(r, 0x0B, 9, Status::Ok, 15);
    check_bytes(r, 6, {1, 4, 3, 2, 1, 8, 7, 6, 5, 3, 0, 3, 1, 0, 0});
    CHECK_EQ(run(f, request(0x0B, 9, {0, 4, 3, 2, 1, 8, 7, 6, 5}), r), 1);
    check_header(r, 0x0B, 9, Status::Ok, 47);
    check_bytes(r, 6, {1, 4, 3, 2, 1, 8, 7, 6, 5, 3, 0, 3, 1, 0, 2,
                      2, 0, 1, 0x20, 0x4E, 0, 0, 0x58, 0x4D, 2, 0, 0, 0, 0, 0, 0,
                      0, 6, 1, 0x28, 0x23, 0xE8, 3, 0, 0, 1, 0, 0, 0x40, 0x32, 0xDC, 0xC0});
    CHECK_EQ(run(f, request(0x0B, 10, {2, 4, 3, 2, 1, 8, 7, 6, 5}), r), 1);
    check_header(r, 0x0B, 10, Status::Ok, 31);
    check_bytes(r, 19, {2, 1, 4, 0, 0, 0xE0, 0x2E, 0, 0, 0, 0, 3, 11, 7, 0, 0, 0, 0});
    CHECK_EQ(run(f, request(0x0B, 11, {3, 4, 3, 2, 1, 8, 7, 6, 5}), r), 1);
    check_header(r, 0x0B, 11, Status::Ok, 15);
    CHECK_EQ(r[19], 3); CHECK_EQ(r[20], 0);
    CHECK_EQ(run(f, request(0x0B, 12, {4, 4, 3, 2, 1, 8, 7, 6, 5}), r), 1);
    check_header(r, 0x0B, 12, Status::BadArg, 0);
    for (auto payload : {std::initializer_list<uint8_t>{}, {0, 0, 0, 0, 0, 8, 7, 6, 5},
                         {0, 4, 3, 2, 1, 9, 7, 6, 5}}) {
        if (payload.size() == 0) continue;
        CHECK_EQ(run(f, request(0x0B, 13, payload), r), 1);
        check_header(r, 0x0B, 13, Status::Busy, 0);
    }
    const int calls = f.calls;
    for (unsigned n = 1; n <= kMaxReqPayload; ++n) {
        if (n == 1 || n == 9 || n == 10) continue;
        auto req = request(0x0B, 14, {}); req[4] = n;
        CHECK_EQ(run(f, req, r), 1);
        check_header(r, 0x0B, 14, Status::BadLength, 0);
    }
    CHECK_EQ(f.calls, calls);
    CHECK_EQ(f.fc_calls + f.releases + f.resets + f.store_calls + f.reboots + f.shots, 0);
    f.test_results = {};
    CHECK_EQ(run(f, request(0x0B, 15, {}), r), 1);
    check_header(r, 0x0B, 15, Status::Ok, 15);
    check_bytes(r, 6, {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    std::array<uint8_t, 30> small{};
    f.test_results.steps = steps;
    CHECK_EQ(encode_charger_test(small, f.test_results, 0, true), 15);
    CHECK_EQ(small[14], 0); // never emit a partial row

    f.test_results = {0x01020304, 0x05060708, 3, 0, 1, steps};
    f.test_results.evidence = {.selected = ct::kAllAlgorithms, .session = 0x11223344,
        .monitor_mv = 36000, .cable_mv = 50000, .cable_ma = 5000, .raw_header = 0x7181,
        .wire_rev = 2, .flags = 255, .num_pdos = 7,
        .reported = {.valid = true, .rev_major = 3, .rev_minor = 2, .ver_major = 1, .ver_minor = 7}};
    f.test_results.configured = ct::bit(ct::Algorithm::Pps) | ct::bit(ct::Algorithm::Pd32Revision);
    f.test_results.stage = 4;
    f.test_results.cursor = 9;     // high nibble of the PDO-count byte
    CHECK_EQ(run(f, request(0x0B, 16, {2}), r), 1);
    check_header(r, 0x0B, 16, Status::Ok, 40);
    check_bytes(r, 6, {2, 4, 3, 2, 1, 8, 7, 6, 5, 3, 0, 3, 1, 0, 0,
                      0xFF, 0xFF, 63, 0, 0x10, 0, 4, 0, 0x12, 0x32, 0xFF,
                      0xA0, 0x8C, 0x50, 0xC3, 0x88, 0x13, 0x17, 0x44, 0x33, 0x22, 0x11, 0x81, 0x71, 0x97});
    for (unsigned i = 0; i < steps.size(); ++i) {
        CHECK_EQ(run(f, request(0x0B, 17, {2, uint8_t(i), 4, 3, 2, 1, 8, 7, 6, 5}), r), 1);
        check_header(r, 0x0B, 17, Status::Ok, 56);
        CHECK_EQ(r[19], i); CHECK_EQ(r[20], 1);
        CHECK_EQ(r[46], static_cast<uint8_t>(steps[i].algorithm));
    }
    for (auto p : {std::initializer_list<uint8_t>{0}, {1}, {3}, {3, 0, 4, 3, 2, 1, 8, 7, 6, 5},
                   {2, 4, 4, 3, 2, 1, 8, 7, 6, 5}}) {
        CHECK_EQ(run(f, request(0x0B, 18, p), r), 1);
        check_header(r, 0x0B, 18, Status::BadArg, 0);
    }
    CHECK_EQ(run(f, request(0x0B, 19, {2, 0, 5, 3, 2, 1, 8, 7, 6, 5}), r), 1);
    check_header(r, 0x0B, 19, Status::Busy, 0);
    std::array<uint8_t, 55> short2{};
    CHECK_EQ(encode_charger_test(short2, f.test_results, 0, true, 2), 40);
    CHECK_EQ(short2[14], 0); CHECK_EQ(encode_charger_test(short2, f.test_results, 0, true, 3), 0);

    const ct::Step revision{.algorithm = ct::Algorithm::Pd30Revision,
        .verdict = ct::Verdict::Skip, .reason = ct::Reason::RevisionUntestable};
    const ct::Step ufcs{.group = ct::Group::Ufcs, .algorithm = ct::Algorithm::Ufcs,
        .verdict = ct::Verdict::Skip, .reason = ct::Reason::NotImplemented};
    const ct::Step cable{.group = ct::Group::Scp, .algorithm = ct::Algorithm::Scp, .mv = 5000, .ma = 3500,
        .verdict = ct::Verdict::Skip, .reason = ct::Reason::CableLimit};
    const std::array projection{steps[0], revision, steps[1], ufcs, cable};
    f.test_results.steps = projection; f.test_results.current = 2; f.test_results.evidence.cable_ma = 3000;
    CHECK_EQ(charger_test_count(f.test_results, 1), 3); CHECK_EQ(charger_test_count(f.test_results, 2), 5);
    CHECK_EQ(run(f, request(0x0B, 20, {}), r), 1);
    CHECK_EQ(r[17], 3); CHECK_EQ(r[18], 1);
    CHECK_EQ(run(f, request(0x0B, 21, {1, 4, 3, 2, 1, 8, 7, 6, 5}), r), 1);
    check_header(r, 0x0B, 21, Status::Ok, 47);
    CHECK_EQ(r[21], 0); CHECK_EQ(r[37], 6); CHECK_EQ(r[47], static_cast<uint8_t>(ct::Reason::NoOffer));
    CHECK_EQ(run(f, request(0x0B, 22, {2, 1, 4, 3, 2, 1, 8, 7, 6, 5}), r), 1);
    CHECK_EQ(r[17], 5); CHECK_EQ(r[18], 2); CHECK_EQ(r[46], static_cast<uint8_t>(ct::Algorithm::Pd30Revision));
    CHECK_EQ(r[55], static_cast<uint8_t>(ct::Verdict::Skip));
    CHECK_EQ(r[56], static_cast<uint8_t>(ct::Reason::RevisionUntestable));
    CHECK_EQ(f.fc_calls + f.releases + f.resets + f.store_calls + f.reboots + f.shots, 0);
}

static void test_charger_units() {
    namespace ct = k2::charger_test;
    Fake f; Frame r;
    // [n, unit...]: the order is passed through, the answer is [n]
    CHECK_EQ(run(f, request(0x0C, 1, {2, 1, 2}), r), 1);
    check_header(r, 0x0C, 1, Status::Ok, 1); check_bytes(r, kRspHeader, {2});
    CHECK_EQ(f.units_set.size(), 2u); CHECK_EQ(f.units_set[0], 1); CHECK_EQ(f.units_set[1], 2);
    // n = 0 clears the list (the default order)
    CHECK_EQ(run(f, request(0x0C, 2, {0}), r), 1);
    check_header(r, 0x0C, 2, Status::Ok, 1); CHECK_EQ(f.units_set.size(), 0u);
    // the full list is accepted
    CHECK_EQ(unsigned(ct::Unit::Count), 13u);
    CHECK_EQ(run(f, request(0x0C, 3, {13, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0}), r), 1);
    check_header(r, 0x0C, 3, Status::Ok, 1); CHECK_EQ(f.units_set.size(), 13u);
    const int calls = f.calls;
    // length must match the count; an unknown or repeated unit is a bad argument; nothing reaches the backend
    CHECK_EQ(run(f, request(0x0C, 4, {}), r), 1); check_header(r, 0x0C, 4, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x0C, 5, {2, 1}), r), 1); check_header(r, 0x0C, 5, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x0C, 6, {1, 13}), r), 1); check_header(r, 0x0C, 6, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x0C, 7, {2, 1, 1}), r), 1); check_header(r, 0x0C, 7, Status::BadArg, 0);
    CHECK_EQ(f.calls, calls);
    // a run is active: the backend answers Busy and the reply carries it
    f.units_status = Status::Busy;
    CHECK_EQ(run(f, request(0x0C, 8, {1, 1}), r), 1); check_header(r, 0x0C, 8, Status::Busy, 0);
}

static void test_charger_cable() {
    namespace cc = k2::ctcable;
    Fake f; Frame r;
    f.cable_info.choice = {cc::Kind::Saved, 0x0102};
    f.cable_info.run = {cc::Kind::Builtin, 0};
    std::snprintf(f.cable_info.run_name.data(), f.cable_info.run_name.size(), "EPR 50V 5A");
    // no payload reads: choice(3), run(3), name(24) and nothing reaches the setter
    CHECK_EQ(run(f, request(0x0D, 1, {}), r), 1);
    check_header(r, 0x0D, 1, Status::Ok, 30);
    check_bytes(r, kRspHeader, {2, 0x02, 0x01, 1, 0, 0, 'E', 'P', 'R', ' ', '5', '0', 'V', ' ', '5', 'A', 0});
    CHECK_EQ(r[kRspHeader + 29], 0);
    CHECK_EQ(f.cable_sets, 0);
    // [kind, seq(2)] sets the choice (little endian) and answers the same state
    CHECK_EQ(run(f, request(0x0D, 2, {2, 0x34, 0x12}), r), 1);
    check_header(r, 0x0D, 2, Status::Ok, 30);
    CHECK_EQ(f.cable_sets, 1); CHECK_EQ(f.cable_set.kind == cc::Kind::Saved, true); CHECK_EQ(f.cable_set.seq, 0x1234);
    CHECK_EQ(run(f, request(0x0D, 3, {1, 0, 0}), r), 1);
    check_header(r, 0x0D, 3, Status::Ok, 30);
    CHECK_EQ(f.cable_sets, 2); CHECK_EQ(f.cable_set.kind == cc::Kind::Builtin, true);
    CHECK_EQ(run(f, request(0x0D, 4, {0, 0, 0}), r), 1);
    CHECK_EQ(f.cable_sets, 3); CHECK_EQ(f.cable_set == cc::Choice{}, true);
    // the built-in id rides in seq: every id of the table is accepted (3 = the retired OnePlus mag id, kept as an alias of 4), the first past it is not
    CHECK_EQ(run(f, request(0x0D, 20, {1, 3, 0}), r), 1); check_header(r, 0x0D, 20, Status::Ok, 30);
    CHECK_EQ(f.cable_sets, 4); CHECK_EQ(f.cable_set.seq, 3);
    CHECK_EQ(run(f, request(0x0D, 21, {1, 4, 0}), r), 1); check_header(r, 0x0D, 21, Status::Ok, 30);
    CHECK_EQ(f.cable_sets, 5); CHECK_EQ(f.cable_set.seq, 4);
    CHECK_EQ(run(f, request(0x0D, 22, {1, 0, 0}), r), 1); check_header(r, 0x0D, 22, Status::Ok, 30);
    CHECK_EQ(f.cable_sets, 6); CHECK_EQ(f.cable_set.seq, 0);                     // 0 stays the default built-in (older clients)
    // malformed requests never reach the backend
    const int calls = f.calls;
    CHECK_EQ(run(f, request(0x0D, 5, {1}), r), 1); check_header(r, 0x0D, 5, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x0D, 6, {1, 0, 0, 0}), r), 1); check_header(r, 0x0D, 6, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x0D, 7, {3, 0, 0}), r), 1); check_header(r, 0x0D, 7, Status::BadArg, 0);   // unknown kind
    CHECK_EQ(run(f, request(0x0D, 8, {2, 0, 0}), r), 1); check_header(r, 0x0D, 8, Status::BadArg, 0);   // a clone needs its id
    CHECK_EQ(run(f, request(0x0D, 9, {1, 5, 0}), r), 1); check_header(r, 0x0D, 9, Status::BadArg, 0);   // the built-in carries none
    CHECK_EQ(f.calls, calls);
    // a run is active or the clone is gone: the backend's answer is the reply
    f.cable_status = Status::Busy;
    CHECK_EQ(run(f, request(0x0D, 10, {1, 0, 0}), r), 1); check_header(r, 0x0D, 10, Status::Busy, 0);
    f.cable_status = Status::BadArg;
    CHECK_EQ(run(f, request(0x0D, 11, {2, 9, 0}), r), 1); check_header(r, 0x0D, 11, Status::BadArg, 0);
}

static void test_charger_hunt() {
    namespace ct = k2::charger_test;
    Fake f; Frame r;
    f.hunt_reply.run_id = 0x01020304;
    f.hunt_reply.count = 2;
    auto& e = f.hunt_reply.slot;
    e.step = 7; e.cause = uint8_t(ct::HuntCause::RefusedDespiteOffer); e.answer = 2; e.last_step = 0x0A; e.caps = 5; e.pos = 3; e.type = 3;
    e.flags = 0x01CF; e.apdo_ma = 6000; e.req_mv = 11000; e.req_ma = 6000; e.cable_ma = 5000; e.authed_ms = 340; e.answer_ms = 12;
    e.apdo = 0xC0A8C8DC; e.rdo = 0x30001770;
    // slot 1 of 2: run id and count, then the slot field for field (little endian)
    CHECK_EQ(run(f, request(0x0E, 1, {1}), r), 1);
    check_header(r, 0x0E, 1, Status::Ok, 5 + 29);
    CHECK_EQ(f.hunt_asked, 1);
    check_bytes(r, kRspHeader, {4, 3, 2, 1, 2,   7, uint8_t(ct::HuntCause::RefusedDespiteOffer), 2, 0x0A, 5, 3, 3,
                                0xCF, 0x01, 0x70, 0x17, 0xF8, 0x2A, 0x70, 0x17, 0x88, 0x13, 0x54, 0x01, 0x0C, 0x00,
                                0xDC, 0xC8, 0xA8, 0xC0, 0x70, 0x17, 0x00, 0x30});
    // a slot at or past the count answers the header only
    CHECK_EQ(run(f, request(0x0E, 2, {2}), r), 1);
    check_header(r, 0x0E, 2, Status::Ok, 5);
    check_bytes(r, kRspHeader, {4, 3, 2, 1, 2});
    f.hunt_reply.count = 0;
    CHECK_EQ(run(f, request(0x0E, 3, {0}), r), 1);
    check_header(r, 0x0E, 3, Status::Ok, 5);
    // malformed requests never reach the backend
    const int calls = f.calls;
    CHECK_EQ(run(f, request(0x0E, 4, {}), r), 1); check_header(r, 0x0E, 4, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x0E, 5, {0, 0}), r), 1); check_header(r, 0x0E, 5, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x0E, 6, {uint8_t(ct::kHuntSlots)}), r), 1); check_header(r, 0x0E, 6, Status::BadArg, 0);
    CHECK_EQ(f.calls, calls);
}

static void test_qrtest() {
    Fake f; Frame r;
    // list, no args: the first chunk of the fit table (off 0), header off(2) total(2) n(1).
    const std::size_t total = k2::qrtest::list_total();
    CHECK(total > 0);
    CHECK_EQ(run(f, request(0x53, 1, {kQrTestList}), r), 1);
    check_header(r, 0x53, 1, Status::Ok, 5 + std::min<std::size_t>(kQrTestChunk, total));
    CHECK_EQ(r[kRspHeader] | r[kRspHeader + 1] << 8, 0);
    CHECK_EQ(r[kRspHeader + 2] | r[kRspHeader + 3] << 8, total);
    CHECK_EQ(r[kRspHeader + 4], std::min<std::size_t>(kQrTestChunk, total));
    // The chunks from 0 reassemble the whole table (the same text list_line() builds).
    std::string all;
    {
        char line[48];
        for (int i = 0; i < k2::qrtest::kListLines; i++) {
            const int n = k2::qrtest::list_line(i, line);
            CHECK(n > 0);
            all.append(line, static_cast<std::size_t>(n));
        }
    }
    CHECK_EQ(all.size(), total);
    std::string got;
    for (std::size_t off = 0; off < total;) {
        CHECK_EQ(run(f, request(0x53, 2, {kQrTestList, uint8_t(off), uint8_t(off >> 8)}), r), 1);
        CHECK_EQ(r[kRspHeader] | r[kRspHeader + 1] << 8, off);
        CHECK_EQ(r[kRspHeader + 2] | r[kRspHeader + 3] << 8, total);
        const uint8_t n = r[kRspHeader + 4];
        CHECK_EQ(r[4], static_cast<uint8_t>(Status::Ok));
        CHECK_EQ(r[5], 5 + n);
        got.append(reinterpret_cast<const char*>(&r[kRspHeader + 5]), n);
        off += n;
        if (n == 0) break;
    }
    CHECK(got == all);
    // list with a bad length / an offset past the end.
    CHECK_EQ(run(f, request(0x53, 3, {kQrTestList, 0}), r), 1);
    check_header(r, 0x53, 3, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x53, 4, {kQrTestList, uint8_t(total + 1), uint8_t((total + 1) >> 8)}), r), 1);
    check_header(r, 0x53, 4, Status::BadArg, 0);
    // show: ok echoes version px quiet and calls the backend once.
    CHECK_EQ(run(f, request(0x53, 5, {kQrTestShow, 13, 3, 4}), r), 1);
    check_header(r, 0x53, 5, Status::Ok, 3);
    check_bytes(r, kRspHeader, {13, 3, 4});
    CHECK(f.qrtest_show_called);
    CHECK_EQ(f.qrtest_shown[0], 13); CHECK_EQ(f.qrtest_shown[1], 3); CHECK_EQ(f.qrtest_shown[2], 4);
    // show: bad ranges answer BadArg with the reason as the payload, and the backend is not called.
    f.qrtest_show_called = false;
    auto show_bad = [&](std::initializer_list<uint8_t> p, const char* needle) {
        CHECK_EQ(run(f, request(0x53, 6, p), r), 1);
        CHECK_EQ(r[4], static_cast<uint8_t>(Status::BadArg));
        const std::string reason(reinterpret_cast<const char*>(&r[kRspHeader]), r[5]);
        CHECK(reason.find(needle) != std::string::npos);
        CHECK(!f.qrtest_show_called);
    };
    show_bad({kQrTestShow, 0, 3, 4}, "version 0");
    show_bad({kQrTestShow, 41, 3, 4}, "41");
    show_bad({kQrTestShow, 13, 1, 4}, "px 1");
    show_bad({kQrTestShow, 13, 4, 4}, "px 4");
    show_bad({kQrTestShow, 13, 3, 0}, "quiet 0");
    show_bad({kQrTestShow, 13, 3, 5}, "quiet 5");
    // show: the box does not fit the 240 px panel -> BadArg with the box size in the reason.
    show_bad({kQrTestShow, 25, 2, 2}, "242");
    show_bad({kQrTestShow, 14, 3, 4}, "243");
    show_bad({kQrTestShow, 26, 2, 1}, "246");
    // show: wrong lengths.
    CHECK_EQ(run(f, request(0x53, 7, {kQrTestShow, 13, 3}), r), 1);
    check_header(r, 0x53, 7, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x53, 8, {kQrTestShow, 13, 3, 4, 0}), r), 1);
    check_header(r, 0x53, 8, Status::BadLength, 0);
    // show: the backend's refusal is relayed.
    f.qrtest_status = Status::Busy;
    CHECK_EQ(run(f, request(0x53, 9, {kQrTestShow, 11, 3, 4}), r), 1);
    check_header(r, 0x53, 9, Status::Busy, 0);
    f.qrtest_status = Status::Ok;
    // stop: reports whether a test screen was showing; unknown op and empty payload.
    f.qrtest_stopped_value = true;
    CHECK_EQ(run(f, request(0x53, 10, {kQrTestStop}), r), 1);
    check_header(r, 0x53, 10, Status::Ok, 1);
    CHECK_EQ(r[kRspHeader], 1);
    CHECK(f.qrtest_stopped);
    f.qrtest_stopped_value = false;
    CHECK_EQ(run(f, request(0x53, 11, {kQrTestStop}), r), 1);
    check_header(r, 0x53, 11, Status::Ok, 1);
    CHECK_EQ(r[kRspHeader], 0);
    CHECK_EQ(run(f, request(0x53, 12, {kQrTestStop, 0}), r), 1);
    check_header(r, 0x53, 12, Status::BadLength, 0);
    CHECK_EQ(run(f, request(0x53, 13, {7}), r), 1);
    check_header(r, 0x53, 13, Status::BadArg, 0);
    CHECK_EQ(run(f, request(0x53, 14, {}), r), 1);
    check_header(r, 0x53, 14, Status::BadLength, 0);
}

int main() {
    test_charger_hunt();
    test_charger_cable();
    test_charger_units();
    test_charger_test_wire();
    test_active_pd_commands();
    test_store();
    test_cable_r();
    test_lab();
    test_emlib();
    test_cards();
    test_blackbox();
    test_transcap();
    test_debug_fake_pdos();
    test_debug_link();
    test_debug_epr_info();
    test_debug_lcd();
    test_debug_cc_probe();
    test_debug_pd_msg();
    test_screenshot();
    test_stream_frame();
    test_parse_valid();
    test_parse_bad_magic_and_short();
    test_truncated();
    test_oversize();
    test_unknown_command();
    test_seq_echo();
    test_ping();
    test_ping_long_git_string();
    test_button();
    test_state();
    test_adc_raw();
    test_ripple();
    test_fc();
    test_ripple_frame();
    test_pdos();
    test_emarker();
    test_identity();
    test_action_page_reboot_log();
    test_dfu_enter();
    test_factory_wipe();
    test_flash_dump();
    test_boot_info();
    test_ufcs();
    test_qrtest();
    test_debug_fault();
    test_build_response_pads_and_cuts();
    return test_report("test_remote_proto");
}
