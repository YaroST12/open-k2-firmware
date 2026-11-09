// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the VDM / SVID side of the engine (USB PD 3.1 6.4.4): Discover Identity,
// Discover SVIDs and Discover Modes ACKs seen on SOP, the SVID name table and the Apple
// Product-VDO model lookup. Header words are written out by bit:
// 0xFF008041 = SVID 0xFF00, structured, ACK (7:6 = 01), command 1 (Discover Identity).
#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_vdm.hpp"
#include <cstring>
#include "test_util.h"

using namespace k2::pd;

static std::uint8_t g_buf[28];
static void put(std::uint32_t* v, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        for (int b = 0; b < 4; b++) g_buf[4 * i + b] = std::uint8_t(v[i] >> (8 * b));
}
// A Vendor_Defined (type 0x0F) message on SOP with `n` data objects. Header bit 8 is the sender's power role
// on SOP: `src` true = from the source (the charger), false = from the sink (the phone behind the K2).
static void vdm(Engine& e, std::uint32_t* v, unsigned n, Sop sop = Sop::Sop, bool src = true) {
    put(v, n);
    e.on_message(sop, std::uint16_t((n << 12) | (src ? 1u << 8 : 0u) | (2u << 6) | 0x0F), {g_buf, n * 4}, 1);
}

// PD page rows are 22 characters wide: "SVID xxxx <name>", "VID xxxx <vendor>" and the Apple model.
static void test_names_fit_a_row() {
    constexpr unsigned kRow = 22;
    for (unsigned id = 0; id <= 0xFFFF; id++) {
        const char* n = svid_name(std::uint16_t(id));
        if (n) CHECK_EQ(5 + 4 + 1 + std::strlen(n) <= kRow, 1);
        const char* v = vendor_name(std::uint16_t(id));
        if (v) CHECK_EQ(4 + 4 + 1 + std::strlen(v) <= kRow, 1);
        const char* m = apple_model(std::uint16_t(id));
        if (m) CHECK_EQ(std::strlen(m) <= kRow, 1);
    }
}

static void test_names() {
    CHECK_STR(svid_name(0x2717), "Xiaomi");
    CHECK_STR(svid_name(0x12D1), "Huawei");
    CHECK_STR(svid_name(0x22D9), "OPPO");
    CHECK_STR(svid_name(0x2D95), "Vivo");
    CHECK_STR(svid_name(0x05AC), "Apple");
    CHECK_STR(svid_name(0xFF00), "PD SID");
    CHECK_STR(svid_name(0xFF01), "DisplayPort");
    CHECK_STR(svid_name(0x2E87), "Cuktech");
    CHECK_STR(svid_name(0x2B16), "UGREEN");
    CHECK_STR(svid_name(0x5FC9), "UFCS");
    CHECK_STR(svid_name(0x0451), "TI");       // falls back to the cable-vendor table
    CHECK_STR(svid_name(0x1234), "Unknown");
    CHECK_STR(vendor_name(0x2109), "VIA Labs");
    CHECK_STR(vendor_name(0x2095), "CE Link");
    CHECK_STR(vendor_name(0x1234), "Unknown");
}

static void test_apple_model() {
    CHECK_STR(apple_model(0x710D), "iPad Air 5");
    CHECK_STR(apple_model(0x0707), "iPad Air");
    CHECK_STR(apple_model(0x711D), "iPad Air");
    CHECK_STR(apple_model(0x7309), "MacBook Pro 16");
    CHECK_STR(apple_model(0x7323), "MacBook Air 13");
    CHECK_STR(apple_model(0x7512), "iPhone 15 Pro");
    CHECK_STR(apple_model(0x0159), "iPhone 17e");
    CHECK_STR(apple_model(0x751F), "iPhone 18 Pro");
    CHECK_STR(apple_model(0x7522), "iPhone 18 PM");
    CHECK_EQ(apple_model(0x7777) == nullptr, true);
    CHECK_EQ(apple_model(0x0000) == nullptr, true);
    CHECK_EQ(apple_model(0xFFFF) == nullptr, true);
    // The table has 44 entries: 12 iPad, 13 Mac, 19 iPhone.
    int n = 0;
    for (unsigned pid = 0; pid <= 0xFFFF; pid++) n += apple_model(std::uint16_t(pid)) != nullptr;
    CHECK_EQ(n, 44);
}

