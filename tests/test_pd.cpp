// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the pure USB-PD decoding in k2::pd: Source_Capabilities PDOs, e-marker
// VDOs and message names. Expected values are worked out by hand from USB PD 3.1 table 6-x
// and were first checked against the old C driver (commit 266caf8) before the port.
#include "k2/pd/pd.hpp"
#include "test_util.h"
#include <string_view>

using namespace k2::pd;

static void put_le32(uint8_t* dst, uint32_t v) {
    for (int i = 0; i < 4; i++) dst[i] = (uint8_t)(v >> (8 * i));
}

/* Source_Capabilities header: message type 1, specification revision bits 7:6, N data objects. */
static uint16_t caps_header(unsigned rev_bits, unsigned num_do) {
    return (uint16_t)(0x01u | (rev_bits << 6) | (num_do << 12));
}

static void test_wire_revision_evidence() {
    const std::array<uint8_t, 4> payload{0x2C, 0x91, 0x01, 0x00};
    for (uint8_t wire : {uint8_t{1}, uint8_t{2}}) {
        SourceCaps caps;
        decode_source_caps(caps, caps_header(wire, 1), payload, 1);
        CHECK_EQ(caps.spec_rev, wire);
    }
    CHECK_EQ(std::string_view(wire_revision_name(2)) == "PD3.x", true);
}

static void test_physical_cable_limits() {
    Emarker e;
    CHECK_EQ(cable_limits(e).max_mv, 20000);
    CHECK_EQ(cable_limits(e).observed, false);
    e.valid = true; e.num_vdos = 5; e.spec_rev = 2;
    e.max_current_a = 5; e.max_voltage_v = 50; e.epr_capable = true; e.vdo_version = 2;
    CHECK_EQ(cable_limits(e).max_mv, 50000);
    CHECK_EQ(cable_limits(e).max_ma, 5000);
    CHECK_EQ(cable_limits(e).observed, true);
    e.spec_rev = 1;
    CHECK_EQ(cable_limits(e).max_mv, 20000);
    e.spec_rev = 2; e.vdo_version = 0;
    CHECK_EQ(cable_limits(e).max_mv, 20000);
    e.vdo_version = 2;
    e.is_active = true;
    CHECK_EQ(cable_limits(e).max_mv, 20000);  // incomplete active-cable identity
    e.vdo2_valid = true;
    CHECK_EQ(cable_limits(e).max_mv, 50000);
    e.max_current_a = 3;
    CHECK_EQ(cable_limits(e).max_mv, 20000);
    CHECK_EQ(cable_limits(e).max_ma, 3000);
    e.num_vdos = 4;
    CHECK_EQ(cable_limits(e).observed, false);
}
static void test_passive_50v_epr_cable_limits() {
    // Baseus-bundled 240 W cable, VID 0x315C: passive, VDO version 1.0 (000b), EPR Mode Capable (bit 17),
    // 50 V (10:9 = 11b), 5 A (6:5 = 10b). Layout from USB PD 3.1 table "Passive Cable VDO" (from memory: unverified).
    const uint32_t identity[5] = {0xff00a041, 0x1800315c, 0, 0x11960000, 0x600a2e40};
    uint8_t bytes[sizeof(identity)];
    for (unsigned n = 0; n < 5; ++n) put_le32(bytes + n * 4, identity[n]);
    Emarker e = parse_emarker(bytes, 5);
    e.spec_rev = 2;
    CHECK_EQ(e.max_voltage_v, 50);
    CHECK_EQ(cable_limits(e).max_mv, 50000);
    CHECK_EQ(cable_limits(e).max_ma, 5000);
    Emarker no_bit = e;
    no_bit.raw_vdos[4] &= ~(1u << 17);   // 50 V claimed without the EPR capable bit: not trusted
    CHECK_EQ(cable_limits(no_bit).max_mv, 20000);
}
static void test_passive_30v_cable_limits() {
    const uint32_t identity[5] = {0xff00a041, 0x180022d9, 0, 0x11960000, 0x60082a40};
    uint8_t bytes[sizeof(identity)];
    for (unsigned n = 0; n < 5; ++n) put_le32(bytes + n * 4, identity[n]);
    Emarker e = parse_emarker(bytes, 5);
    e.spec_rev = 2;
    CHECK_EQ(e.max_voltage_v, 30);
    CHECK_EQ(e.max_current_a, 5);
    CHECK_EQ(cable_limits(e).max_mv, 30000);
    CHECK_EQ(cable_limits(e).max_ma, 5000);
    for (unsigned mode = 0; mode < 10; ++mode) {
        Emarker bad = e;
        if (mode == 0) bad.raw_vdos[0] ^= 1u << 16;
        if (mode == 1) bad.raw_vdos[1] &= ~(7u << 27);
        if (mode == 2) bad.raw_vdos[4] |= 1u << 21;
        if (mode == 3) bad.raw_vdos[4] |= 1u << 7;
        if (mode == 4) bad.raw_vdos[4] |= 1u << 5;
        if (mode == 5) bad.raw_vdos[4] &= ~(3u << 18);
        if (mode == 6) bad.spec_rev = 1;
        if (mode == 7) bad.raw_vdos[4] &= ~(15u << 13);
        if (mode == 8) bad.raw_vdos[4] |= 3u << 11;
        if (mode == 9) bad.num_vdos = 4;
        CHECK_EQ(cable_limits(bad).max_mv, 20000);
    }
}

