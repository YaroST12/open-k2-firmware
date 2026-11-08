// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the extended-message parsers (USB PD 3.1 6.5) and their routing through
// Engine. Byte vectors are written out by hand, field by field, from the PD 3.1 data blocks:
// SCEDB table 6-52, PPSSDB 6-45, SDB 6-50, BCDB 6-47, MIDB 6-56, RMDO 6-26.
#include "k2/pd/pd_engine.hpp"
#include "test_util.h"

using namespace k2::pd;

// header = extended bit, N data objects, PD 3.1, type; then ext header (chunked, chunk 0,
// size) and the data block. Returns the payload (ext header + data) for Engine::on_message.
struct Frame {
    std::uint16_t header;
    std::uint8_t payload[32];
    std::size_t len;
};
static Frame ext_frame(std::uint8_t type, const std::uint8_t* data, std::size_t size) {
    Frame f{};
    f.payload[0] = size & 0xFF;
    f.payload[1] = 0x80 | (size >> 8);  // chunked, chunk 0
    for (std::size_t i = 0; i < size; i++) f.payload[2 + i] = data[i];
    f.len = (2 + size + 3) & ~std::size_t(3);
    f.header = 0x8000 | ((f.len / 4) << 12) | (2u << 6) | type;
    return f;
}
static void feed(Engine& e, const Frame& f) { e.on_message(Sop::Sop, f.header, {f.payload, f.len}, 1); }

static const std::uint8_t kScedb[24] = {
    0xAC, 0x05,                  // VID 0x05AC
    0x34, 0x12,                  // PID 0x1234
    0x44, 0x33, 0x22, 0x11,      // XID 0x11223344
    0x07, 0x03,                  // FW 7, HW 3
    0x01, 0x00,                  // voltage regulation, hold-up
    0x01, 0x00,                  // compliance, touch current
    0x64, 0x00, 0xC8, 0x00, 0x2C, 0x01,  // peak current 1..3: 100, 200, 300
    0x01, 0x02, 0x00,            // touch temp, source inputs, batteries
    100,                         // PDP 100 W
};

static void test_source_caps_ext() {
    Engine e;
    feed(e, ext_frame(0x01, kScedb, 24));
    const SourceCapsExt& s = e.ext.caps_ext;
    CHECK_EQ(s.valid, true);
    CHECK_EQ(s.vid, 0x05AC);
    CHECK_EQ(s.pid, 0x1234);
    CHECK_EQ(s.xid, 0x11223344u);
    CHECK_EQ(s.fw, 7);
    CHECK_EQ(s.hw, 3);
    CHECK_EQ(s.peak_current[0], 100);
    CHECK_EQ(s.peak_current[2], 300);
    CHECK_EQ(s.source_inputs, 2);
    CHECK_EQ(s.pdp_w, 100);
    CHECK_EQ(s.epr_pdp_w, 0);               // 24-byte PD 3.0 block has no EPR rating
    std::uint8_t b25[25];
    for (int i = 0; i < 24; i++) b25[i] = kScedb[i];
    b25[24] = 240;                          // PD 3.1: EPR source PDP rating
    feed(e, ext_frame(0x01, b25, 25));
    CHECK_EQ(e.ext.caps_ext.epr_pdp_w, 240);
    CHECK_EQ(e.ext.caps_ext.pdp_w, 100);
    Engine s2;
    feed(s2, ext_frame(0x01, kScedb, 20));  // truncated block: ignored
    CHECK_EQ(s2.ext.caps_ext.valid, false);
}

static void test_status() {
    Engine e;
    const std::uint8_t sdb[6] = {45, 0x06, 0x00, 0x10, 0x02, 0x00};  // 45 C, ext power + AC, event flag, temp normal
    feed(e, ext_frame(0x02, sdb, 6));
    const Status& s = e.ext.status;
    CHECK_EQ(s.valid, true);
    CHECK_EQ(s.internal_temp_c, 45);
    CHECK_EQ(s.present_input, 0x06);
    CHECK_EQ(s.event_flags, 0x10);
    CHECK_EQ(s.temp_status, 1);             // bits 2:1 of 0x02
    const std::uint8_t sdb5[5] = {1, 0, 0, 0, 0x06};
    Engine e5;
    feed(e5, ext_frame(0x02, sdb5, 5));     // 5-byte older block
    CHECK_EQ(e5.ext.status.valid, true);
    CHECK_EQ(e5.ext.status.temp_status, 3); // over temperature
}