static void test_identity_apple() {
    Engine e;
    std::uint32_t v[4] = {0xFF008041, 0x400005AC, 0x00000000, (0x710Du << 16) | 0x0100};
    vdm(e, v, 4);
    const Identity& id = e.vdm.identity;
    CHECK_EQ(id.valid, true);
    CHECK_EQ(id.vid, 0x05AC);
    CHECK_EQ(id.pid, 0x710D);
    CHECK_EQ(id.bcd, 0x0100);
    CHECK_STR(id.vendor, "Apple");
    CHECK_STR(id.model, "iPad Air 5");
    CHECK_EQ(id.id_header, 0x400005ACu);
    CHECK_EQ(id.num_vdos, 4);
    // Apple with an ignored PID gets no model.
    std::uint32_t w[4] = {0xFF008041, 0x400005AC, 0, (0xFFFFu << 16)};
    vdm(e, w, 4);
    CHECK_EQ(e.vdm.identity.model == nullptr, true);
    CHECK_EQ(e.vdm.identity.pid, 0xFFFF);
}

static void test_identity_other_vendor_and_nonack() {
    Engine e;
    std::uint32_t req[1] = {0xFF008001};     // cmd type 0 = REQ: the source asks us, not an answer
    vdm(e, req, 1);
    CHECK_EQ(e.vdm.identity.valid, false);
    CHECK_EQ(e.vdm.requests, 1);
    std::uint32_t nak[1] = {0xFF0080C1};     // cmd type 3 = BUSY
    vdm(e, nak, 1);
    CHECK_EQ(e.vdm.identity.valid, false);
    std::uint32_t v[4] = {0xFF008041, 0x00002717, 0, (0x1234u << 16)};
    vdm(e, v, 4);
    CHECK_EQ(e.vdm.identity.valid, true);
    CHECK_EQ(e.vdm.identity.model == nullptr, true);
    CHECK_STR(e.vdm.identity.vendor, "Xiaomi");   // the SVID table is tried before the cable-vendor VID table
}

static void test_svids() {
    Engine e;
    std::uint32_t v[3] = {0xFF008042, 0x2717FF01, 0x05AC0000};  // three SVIDs, terminated by 0000
    vdm(e, v, 3);
    CHECK_EQ(e.vdm.num_svids, 3);
    CHECK_EQ(e.vdm.svids[0], 0x2717);
    CHECK_EQ(e.vdm.svids[1], 0xFF01);
    CHECK_EQ(e.vdm.svids[2], 0x05AC);
    // Two VDOs, the last SVID slot used, no terminator: four SVIDs.
    std::uint32_t w[3] = {0xFF008042, 0x12D122D9, 0x2D952E87};
    vdm(e, w, 3);
    CHECK_EQ(e.vdm.num_svids, 4);
    CHECK_EQ(e.vdm.svids[3], 0x2E87);
}

static void test_modes() {
    Engine e;
    std::uint32_t v[3] = {0xFF018043, 0x000C0045, 0x00000085};  // DisplayPort, two mode VDOs
    vdm(e, v, 3);
    CHECK_EQ(e.vdm.modes_svid, 0xFF01);
    CHECK_EQ(e.vdm.num_modes, 2);
    CHECK_EQ(e.vdm.modes[0], 0x000C0045u);
    CHECK_EQ(e.vdm.modes[1], 0x00000085u);
}

static void test_cuktech_and_xiaomi() {
    Engine e;
    std::uint32_t c[3] = {0x2E878041, 0x30383041, 0x424E0000};
    vdm(e, c, 3);
    CHECK_EQ(e.vdm.cuktech, true);
    Engine f;
    std::uint32_t c2[3] = {0x2E878041, 0x30383042, 0x424E0000};   // wrong signature
    vdm(f, c2, 3);
    CHECK_EQ(f.vdm.cuktech, false);
    Engine x;
    std::uint32_t xm[2] = {0x27170003, 0x00000000};               // Xiaomi proprietary command 3
    vdm(x, xm, 2);
    CHECK_EQ(x.vdm.xiaomi, true);
    CHECK_EQ(x.vdm.xiaomi_cmd, 3);
    std::uint32_t no[1] = {0x27170009};                           // 9 is outside 1..8
    Engine y;
    vdm(y, no, 1);
    CHECK_EQ(y.vdm.xiaomi, false);
}