static void test_source_caps() {
    const uint32_t pdo[6] = {
        (100u << 10) | 300u,                                   /* Fixed 5 V / 3 A */
        (400u << 10) | 500u,                                   /* Fixed 20 V / 5 A */
        (1u << 30) | (420u << 20) | (100u << 10) | 240u,       /* Battery 5-21 V, 60 W */
        (2u << 30) | (240u << 20) | (100u << 10) | 300u,       /* Variable 5-12 V, 3 A */
        (3u << 30) | (110u << 17) | (33u << 8) | 60u,          /* PPS 3.3-11 V, 3 A */
        (3u << 30) | (1u << 28) | (280u << 17) | (150u << 8) | 100u, /* AVS 15-28 V, 100 W */
    };
    uint8_t payload[24 + 4];
    for (int i = 0; i < 6; i++) put_le32(payload + 4 * i, pdo[i]);

    SourceCaps g_caps{};
    decode_source_caps(g_caps, caps_header(2, 6), payload, 1);
    CHECK_EQ(g_caps.valid, true);
    CHECK_EQ(g_caps.num_pdos, 6);
    CHECK_EQ(g_caps.spec_rev, 2);          /* PD3.x messaging, not proof of PD3.1 */
    CHECK_EQ(g_caps.raw_header, caps_header(2, 6));
    CHECK_EQ(g_caps.rx_count, 1);
    CHECK_EQ(g_caps.active_cc, 1);

    CHECK_EQ((int)g_caps.pdos[0].type, (int)PdoType::Fixed);
    CHECK_EQ(g_caps.pdos[0].raw, pdo[0]);
    CHECK_EQ(g_caps.pdos[0].max_mv, 5000);
    CHECK_EQ(g_caps.pdos[0].min_mv, 5000);
    CHECK_EQ(g_caps.pdos[0].max_ma, 3000);
    CHECK_EQ(g_caps.pdos[0].max_mw, 15000);

    CHECK_EQ((int)g_caps.pdos[1].type, (int)PdoType::Fixed);
    CHECK_EQ(g_caps.pdos[1].max_mv, 20000);
    CHECK_EQ(g_caps.pdos[1].max_ma, 5000);
    CHECK_EQ(g_caps.pdos[1].max_mw, 100000);

    CHECK_EQ((int)g_caps.pdos[2].type, (int)PdoType::Battery);
    CHECK_EQ(g_caps.pdos[2].max_mv, 21000);
    CHECK_EQ(g_caps.pdos[2].min_mv, 5000);
    CHECK_EQ(g_caps.pdos[2].max_mw, 60000);
    CHECK_EQ(g_caps.pdos[2].max_ma, 12000);   /* 60 W / 5 V */

    CHECK_EQ((int)g_caps.pdos[3].type, (int)PdoType::Variable);
    CHECK_EQ(g_caps.pdos[3].max_mv, 12000);
    CHECK_EQ(g_caps.pdos[3].min_mv, 5000);
    CHECK_EQ(g_caps.pdos[3].max_ma, 3000);
    CHECK_EQ(g_caps.pdos[3].max_mw, 36000);

    CHECK_EQ((int)g_caps.pdos[4].type, (int)PdoType::Pps);
    CHECK_EQ(g_caps.pdos[4].max_mv, 11000);
    CHECK_EQ(g_caps.pdos[4].min_mv, 3300);
    CHECK_EQ(g_caps.pdos[4].max_ma, 3000);
    CHECK_EQ(g_caps.pdos[4].max_mw, 33000);

    CHECK_EQ((int)g_caps.pdos[5].type, (int)PdoType::Avs);
    CHECK_EQ(g_caps.pdos[5].max_mv, 28000);
    CHECK_EQ(g_caps.pdos[5].min_mv, 15000);
    CHECK_EQ(g_caps.pdos[5].max_ma, 3571);    /* 100 W / 28 V */
    CHECK_EQ(g_caps.pdos[5].max_mw, 100000);
}