static void test_pps_status() {
    Engine e;
    const std::uint8_t d[4] = {0xFA, 0x00, 0x28, 0x0A};  // 250 * 20 mV = 5 V, 40 * 50 mA = 2 A, PTF 1 + OMF
    feed(e, ext_frame(0x0C, d, 4));
    const PpsStatus& p = e.ext.pps;
    CHECK_EQ(p.valid, true);
    CHECK_EQ(p.out_mv, 5000);
    CHECK_EQ(p.out_ma, 2000);
    CHECK_EQ(p.ptf, 1);
    CHECK_EQ(p.omf, true);
    const std::uint8_t na[4] = {0xFF, 0xFF, 0xFF, 0x00};  // output not reported
    feed(e, ext_frame(0x0C, na, 4));
    CHECK_EQ(e.ext.pps.out_mv, 0);
    CHECK_EQ(e.ext.pps.out_mv_known, false);
    CHECK_EQ(e.ext.pps.out_ma_known, false);
}

static void test_manufacturer_info() {
    Engine e;
    std::uint8_t d[4 + 8] = {0xAC, 0x05, 0x34, 0x12, 'A', 'n', 'k', 'e', 'r', 0, 'x', 'x'};
    feed(e, ext_frame(0x07, d, 12));
    CHECK_EQ(e.ext.mfr.valid, true);
    CHECK_EQ(e.ext.mfr.vid, 0x05AC);
    CHECK_EQ(e.ext.mfr.pid, 0x1234);
    CHECK_STR(e.ext.mfr.name, "Anker");       // stops at the NUL
    d[5] = 0x07;                               // non-printable byte shows as '?'
    feed(e, ext_frame(0x07, d, 12));
    CHECK_STR(e.ext.mfr.name, "A?ker");
}

static void test_battery_caps() {
    Engine e;
    const std::uint8_t d[9] = {0xAC, 0x05, 0x34, 0x12, 0x2C, 0x01, 0x20, 0x01, 0x00};  // 30.0 Wh, 28.8 Wh
    feed(e, ext_frame(0x05, d, 9));
    const BatteryCaps& b = e.ext.battery;
    CHECK_EQ(b.valid, true);
    CHECK_EQ(b.vid, 0x05AC);
    CHECK_EQ(b.design_dwh, 300);
    CHECK_EQ(b.last_full_dwh, 288);
    CHECK_EQ(b.invalid_ref, false);
    const std::uint8_t inv[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    feed(e, ext_frame(0x05, inv, 9));
    CHECK_EQ(e.ext.battery.invalid_ref, true);
}

static void test_revision() {
    Engine e;
    // Revision_Message_Data_Object is a data message (type 0x0C): rev 3.1, version 1.7.
    const std::uint8_t d[4] = {0x00, 0x00, 0x17, 0x31};
    e.on_message(Sop::Sop, std::uint16_t((1u << 12) | (1u << 8) | (2u << 6) | kDataRevision), d, 1);
    CHECK_EQ(e.ext.revision.valid, true);
    CHECK_EQ(e.ext.revision.rev_major, 3);
    CHECK_EQ(e.ext.revision.rev_minor, 1);
    CHECK_EQ(e.ext.revision.ver_major, 1);
    CHECK_EQ(e.ext.revision.ver_minor, 7);
}

static void test_ignores_non_sop_and_continuation_chunks() {
    Engine e;
    Frame f = ext_frame(0x02, kScedb, 6);
    e.on_message(Sop::SopP, f.header, {f.payload, f.len}, 1);
    CHECK_EQ(e.ext.status.valid, false);
    f.payload[1] |= (1 << 3);               // chunk number 1: not the start of a block
    feed(e, f);
    CHECK_EQ(e.ext.status.valid, false);
    f.payload[1] = 0x80 | (1 << 2);         // chunk request bit: no data
    feed(e, f);
    CHECK_EQ(e.ext.status.valid, false);
}

static void test_reset_clears_ext() {
    Engine e;
    feed(e, ext_frame(0x01, kScedb, 24));
    e.reset();
    CHECK_EQ(e.ext.caps_ext.valid, false);
}

int main() {
    test_source_caps_ext();
    test_status();
    test_pps_status();
    test_manufacturer_info();
    test_battery_caps();
    test_revision();
    test_ignores_non_sop_and_continuation_chunks();
    test_reset_clears_ext();
    return test_report("test_pd_ext");
}
