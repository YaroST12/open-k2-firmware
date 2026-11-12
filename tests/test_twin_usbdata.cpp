// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the USB data properties of a twin (k2/twin/usbdata.hpp): the declared block read from a device alone
// (Discover Identity product-type VDOs, SVIDs, mode VDOs), the observed block sniffed inline (Enter_USB, Enter_Mode, DP
// Configure), their decoders and their wording. Message words are written out by hand from what the K2 sees on the wire.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "k2/pd/usb_link.hpp"
#include "k2/twin/learn.hpp"
#include "k2/twin/library.hpp"
#include "k2/twin/usbdata.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::twin;

namespace {

std::vector<std::uint8_t> g_pl;
std::uint16_t hdr(unsigned ndo, unsigned type) { return std::uint16_t((ndo << 12) | (2u << 6) | type); }
void feed(usblink::Classifier& c, unsigned type, std::initializer_list<std::uint32_t> v) {
    g_pl.clear();
    for (std::uint32_t w : v)
        for (int b = 0; b < 4; b++) g_pl.push_back(std::uint8_t(w >> (8 * b)));
    c.on_message(usblink::Sop::Sop, hdr(unsigned(v.size()), type), {g_pl.data(), g_pl.size()});
}
void vdm(usblink::Classifier& c, std::initializer_list<std::uint32_t> v) { feed(c, 0x0F, v); }
void accept(usblink::Classifier& c) { feed(c, 3, {}); }

// ID header (bits 31:23 = id_hi): bit 30 device capable, 29:27 UFP type 1 hub, 26 modal; a dock.
constexpr std::uint32_t kIdDock = 0x4C000000u | 0x17EF;
constexpr std::uint16_t kIdDockHi = std::uint16_t(kIdDock >> 23);
// UFP VDO: 27:24 device capability (b0 USB2, b1 billboard, b2 USB3.2, b3 USB4), 5:3 alt modes, 2:0 highest speed.
constexpr std::uint32_t kUfpTbt4Dock = 0x6D800000u | (1u << 3) | 3;   // USB4 capable, TBT3 alt mode, code 3 (40G)
constexpr std::uint32_t kUfpGen4 = 0x6D800000u | 4;                   // code 100b: USB4 Gen4 (80G)
constexpr std::uint32_t kUfpGen2 = 0x65800000u | 2;                   // USB 3.2 Gen2 10G
constexpr std::uint32_t kEuUsb4Gen3 = (2u << 28) | (3u << 21);
constexpr std::uint32_t kEuUsb4Gen4 = (2u << 28) | (4u << 21);
constexpr std::uint32_t kTbtEnterReq = 0x8087A104u, kTbtEnterAck = 0x8087A144u;
constexpr std::uint32_t kDpEnterReq = 0xFF01A104u, kDpEnterAck = 0xFF01A144u, kDpConfReq = 0xFF01A111u, kDpConfAck = 0xFF01A151u;

Twin phone() {
    Twin t;
    t.vid = 0x2717;
    t.pid = 0x1234;
    t.id_hi = 0x0A0;
    t.pd_rev = 2;
    t.num_pdos = 2;
    t.pdos[0] = 0x3601912C;
    t.pdos[1] = 0xC0DC213C;
    return t;
}

void test_vdo_decoders() {
    {  // a USB4 dock: UFP VDO
        Declared d;
        d.n_pt = 1;
        d.pt[0] = kUfpTbt4Dock;
        const UfpVdo u = ufp_vdo(kIdDockHi, d);
        CHECK_EQ(u.present, true);
        CHECK_EQ(int(u.cap), 0xD);
        CHECK_EQ(int(u.alt), 1);
        CHECK_EQ(int(u.speed), 3);
        CHECK_EQ(u.usb4(), true);
        CHECK_EQ(u.billboard(), false);
        CHECK_EQ(dfp_vdo(kIdDockHi, d).present, false);
    }
    {  // a laptop-like host: DFP VDO only (DFP type 2 host, no UFP type), host capability USB2 + USB3.2 + USB4, port 1
        Declared d;
        d.n_pt = 1;
        d.pt[0] = 0x67800000u | 1;
        const std::uint16_t id_hi = std::uint16_t((0x81000000u | 0x17EF) >> 23);  // host capable, DFP type 2
        CHECK_EQ(ufp_vdo(id_hi, d).present, false);
        const DfpVdo f = dfp_vdo(id_hi, d);
        CHECK_EQ(f.present, true);
        CHECK_EQ(int(f.cap), 7);
        CHECK_EQ(int(f.port), 1);
    }
    {  // hub with both: the DFP VDO follows the UFP VDO
        Declared d;
        d.n_pt = 2;
        d.pt[0] = kUfpGen2;
        d.pt[1] = 0x63800000u | 2;
        const std::uint16_t id_hi = std::uint16_t((0xCC000000u | 0x17EF | (1u << 23)) >> 23);  // host+device, hub, DFP hub
        CHECK_EQ(ufp_vdo(id_hi, d).present, true);
        CHECK_EQ(int(dfp_vdo(id_hi, d).port), 2);
    }
    {  // UFP VDO: VCONN and VBUS needs (bits 7, 6)
        Declared d;
        d.n_pt = 1;
        d.pt[0] = kUfpGen2 | (1u << 7) | (1u << 6) | (3u << 8);
        const UfpVdo u = ufp_vdo(kIdDockHi, d);
        CHECK_EQ(u.vconn_required, true);
        CHECK_EQ(u.vbus_required, true);
        CHECK_EQ(int(u.vconn_power), 3);
    }
    {  // DisplayPort capabilities: UFP_D capable, receptacle, pin assignments C and D on the UFP side
        const DpCaps c = dp_caps(0x000C0045u);
        CHECK_EQ(int(c.port), 1);
        CHECK_EQ(c.ufp_d(), true);
        CHECK_EQ(c.dfp_d(), false);
        CHECK_EQ(c.receptacle, true);
        CHECK_EQ(c.usb2_unused, false);
        CHECK_EQ(int(c.ufp_pins), 0x0C);
        CHECK_EQ(int(c.dfp_pins), 0);
        CHECK_EQ(c.four_lane(), true);   // pin assignment C
        const DpCaps d = dp_caps(0x00000C83u);  // both roles, USB 2.0 signalling not used, DFP pins C and D
        CHECK_EQ(int(d.port), 3);
        CHECK_EQ(d.usb2_unused, true);
        CHECK_EQ(int(d.dfp_pins), 0x0C);
    }
    {  // Thunderbolt mode word: alt mode id in 15:0, adapter bit 16
        const TbtMode m = tbt_mode(0x00010001u);
        CHECK_EQ(int(m.alt_mode), 1);
        CHECK_EQ(m.adapter, true);
        CHECK_EQ(tbt_mode(1).adapter, false);
    }
}

void test_enter_usb_decode() {
    const std::uint32_t w = kEuUsb4Gen3 | (1u << 16) | (1u << 15) | (1u << 14) | (1u << 13) | (1u << 19) | (2u << 17);
    const EnterUsb e = enter_usb(w);
    CHECK_EQ(int(e.mode), 2);
    CHECK_EQ(int(e.cable_speed), 3);
    CHECK_EQ(int(e.cable_type), 1);
    CHECK_EQ(int(e.cable_current), 2);
    CHECK_EQ(e.pcie, true);
    CHECK_EQ(e.dp, true);
    CHECK_EQ(e.tbt, true);
    CHECK_EQ(e.host_present, true);
    CHECK_EQ(enter_usb(0).tbt, false);
    // the wording comes from the same classifier the Home screen uses
    usblink::Link l = enter_usb_link(kEuUsb4Gen3);
    CHECK_EQ(int(l.family), int(usblink::Family::Usb4));
    CHECK_EQ(l.mbps, 40000u);
    l = enter_usb_link(kEuUsb4Gen4);  // cable speed 100b: USB4 Gen4, 80G
    CHECK_EQ(int(l.family), int(usblink::Family::Usb4v2));
    CHECK_EQ(l.mbps, 80000u);
    l = enter_usb_link((1u << 28) | (2u << 21));
    CHECK_EQ(int(l.family), int(usblink::Family::Usb32));
    CHECK_EQ(l.mbps, 10000u);
    CHECK_EQ(int(enter_usb_link(0).family), int(usblink::Family::Usb2));
    char b[40];
    enter_usb_text(kEuUsb4Gen3 | (1u << 14), b);
    CHECK_STR(b, "USB4 40G");
    enter_usb_text(kEuUsb4Gen4, b);
    CHECK_STR(b, "USB4v2 80G");
}

void test_declared_capture() {
    pd::Identity id;
    id.valid = true;
    id.vid = 0x17EF;
    id.pid = 0x3030;
    id.id_header = kIdDock;
    id.vdos = {0xFF00A041u, kIdDock, 0, 0x30300001u, kUfpTbt4Dock, 0x63800000u | 2};
    id.num_vdos = 6;
    pd::IdStatus st;
    st.state = pd::IdState::Done;
    st.num_sink_pdos = 1;
    st.sink_pdos[0] = 0x3601912C;
    st.num_modes = 2;
    st.mode_svid = {0xFF01, 0x8087};
    st.mode_vdo = {0x000C0045u, 0x00000001u};
    const std::uint16_t svids[7] = {0xFF01, 0x8087, 0x2717, 0x1111, 0x2222, 0x3333, 0x4444};
    Twin t;
    CHECK_EQ(from_identify(id, st, 2, svids, t), true);
    CHECK_EQ(t.decl.any(), true);
    CHECK_EQ(int(t.decl.n_pt), 2);
    CHECK_EQ(t.decl.pt[0], kUfpTbt4Dock);
    CHECK_EQ(t.decl.pt[1], 0x63800000u | 2);
    CHECK_EQ(int(t.decl.n_svids), 6);   // the first six
    CHECK_EQ(int(t.decl.svids[0]), 0xFF01);
    CHECK_EQ(int(t.decl.svids[5]), 0x3333);
    CHECK_EQ(int(t.decl.n_modes), 2);
    CHECK_EQ(int(t.decl.mode_svid[1]), 0x8087);
    CHECK_EQ(t.decl.mode_vdo[0], 0x000C0045u);
    // The old call (no SVIDs) still works and learns the same identity and PDOs, with no declared SVIDs
    Twin u;
    CHECK_EQ(from_identify(id, st, 2, u), true);
    CHECK_EQ(int(u.decl.n_svids), 0);
    CHECK_EQ(int(u.decl.n_pt), 2);
    // The product-type VDOs of a device that answered with none (Pixel, laptop): an empty block, nothing invented
    id.num_vdos = 4;
    st.num_modes = 0;
    Twin v;
    CHECK_EQ(from_identify(id, st, 2, {}, v), true);
    CHECK_EQ(v.decl.any(), false);
    // a count the engine never produces cannot overrun the block
    id.num_vdos = 9;
    Twin w;
    CHECK_EQ(from_identify(id, st, 2, {}, w), true);
    CHECK_EQ(int(w.decl.n_pt), 2);
}

void test_declared_wording() {
    // a Thunderbolt-capable dock: "can USB4 40G", DP and Thunderbolt advertised; TB4 only as an inference
    Declared d;
    d.n_pt = 1;
    d.pt[0] = kUfpTbt4Dock;
    d.n_svids = 2;
    d.svids = {0xFF01, 0x8087};
    usblink::Link l = declared_link(kIdDockHi, d);
    CHECK_EQ(int(l.family), int(usblink::Family::Usb4));
    CHECK_EQ(l.mbps, 40000u);
    CHECK_EQ(int(l.conf), int(usblink::Confidence::Advertised));
    CHECK_EQ(l.dp_adv, true);
    CHECK_EQ(l.tbt_adv, true);
    CHECK_EQ(int(l.tbt), int(usblink::Tbt::None));   // advertised is never claimed as the link
    char b[64];
    declared_text(kIdDockHi, d, b);
    CHECK_STR(b, "USB4 40G, DP, TB");
    CHECK_STR(tb_class(l), "TB4 class");
    // 100b: Gen4 80G, the TB5 class
    d.pt[0] = kUfpGen4;
    l = declared_link(kIdDockHi, d);
    CHECK_EQ(int(l.family), int(usblink::Family::Usb4v2));
    CHECK_EQ(l.mbps, 80000u);
    declared_text(kIdDockHi, d, b);
    CHECK_STR(b, "USB4v2 80G, DP, TB");
    CHECK_STR(tb_class(l), "TB5 class");
    // a 10G dock with no alternate mode
    Declared g;
    g.n_pt = 1;
    g.pt[0] = kUfpGen2;
    declared_text(kIdDockHi, g, b);
    CHECK_STR(b, "USB 3.2 Gen2 10G");
    CHECK_STR(tb_class(declared_link(kIdDockHi, g)), "");
    // USB4 40G without the 0x8087 SVID is not called Thunderbolt
    Declared h;
    h.n_pt = 1;
    h.pt[0] = 0x6D800000u | 3;
    CHECK_STR(tb_class(declared_link(kIdDockHi, h)), "");
    // nothing declared: "-"
    declared_text(0, Declared{}, b);
    CHECK_STR(b, "-");
    // a single mode VDO is what names the DP role and pins
    char c[64];
    dp_caps_text(dp_caps(0x000C0045u), c);
    CHECK_STR(c, "DP sink, pins C D");
    dp_caps_text(dp_caps(0x00000C83u), c);
    CHECK_STR(c, "DP source+sink, pins C D");
}

void test_observed() {
    // Advertised only: nothing to observe
    usblink::Classifier adv;
    adv.on_message(usblink::Sop::Sop, hdr(1, 0x0F), {});  // too short, ignored
    Observed o;
    CHECK_EQ(make_observed(adv.link(), adv.raw(), 0x17EF, o), false);
    CHECK_EQ(o.valid, false);

    // USB4 entered with Enter_USB (host asked, dock accepted) plus Thunderbolt entered
    usblink::Classifier c;
    feed(c, 8, {kEuUsb4Gen3});
    accept(c);
    vdm(c, {kTbtEnterReq, 3u << 16});
    vdm(c, {kTbtEnterAck});
    CHECK_EQ(make_observed(c.link(), c.raw(), 0x17EF, o), true);
    CHECK_EQ(o.valid, true);
    CHECK_EQ(int(o.partner_vid), 0x17EF);
    CHECK_EQ(int(o.family), int(usblink::Family::Usb4));
    CHECK_EQ(int(o.tbt), int(usblink::Tbt::Tbt4));
    CHECK_EQ(o.mbps, 40000u);
    CHECK_EQ(bool(o.flags & kObsEnterUsb), true);
    CHECK_EQ(o.enter_usb, kEuUsb4Gen3);
    CHECK_EQ(bool(o.flags & kObsTbtEnter), true);
    CHECK_EQ(o.tbt_enter, 3u << 16);
    char b[64];
    observed_text(o, b);
    CHECK_STR(b, "USB4 40G TBT4");
    const usblink::Link l = observed_link(o);
    CHECK_EQ(int(l.conf), int(usblink::Confidence::Negotiated));
    CHECK_EQ(int(l.tbt), int(usblink::Tbt::Tbt4));

    // DP alt mode on 4 lanes (pin E): USB falls back to 2.0
    usblink::Classifier d;
    vdm(d, {kDpEnterReq});
    vdm(d, {kDpEnterAck});
    vdm(d, {kDpConfReq, (1u << 12) | 2});
    vdm(d, {kDpConfAck});
    Observed p;
    CHECK_EQ(make_observed(d.link(), d.raw(), 0x05AC, p), true);
    CHECK_EQ(int(p.dp_pin), 0x10);
    CHECK_EQ(bool(p.flags & kObsDp), true);
    CHECK_EQ(bool(p.flags & kObsDp4), true);
    observed_text(p, b);
    CHECK_STR(b, "USB 2.0 480M +DP 4-lane");

    // DP on 2 lanes (pin D) keeps USB 3
    usblink::Classifier e;
    feed(e, 8, {(1u << 28) | (2u << 21)});
    accept(e);
    vdm(e, {kDpEnterReq});
    vdm(e, {kDpEnterAck});
    vdm(e, {kDpConfReq, (1u << 11) | 2});
    vdm(e, {kDpConfAck});
    Observed q;
    CHECK_EQ(make_observed(e.link(), e.raw(), 0x05AC, q), true);
    CHECK_EQ(bool(q.flags & kObsDp2), true);
    observed_text(q, b);
    CHECK_STR(b, "USB 3.2 Gen2 10G +DP 2-lane");
    // Data_Reset puts the link back to USB 2.0 and clears the observed state: nothing negotiated any more
    feed(e, 14, {});
    CHECK_EQ(make_observed(e.link(), e.raw(), 0x05AC, q), false);
}

void test_record_v2() {
    // a twin with no USB blocks stays a version 1 record, byte for byte
    Twin a = phone();
    a.name = auto_name(a);
    std::array<std::uint8_t, kMaxFileBytes> b{};
    const std::size_t n1 = encode(a, 3, b);
    CHECK_EQ(int(b[3]), 1);
    CHECK_EQ(n1, kHeaderBytes + 8 + 2);

    // with the declared block: version 2, flag b1, a counted block
    Twin t = a;
    t.decl.n_pt = 2;
    t.decl.pt = {kUfpTbt4Dock, 0x63800002u};
    t.decl.n_svids = 3;
    t.decl.svids = {0xFF01, 0x8087, 0x2717, 0, 0, 0};
    t.decl.n_modes = 2;
    t.decl.mode_svid = {0xFF01, 0x8087};
    t.decl.mode_vdo = {0x000C0045u, 1};
    const std::size_t n2 = encode(t, 3, b);
    CHECK_EQ(int(b[3]), 2);
    CHECK_EQ(bool(b[kOffFlags] & kFlagDeclared), true);
    CHECK_EQ(n2, n1 + kDeclHeader + 2 * 4 + 3 * 2 + 2 * 6);
    std::uint8_t seq = 0;
    Twin r;
    CHECK_EQ(int(decode({b.data(), n2}, seq, r)), int(Dec::Ok));
    CHECK_EQ(int(r.decl.n_pt), 2);
    CHECK_EQ(r.decl.pt[0], kUfpTbt4Dock);
    CHECK_EQ(r.decl.pt[1], 0x63800002u);
    CHECK_EQ(int(r.decl.n_svids), 3);
    CHECK_EQ(int(r.decl.svids[2]), 0x2717);
    CHECK_EQ(int(r.decl.n_modes), 2);
    CHECK_EQ(r.decl.mode_vdo[0], 0x000C0045u);
    CHECK_EQ(r.obs.valid, false);

    // with both blocks and a Request, the largest record
    t.rdo = 0x1304B12C;
    t.rdo_pdo = 0x0002D12C;
    t.num_pdos = 7;
    for (std::size_t i = 2; i < 7; ++i) t.pdos[i] = 0x0001912C + std::uint32_t(i);
    t.obs.valid = true;
    t.obs.partner_vid = 0x17EF;
    t.obs.family = std::uint8_t(usblink::Family::Usb4);
    t.obs.tbt = std::uint8_t(usblink::Tbt::Tbt4);
    t.obs.flags = kObsEnterUsb | kObsTbtEnter | kObsDp;
    t.obs.dp_pin = 0x08;
    t.obs.mbps = 40000;
    t.obs.enter_usb = kEuUsb4Gen3;
    t.obs.tbt_enter = 3u << 16;
    t.decl.n_pt = 2;
    t.decl.n_svids = 6;
    t.decl.n_modes = 2;
    const std::size_t big = encode(t, 3, b);
    CHECK_EQ(big, kMaxFileBytes);
    CHECK_EQ(int(decode({b.data(), big}, seq, r)), int(Dec::Ok));
    CHECK_EQ(r.obs.valid, true);
    CHECK_EQ(int(r.obs.partner_vid), 0x17EF);
    CHECK_EQ(int(r.obs.tbt), int(usblink::Tbt::Tbt4));
    CHECK_EQ(r.obs.mbps, 40000u);
    CHECK_EQ(r.obs.enter_usb, kEuUsb4Gen3);
    CHECK_EQ(r.obs.tbt_enter, 3u << 16);
    CHECK_EQ(int(r.obs.dp_pin), 8);
    CHECK_EQ(r.rdo, 0x1304B12Cu);

    // an observed block alone
    Twin o = a;
    o.obs = t.obs;
    const std::size_t no = encode(o, 3, b);
    CHECK_EQ(no, n1 + kObsBytes);
    CHECK_EQ(int(decode({b.data(), no}, seq, r)), int(Dec::Ok));
    CHECK_EQ(r.obs.valid, true);
    CHECK_EQ(r.decl.any(), false);

    // a cut or padded record, a bad CRC or a bad count never decode
    CHECK_EQ(int(decode({b.data(), no - 1}, seq, r)) != int(Dec::Ok), true);
    std::array<std::uint8_t, kMaxFileBytes + 1> pad{};
    std::memcpy(pad.data(), b.data(), no);
    CHECK_EQ(int(decode({pad.data(), no + 1}, seq, r)), int(Dec::Invalid));
    // a version 1 record with a version 2 flag is damaged, not a new format
    std::array<std::uint8_t, kMaxFileBytes> v1{};
    const std::size_t nv1 = encode(a, 1, v1);
    v1[kOffFlags] |= kFlagObserved;
    detail::put16(&v1[nv1 - 2], bbstore::crc16(0xFFFF, {v1.data(), nv1 - 2}));
    CHECK_EQ(int(decode({v1.data(), nv1}, seq, r)), int(Dec::Invalid));
    // a version 3 record is refused, never guessed
    std::array<std::uint8_t, kMaxFileBytes> v3{};
    const std::size_t nv3 = encode(t, 1, v3);
    v3[3] = 3;
    CHECK_EQ(int(decode({v3.data(), nv3}, seq, r)), int(Dec::Version));
    // counts that overrun the block
    std::array<std::uint8_t, kMaxFileBytes> bad{};
    const std::size_t nb = encode(t, 1, bad);
    std::size_t at = kHeaderBytes + t.num_pdos * 4 + 8;   // the declared block starts here
    bad[at + 1] = 7;                                       // svid count 7 > 6
    detail::put16(&bad[nb - 2], bbstore::crc16(0xFFFF, {bad.data(), nb - 2}));
    CHECK_EQ(int(decode({bad.data(), nb}, seq, r)), int(Dec::Invalid));
}

void test_library_merge() {
    k2test::RamDisk disk{96};
    fs::Fs f{disk, 96};
    bool formatted;
    f.mount_or_format(formatted);
    Library lib(f);
    // learned alone first (declared), then sniffed inline: both kept in one twin
    Twin a = phone();
    a.decl.n_pt = 1;
    a.decl.pt[0] = kUfpGen2;
    Result r = lib.save(a);
    CHECK_EQ(int(r.err), int(Err::Ok));
    Twin s;
    s.vid = a.vid;
    s.pid = a.pid;
    s.rdo = 0x1304B12C;
    s.rdo_pdo = 0x0002D12C;
    s.obs.valid = true;
    s.obs.partner_vid = 0x17EF;
    s.obs.family = std::uint8_t(usblink::Family::Usb32);
    s.obs.mbps = 10000;
    r = lib.save(s);
    CHECK_EQ(int(r.err), int(Err::Ok));
    CHECK_EQ(r.existed, true);
    Twin g;
    CHECK_EQ(lib.get(r.seq, g), true);
    CHECK_EQ(g.decl.any(), true);
    CHECK_EQ(g.obs.valid, true);
    CHECK_EQ(g.has_request(), true);
    CHECK_EQ(int(g.num_pdos), 2);
    // the same sniff again changes nothing: nothing is written
    r = lib.save(s);
    CHECK_EQ(r.unchanged, true);
    // an Identify with no USB words (a phone that does not answer them) leaves the saved blocks alone
    Twin n = phone();
    r = lib.save(n);
    CHECK_EQ(r.unchanged, true);
    CHECK_EQ(lib.get(r.seq, g), true);
    CHECK_EQ(g.obs.valid, true);
    CHECK_EQ(g.decl.any(), true);
    // a new observation replaces the old one
    s.obs.mbps = 5000;
    r = lib.save(s);
    CHECK_EQ(r.unchanged, false);
    CHECK_EQ(lib.get(r.seq, g), true);
    CHECK_EQ(g.obs.mbps, 5000u);
}

}  // namespace

int main() {
    test_vdo_decoders();
    test_enter_usb_decode();
    test_declared_capture();
    test_declared_wording();
    test_observed();
    test_record_v2();
    test_library_merge();
    return test_report("test_twin_usbdata");
}