static void test_other_sop_ignored() {
    Engine e;
    std::uint32_t v[4] = {0xFF008041, 0x400005AC, 0, (0x710Du << 16)};
    vdm(e, v, 4, Sop::SopP);                  // cable answers are the e-marker reader's job
    CHECK_EQ(e.vdm.identity.valid, false);
    e.on_message(Sop::Sop, 0x0F | (4u << 12), {g_buf, 3}, 1);  // truncated payload
    CHECK_EQ(e.vdm.identity.valid, false);
}

static void test_product_type_and_role() {
    // ID header: bit 31 USB host, bit 30 USB device, 29:27 UFP type, 25:23 DFP type.
    CHECK_STR(id_product_type(0x400005ACu), "");                       // undefined (the Apple sample)
    CHECK_STR(id_product_type(0x40000000u | (2u << 27)), "Peripheral");
    CHECK_STR(id_product_type(0x40000000u | (1u << 27)), "Hub");
    CHECK_STR(id_product_type(3u << 27), "Power bank");
    CHECK_STR(id_product_type(0x80000000u | (3u << 23)), "Charger");   // DFP power brick
    CHECK_STR(id_product_type(0x80000000u | (2u << 23)), "Host");
    CHECK_STR(id_product_type(1u << 23), "Hub");
    CHECK_STR(id_usb_role(0xC0000000u), "Host+Device");
    CHECK_STR(id_usb_role(0x80000000u), "Host");
    CHECK_STR(id_usb_role(0x40000000u), "Device");
    CHECK_STR(id_usb_role(0u), "");
}

// Passive inline: both partners answer on SOP. The source's answers (header bit 8 set) fill `vdm`; the sink's
// (bit 8 clear) fill `sink_vdm` and never overwrite the source's identity, SVIDs or modes.
static void test_sink_answers_stay_on_the_sink_side() {
    Engine e;
    std::uint32_t src_id[4] = {0xFF008041, 0x00002B01, 0, 0};          // charger: Discover Identity ACK
    vdm(e, src_id, 4);
    std::uint32_t src_sv[2] = {0xFF008042, 0x27170000};                // charger lists SVID 0x2717
    vdm(e, src_sv, 2);
    std::uint32_t snk_id[4] = {0xFF008041, 0x000018D1, 0, (0x4EE1u << 16)};  // phone: its own Discover Identity ACK
    vdm(e, snk_id, 4, Sop::Sop, false);
    std::uint32_t snk_sv[2] = {0xFF008042, 0xFF010000};
    vdm(e, snk_sv, 2, Sop::Sop, false);
    CHECK_EQ(e.vdm.identity.valid, true);
    CHECK_EQ(e.vdm.identity.vid, 0x2B01);                              // the charger's, not the phone's
    CHECK_EQ(e.vdm.num_svids, 1);
    CHECK_EQ(e.vdm.svids[0], 0x2717);
    CHECK_EQ(e.sink_vdm.identity.valid, true);
    CHECK_EQ(e.sink_vdm.identity.vid, 0x18D1);
    CHECK_EQ(e.sink_vdm.identity.pid, 0x4EE1);
    CHECK_STR(e.sink_vdm.identity.vendor, "Google");
    CHECK_EQ(e.sink_vdm.num_svids, 1);
    CHECK_EQ(e.sink_vdm.svids[0], 0xFF01);
    // A vendor flag set by the sink is the sink's, not the charger's.
    Engine x;
    std::uint32_t xm[2] = {0x27170003, 0x00000000};
    vdm(x, xm, 2, Sop::Sop, false);
    CHECK_EQ(x.vdm.xiaomi, false);
    CHECK_EQ(x.sink_vdm.xiaomi, true);
    // A new charger clears both sides.
    e.reset_source();
    CHECK_EQ(e.vdm.identity.valid, false);
    CHECK_EQ(e.sink_vdm.identity.valid, false);
}

int main() {
    test_sink_answers_stay_on_the_sink_side();
    test_product_type_and_role();
    test_names();
    test_names_fit_a_row();
    test_apple_model();
    test_identity_apple();
    test_identity_other_vendor_and_nonack();
    test_svids();
    test_modes();
    test_cuktech_and_xiaomi();
    test_other_sop_ignored();
    return test_report("test_pd_vdm");
}