/* PD 3.2 SPR AVS (APDO subtype 10b), layout of the PD 3.2 PDO. */
static void test_spr_avs() {
    Pdo a = decode_pdo((3u << 30) | (2u << 28) | (300u << 10));            /* 9-15 V at 3 A */
    CHECK_EQ((int)a.type, (int)PdoType::Avs);
    CHECK_EQ(a.min_mv, 9000);
    CHECK_EQ(a.max_mv, 15000);
    CHECK_EQ(a.max_ma, 3000);
    CHECK_EQ(a.max_mw, 45000);
    Pdo b = decode_pdo((3u << 30) | (2u << 28) | (300u << 10) | 200u);     /* 9-20 V at 2 A */
    CHECK_EQ(b.max_mv, 20000);
    CHECK_EQ(b.max_ma, 2000);
    CHECK_EQ(b.max_mw, 40000);
    CHECK_EQ(current_limit_ma(b, 9000), 3000);
    CHECK_EQ(current_limit_ma(b, 15000), 3000);
    CHECK_EQ(current_limit_ma(b, 15100), 2000);
    CHECK_EQ(current_limit_ma(b, 20000), 2000);
    Pdo c = decode_pdo(0xE0000000u | (50u << 10) | 200u);
    CHECK_EQ(current_limit_ma(c, 9000), 500);
    CHECK_EQ((int)decode_pdo((3u << 30) | (3u << 28)).type, (int)PdoType::Unknown);  /* reserved */
}

static void test_source_caps_header_fields() {
    uint8_t payload[28] = {0};
    put_le32(payload, (100u << 10) | 300u);
    SourceCaps g_caps{};
    decode_source_caps(g_caps, caps_header(0, 1), payload, 2);
    CHECK_EQ(g_caps.spec_rev, 0);          /* not PD2.0 */
    CHECK_EQ(g_caps.num_pdos, 1);
    CHECK_EQ(g_caps.rx_count, 1);
    decode_source_caps(g_caps, caps_header(1, 7), payload, 2);
    CHECK_EQ(g_caps.spec_rev, 1);          /* PD2.0 messaging */
    CHECK_EQ(g_caps.num_pdos, 7);          /* kMaxPdos */
    CHECK_EQ(g_caps.rx_count, 2);
}

/* Discover Identity ACK from SOP': VDM header, ID header, cert stat, product, cable VDO 1, [VDO 2]. */
static void emarker(uint32_t* vdos, unsigned n, Emarker* out) {
    uint8_t body[24 + 4] = {0};
    for (unsigned i = 0; i < n; i++) put_le32(body + 4 * i, vdos[i]);
    *out = parse_emarker(body, n);
}

static void test_emarker() {
    Emarker e;

    /* Passive cable (product type 3), Apple, 5 A / 50 V, USB 3.2 Gen2 -> 240 W */
    uint32_t a[5] = { 0xFF00A041u, (3u << 27) | 0x05ACu, 0x12345678u,
                      (0x1234u << 16) | (0x05u << 8) | 0x07u,
                      (3u << 9) | (2u << 5) | 2u };
    emarker(a, 5, &e);
    CHECK_EQ(e.valid, true);
    CHECK_EQ(e.num_vdos, 5);
    CHECK_EQ(e.is_active, false);
    CHECK_EQ(e.vid, 0x05AC);
    CHECK_STR(e.vendor_str, "Apple");
    CHECK_EQ(e.xid, 0x12345678u);
    CHECK_EQ(e.pid, 0x1234);
    CHECK_EQ(e.hw_rev, 5);
    CHECK_EQ(e.fw_rev, 7);
    CHECK_EQ(e.max_voltage_v, 50);
    CHECK_EQ(e.max_current_a, 5);
    CHECK_EQ(e.max_power_w, 240);
    CHECK_STR(e.speed_str, "USB 3.2 Gen2 (10G)");

    /* Active cable (product type 4), Intel, 3 A, 40 V; VDO 2 carries temperatures and flags and
       must not change the rating (the old "EPR bit" in VDO 2 was not in the PD spec). */
    uint32_t b[6] = { 0xFF00A041u, (4u << 27) | 0x8087u, 0, 0x00020101u,
                      (2u << 9) | (1u << 5) | 3u, 1u << 20 };
    emarker(b, 6, &e);
    CHECK_EQ(e.is_active, true);
    CHECK_STR(e.vendor_str, "Intel");
    CHECK_EQ(e.max_voltage_v, 40);
    CHECK_EQ(e.max_current_a, 3);
    CHECK_EQ(e.max_power_w, 60);
    CHECK_EQ(e.epr_capable, false);
    CHECK_STR(e.speed_str, "Thunderbolt (40G)");  /* Intel VID with speed >= 2 */

    /* 5 A cables: 40 V -> 200 W, 30 V -> 100 W, 20 V -> 100 W */
    uint32_t c[5] = { 0, 0x1234u, 0, 0, (2u << 9) | (2u << 5) | 4u };
    emarker(c, 5, &e);
    CHECK_STR(e.vendor_str, "Unknown");
    CHECK_EQ(e.max_voltage_v, 40);
    CHECK_EQ(e.max_power_w, 200);
    CHECK_STR(e.speed_str, "USB4 Gen4 (80G)");
    c[4] = (1u << 9) | (2u << 5);
    emarker(c, 5, &e);
    CHECK_EQ(e.max_voltage_v, 30);
    CHECK_EQ(e.max_power_w, 100);
    CHECK_STR(e.speed_str, "USB 2.0 (480M)");
    c[4] = (2u << 5) | 1u;
    emarker(c, 5, &e);
    CHECK_EQ(e.max_voltage_v, 20);
    CHECK_EQ(e.max_power_w, 100);
    CHECK_STR(e.speed_str, "USB 3.2 Gen1 (5G)");

    /* Truncated answers keep the defaults: 3 A / 20 V / 60 W, USB 2.0 */
    uint32_t d[4] = { 0, 0x2E6Bu, 0xCAFEu, (0xBEEFu << 16) | 0x0102u };
    emarker(d, 4, &e);
    CHECK_STR(e.vendor_str, "ChargerLAB");
    CHECK_EQ(e.pid, 0xBEEF);
    CHECK_EQ(e.hw_rev, 1);
    CHECK_EQ(e.fw_rev, 2);
    CHECK_EQ(e.max_current_a, 3);
    CHECK_EQ(e.max_voltage_v, 20);
    CHECK_EQ(e.max_power_w, 60);
    CHECK_STR(e.speed_str, "USB 2.0 (480M)");
    emarker(d, 1, &e);
    CHECK_EQ(e.num_vdos, 1);
    CHECK_STR(e.vendor_str, "Unknown");
    CHECK_EQ(e.vid, 0);
}

/* Cable VDO 1 / VDO 2 fields (USB PD 3.1 6.4.4.3.1 and 6.4.4.3.2). */
static void test_cable_vdo_fields() {
    Emarker e;
    /* Passive, HW 3 / FW 4, VDO version 3, Type-C plug, latency 0010 (~2 m), termination 01
       (VCONN required), 50 V, 5 A, Gen2 */
    uint32_t a[5] = { 0xFF00A041u, (3u << 27) | 0x05ACu, 0, 0,
                      (3u << 28) | (4u << 24) | (3u << 21) | (2u << 18) | (2u << 13) | (1u << 11) |
                      (3u << 9) | (2u << 5) | 2u };
    emarker(a, 5, &e);
    CHECK_EQ(e.cable_hw, 3);
    CHECK_EQ(e.cable_fw, 4);
    CHECK_EQ(e.vdo_version, 3);
    CHECK_EQ(e.type_c_plug, true);
    CHECK_EQ(e.latency_code, 2);
    CHECK_STR(e.latency_str, "10-20 ns (~2 m)");
    CHECK_EQ(e.termination, 1);
    CHECK_STR(e.termination_str, "Passive, VCONN required");
    CHECK_EQ(e.vconn_required, true);
    CHECK_EQ(e.epr_capable, true);
    CHECK_EQ(e.vdo2_valid, false);

    a[4] = (1u << 5);  /* 3 A, latency 0 reserved, passive no VCONN, 20 V */
    emarker(a, 5, &e);
    CHECK_STR(e.termination_str, "Passive");
    CHECK_STR(e.latency_str, "n/a");
    CHECK_EQ(e.vconn_required, false);
    CHECK_EQ(e.type_c_plug, false);
    CHECK_EQ(e.epr_capable, false);

    /* 5 A but only 20 V is not EPR; 50 V with 3 A is not EPR either */
    a[4] = (2u << 5);
    emarker(a, 5, &e);
    CHECK_EQ(e.epr_capable, false);
    a[4] = (3u << 9) | (1u << 5);
    emarker(a, 5, &e);
    CHECK_EQ(e.epr_capable, false);

    /* Active cable: termination 10 (one end), latency 1000, VDO 2: 60/70 C, U3 power 2, USB4
       supported (bit 8 clear), two lanes, Gen2+, optically isolated */
    uint32_t b[6] = { 0xFF00A041u, (4u << 27) | 0x8087u, 0, 0, (2u << 11) | (8u << 13) | (3u << 9) | (2u << 5) | 3u,
                      (60u << 24) | (70u << 16) | (2u << 12) | (1u << 3) | (1u << 2) | 1u };
    emarker(b, 6, &e);
    CHECK_EQ(e.termination, 2);
    CHECK_STR(e.termination_str, "Active, one end");
    CHECK_STR(e.latency_str, ">70 ns");
    CHECK_EQ(e.vdo2_valid, true);
    CHECK_EQ(e.max_temp_c, 60);
    CHECK_EQ(e.shutdown_temp_c, 70);
    CHECK_EQ(e.u3_power, 2);
    CHECK_EQ(e.usb4_supported, true);
    CHECK_EQ(e.usb_lanes, 2);
    CHECK_EQ(e.usb_gen2, true);
    CHECK_EQ(e.optical_isolated, true);
    CHECK_EQ(e.epr_capable, true);
    b[5] = 1u << 8;  /* bit 8 set = USB4 not supported */
    emarker(b, 6, &e);
    CHECK_EQ(e.usb4_supported, false);
    CHECK_EQ(e.usb_lanes, 1);

    /* VDO 2 of a passive cable is not decoded (the sixth VDO would be something else) */
    uint32_t c[6] = { 0xFF00A041u, (3u << 27) | 0x1234u, 0, 0, 0, 0xFFFFFFFFu };
    emarker(c, 6, &e);
    CHECK_EQ(e.vdo2_valid, false);
}

static void test_control_name() {
    CHECK_STR(control_name(0x01), "GoodCRC");
    CHECK_STR(control_name(0x03), "Accept");
    CHECK_STR(control_name(0x06), "PS_RDY");
    CHECK_STR(control_name(0x07), "Get_Source_Cap");
    CHECK_STR(control_name(0x0D), "Soft_Reset");
    CHECK_STR(control_name(0x0B), "OTHER");
    CHECK_STR(control_name(0x1F), "OTHER");
}


static void test_decode_rdo() {
    // fixed: pos 3, mismatch + usb comm, 1.5 A operating, 3 A max
    const Pdo fx = decode_pdo(0x0002D12C);  // 9 V 3 A
    Rdo r = decode_rdo((3u << 28) | (3u << 25) | (150u << 10) | 300u, &fx);
    CHECK_EQ(r.pos, 3);
    CHECK_EQ(int(r.kind), int(PdoType::Fixed));
    CHECK_EQ(r.mv, 9000);
    CHECK_EQ(r.op_ma, 1500);
    CHECK_EQ(r.max_ma, 3000);
    CHECK_EQ(r.flags, kRdoFMismatch | kRdoFUsbComm);
    // PPS: 9.0 V (450 x 20 mV), 2.0 A (40 x 50 mA)
    const Pdo pps = decode_pdo(0xC0DC2164);
    r = decode_rdo((5u << 28) | (1u << 24) | (450u << 9) | 40u, &pps);
    CHECK_EQ(int(r.kind), int(PdoType::Pps));
    CHECK_EQ(r.mv, 9000);
    CHECK_EQ(r.op_ma, 2000);
    CHECK_EQ(r.flags, kRdoFNoSuspend);
    // no PDO known: current from the Fixed layout, voltage unknown
    r = decode_rdo((4u << 28) | (300u << 10) | 300u, nullptr);
    CHECK_EQ(r.mv, 0);
    CHECK_EQ(r.op_ma, 3000);
    CHECK_EQ(int(r.kind), int(PdoType::Unknown));
}

int main() {
    test_wire_revision_evidence();
    test_physical_cable_limits(); test_passive_30v_cable_limits(); test_passive_50v_epr_cable_limits();
    test_source_caps();
    test_spr_avs();
    test_source_caps_header_fields();
    test_emarker();
    test_cable_vdo_fields();
    test_control_name();
    test_decode_rdo();
    return test_report("test_pd");
}
