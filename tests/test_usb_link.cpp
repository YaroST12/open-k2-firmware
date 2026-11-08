// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the USB link classifier (k2/pd/usb_link.hpp). Message words are written out from the USB PD 3.1
// tables (6.4.4.3.1 ID header, UFP/DFP/cable VDOs, 6.4.10 Enter_USB, 6.4.4.2 VDM header) rather than computed by
// the code under test; no on-wire dock capture exists in the repo yet (see the commit body).
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "k2/pd/usb_link.hpp"
#include "test_util.h"

using namespace k2::usblink;

namespace {

std::vector<std::uint8_t> g_pl;
std::uint16_t hdr(unsigned ndo, unsigned type) { return std::uint16_t((ndo << 12) | (2u << 6) | type); }

void feed(Classifier& c, Sop sop, unsigned type, std::initializer_list<std::uint32_t> v) {
    g_pl.clear();
    for (std::uint32_t w : v)
        for (int b = 0; b < 4; b++) g_pl.push_back(std::uint8_t(w >> (8 * b)));
    c.on_message(sop, hdr(unsigned(v.size()), type), {g_pl.data(), g_pl.size()});
}
void vdm(Classifier& c, Sop sop, std::initializer_list<std::uint32_t> v) { feed(c, sop, 0x0F, v); }
void ctl(Classifier& c, unsigned type, Sop sop = Sop::Sop) { feed(c, sop, type, {}); }

constexpr unsigned kAccept = 3, kReject = 4, kDataReset = 14, kNotSupported = 16, kEnterUsb = 8;

// Discover Identity ACK on SOP: header 0xFF00A041 (SVID 0xFF00, structured, VDM 1.0, ACK, cmd 1).
void identity(Classifier& c, Sop sop, std::uint32_t id_hdr, std::initializer_list<std::uint32_t> pt) {
    std::vector<std::uint32_t> w = {0xFF00A041u, id_hdr, 0, 0x00010000u};
    w.insert(w.end(), pt.begin(), pt.end());
    g_pl.clear();
    for (std::uint32_t x : w)
        for (int b = 0; b < 4; b++) g_pl.push_back(std::uint8_t(x >> (8 * b)));
    c.on_message(sop, hdr(unsigned(w.size()), 0x0F), {g_pl.data(), g_pl.size()});
}

// ID header words: bit 30 USB device capable, 29:27 UFP type (001 hub, 010 peripheral), 26 modal, 15:0 VID.
constexpr std::uint32_t kIdDockHub = 0x4C000000u | 0x17EF;      // device capable, hub, modal
constexpr std::uint32_t kIdHost = 0x81000000u | 0x17EF;         // host capable, DFP type 010 (host)
constexpr std::uint32_t kIdPassiveCable = 0x18000000u | 0x05AC; // UFP type 011 passive cable
constexpr std::uint32_t kIdActiveCable = 0x20000000u | 0x05AC;  // UFP type 100 active cable
// UFP VDO1: 31:29 ver 011, 27:24 device capability, 23:22 receptacle (10), 5:3 alt modes, 2:0 speed.
constexpr std::uint32_t kUfpTbt4Dock = 0x6D800000u | (1u << 3) | 3;  // USB2+USB3.2+USB4, TBT3 alt, Gen3
constexpr std::uint32_t kUfpGen1 = 0x65800000u | 1;                  // USB2+USB3.2, Gen1
constexpr std::uint32_t kUfpGen2 = 0x65800000u | 2;                  // USB2+USB3.2, Gen2
constexpr std::uint32_t kUfpUsb2 = 0x61800000u | 0;                  // USB2 only
constexpr std::uint32_t kUfpBillboard = 0x62800000u | 0;             // USB2 billboard only
// DFP VDO: 31:29 ver, 26:24 host capability (b0 USB2, b1 USB3.2, b2 USB4), 23:22 receptacle.
constexpr std::uint32_t kDfpUsb4 = 0x67800000u;
constexpr std::uint32_t kDfpUsb3 = 0x63800000u;
// Cable VDO1 low bits: 6:5 current 3 A (01 -> 0x20? spec 01b = 3A, 10b = 5A), 2:0 speed.
constexpr std::uint32_t kCableGen1 = 0x20 | 1, kCableGen2 = 0x20 | 2, kCableGen3 = 0x40 | 3, kCableGen4 = 0x40 | 4;
constexpr std::uint32_t kCableUsb2 = 0x20 | 0;

// Enter_USB data objects: 30:28 mode (0 USB2, 1 USB3.2, 2 USB4), 23:21 cable speed.
constexpr std::uint32_t kEuUsb4Gen3 = (2u << 28) | (3u << 21);  // 0x20600000
constexpr std::uint32_t kEuUsb4Gen4 = (2u << 28) | (4u << 21);  // 0x20800000
constexpr std::uint32_t kEuUsb32Gen1 = (1u << 28) | (1u << 21);  // 0x10200000
constexpr std::uint32_t kEuUsb32Gen2 = (1u << 28) | (2u << 21);  // 0x10400000
constexpr std::uint32_t kEuUsb2 = 0;

// Enter_Mode / Configure headers: SVID<<16 | structured 0x8000 | VDM 1.0 0x2000 | object position 1 | cmd type | cmd.
constexpr std::uint32_t kTbtEnterReq = 0x8087A104u, kTbtEnterAck = 0x8087A144u, kTbtEnterNak = 0x8087A184u;
constexpr std::uint32_t kDpConfReq = 0xFF01A111u, kDpConfAck = 0xFF01A151u;
constexpr std::uint32_t kDpEnterReq = 0xFF01A104u, kDpEnterAck = 0xFF01A144u;

void test_enter_usb() {
    {
        Classifier c;
        feed(c, Sop::Sop, kEnterUsb, {kEuUsb4Gen3});
        CHECK_EQ(int(c.link().family), int(Family::Unknown));  // not accepted yet: nothing negotiated
        ctl(c, kAccept);
        const Link l = c.link();
        CHECK_EQ(int(l.family), int(Family::Usb4));
        CHECK_EQ(l.mbps, 40000);
        CHECK_EQ(int(l.conf), int(Confidence::Negotiated));
        CHECK_EQ(int(l.source), int(Source::EnterUsb));
        CHECK_EQ(int(l.tbt), int(Tbt::None));
    }
    {
        Classifier c;
        feed(c, Sop::Sop, kEnterUsb, {kEuUsb4Gen4}); ctl(c, kAccept);
        CHECK_EQ(int(c.link().family), int(Family::Usb4v2));
        CHECK_EQ(c.link().mbps, 80000);
    }
    {
        Classifier c;
        feed(c, Sop::Sop, kEnterUsb, {kEuUsb32Gen1}); ctl(c, kAccept);
        CHECK_EQ(int(c.link().family), int(Family::Usb32));
        CHECK_EQ(c.link().mbps, 5000);
    }
    {
        Classifier c;
        feed(c, Sop::Sop, kEnterUsb, {kEuUsb32Gen2}); ctl(c, kAccept);
        CHECK_EQ(c.link().mbps, 10000);
    }
    {
        Classifier c;
        feed(c, Sop::Sop, kEnterUsb, {kEuUsb2}); ctl(c, kAccept);
        CHECK_EQ(int(c.link().family), int(Family::Usb2));
        CHECK_EQ(c.link().mbps, 480);
        CHECK_EQ(int(c.link().conf), int(Confidence::Negotiated));
    }
    {  // Reject / Not_Supported: the request does not count
        Classifier c;
        feed(c, Sop::Sop, kEnterUsb, {kEuUsb4Gen3}); ctl(c, kReject);
        CHECK_EQ(int(c.link().family), int(Family::Unknown));
        feed(c, Sop::Sop, kEnterUsb, {kEuUsb4Gen3}); ctl(c, kNotSupported);
        CHECK_EQ(int(c.link().family), int(Family::Unknown));
    }
    {  // an Accept to something else (no Enter_USB pending) changes nothing
        Classifier c;
        ctl(c, kAccept);
        CHECK_EQ(int(c.link().conf), int(Confidence::None));
    }
    {  // Enter_USB toward the cable plug is not the partner's link
        Classifier c;
        feed(c, Sop::SopP, kEnterUsb, {kEuUsb4Gen3}); ctl(c, kAccept, Sop::SopP);
        CHECK_EQ(int(c.link().conf), int(Confidence::None));
    }
}

void test_identity_partner() {
    {
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpTbt4Dock});
        const Link l = c.link();
        CHECK_EQ(int(l.family), int(Family::Usb4));
        CHECK_EQ(l.mbps, 40000);
        CHECK_EQ(int(l.conf), int(Confidence::Advertised));
        CHECK_EQ(int(l.source), int(Source::Identity));
        CHECK_EQ(int(l.tbt), int(Tbt::None));  // TBT alt mode flag is capability only
    }
    {
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpGen1});
        CHECK_EQ(int(c.link().family), int(Family::Usb32));
        CHECK_EQ(c.link().mbps, 5000);
        CHECK_EQ(int(c.link().tbt), int(Tbt::None));
    }
    {
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpGen2});
        CHECK_EQ(c.link().mbps, 10000);
    }
    {
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpUsb2});
        CHECK_EQ(int(c.link().family), int(Family::Usb2));
        CHECK_EQ(c.link().mbps, 480);
    }
    {
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpBillboard});
        CHECK_EQ(c.link().billboard, true);
    }
    {  // a USB4 Gen4 dock is USB4v2; the TBT alt flag stays capability, no tag
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {0x6D800000u | (1u << 3) | 4});
        CHECK_EQ(int(c.link().family), int(Family::Usb4v2));
        CHECK_EQ(c.link().mbps, 80000);
        CHECK_EQ(int(c.link().tbt), int(Tbt::None));
    }
    {  // host side only: DFP VDO gives the family, not a rate (USB4 hosts do at least 40G)
        Classifier c;
        identity(c, Sop::Sop, kIdHost, {kDfpUsb4});
        CHECK_EQ(int(c.link().family), int(Family::Usb4));
        CHECK_EQ(c.link().mbps, 40000);
        Classifier d;
        identity(d, Sop::Sop, kIdHost, {kDfpUsb3});
        CHECK_EQ(int(d.link().family), int(Family::Usb32));
        CHECK_EQ(d.link().mbps, 5000);
    }
    {  // both ends seen: the lower ceiling wins
        Classifier c;
        identity(c, Sop::Sop, kIdHost, {kDfpUsb3});
        identity(c, Sop::Sop, kIdDockHub, {kUfpTbt4Dock});
        CHECK_EQ(c.link().mbps, 5000);
    }
}

void test_cable_clamp() {
    {  // 40G dock over a USB 3.2 Gen1 passive cable
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpTbt4Dock});
        identity(c, Sop::SopP, kIdPassiveCable, {kCableGen1});
        const Link l = c.link();
        CHECK_EQ(int(l.family), int(Family::Usb32));
        CHECK_EQ(l.mbps, 5000);
        CHECK_EQ(l.cable_limited, true);
        CHECK_EQ(int(l.source), int(Source::Cable));
        CHECK_EQ(int(l.tbt), int(Tbt::None));  // TBT needs a Gen3 cable
    }
    {  // charge-only (USB2) cable
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpTbt4Dock});
        identity(c, Sop::SopP, kIdPassiveCable, {kCableUsb2});
        CHECK_EQ(int(c.link().family), int(Family::Usb2));
        CHECK_EQ(c.link().cable_limited, true);
    }
    {  // Gen2 passive cable: USB4 dock gets 20G
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpTbt4Dock});
        identity(c, Sop::SopP, kIdPassiveCable, {kCableGen2});
        CHECK_EQ(int(c.link().family), int(Family::Usb4));
        CHECK_EQ(c.link().mbps, 20000);
    }
    {  // matching 40G cable does not limit
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpTbt4Dock});
        identity(c, Sop::SopP, kIdPassiveCable, {kCableGen3});
        CHECK_EQ(c.link().mbps, 40000);
        CHECK_EQ(c.link().cable_limited, false);
    }
    {  // an 80G cable with a 40G dock: still 40G, dock is the limit
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpTbt4Dock});
        identity(c, Sop::SopPP, kIdActiveCable, {kCableGen4, 0});
        CHECK_EQ(c.link().mbps, 40000);
        CHECK_EQ(c.link().cable_limited, false);
    }
    {  // a cable alone claims no link: it only caps one (real case: 240 W USB4 cable on a 10G dock)
        Classifier c;
        identity(c, Sop::SopP, kIdPassiveCable, {kCableGen3});
        CHECK_EQ(c.link().mbps, 0);
    }
    {  // real dock (VID 042B PID 0211): Alternate Mode Adapter, AMA VDO SS = Gen1+Gen2, DP entered
        Classifier c;
        identity(c, Sop::Sop, 0x6E40042Bu, {0x0000000Au});  // cert, product filled by the helper
        identity(c, Sop::SopP, kIdPassiveCable, {kCableGen3});
        const Link l = c.link();
        CHECK_EQ(l.mbps, 10000);
        CHECK_EQ(l.cable_limited, false);
    }
    {  // the negotiated Enter_USB speed already includes the cable the host chose
        Classifier c;
        identity(c, Sop::SopP, kIdPassiveCable, {kCableGen1});
        feed(c, Sop::Sop, kEnterUsb, {kEuUsb32Gen1}); ctl(c, kAccept);
        CHECK_EQ(c.link().mbps, 5000);
        CHECK_EQ(int(c.link().conf), int(Confidence::Negotiated));
    }
}

void test_thunderbolt() {
    {  // Enter_Mode 0x8087 ACKed, cable speed field 3 (TBT3 40G cable) -> Thunderbolt 3
        Classifier c;
        vdm(c, Sop::Sop, {kTbtEnterReq, (3u << 16) | (1u << 24)});
        vdm(c, Sop::Sop, {kTbtEnterAck});
        const Link l = c.link();
        CHECK_EQ(int(l.tbt), int(Tbt::Tbt3));
        CHECK_EQ(l.mbps, 40000);  // cable speed 3 = 20 Gbps per lane, two bonded lanes = 40G
        CHECK_EQ(int(l.conf), int(Confidence::Negotiated));
        CHECK_EQ(int(l.source), int(Source::EnterTbt));
    }
    {  // NAK: not entered
        Classifier c;
        vdm(c, Sop::Sop, {kTbtEnterReq, 3u << 16});
        vdm(c, Sop::Sop, {kTbtEnterNak});
        CHECK_EQ(int(c.link().tbt), int(Tbt::None));
    }
    {  // Enter_Mode TBT plus accepted Enter_USB USB4 -> Thunderbolt 4
        Classifier c;
        feed(c, Sop::Sop, kEnterUsb, {kEuUsb4Gen3}); ctl(c, kAccept);
        vdm(c, Sop::Sop, {kTbtEnterReq, 3u << 16}); vdm(c, Sop::Sop, {kTbtEnterAck});
        CHECK_EQ(int(c.link().tbt), int(Tbt::Tbt4));
        CHECK_EQ(int(c.link().family), int(Family::Usb4));
    }
    {  // 80G with TBT -> Thunderbolt 5
        Classifier c;
        feed(c, Sop::Sop, kEnterUsb, {kEuUsb4Gen4}); ctl(c, kAccept);
        vdm(c, Sop::Sop, {kTbtEnterReq, 3u << 16}); vdm(c, Sop::Sop, {kTbtEnterAck});
        CHECK_EQ(int(c.link().tbt), int(Tbt::Tbt5));
    }
    {  // Discover SVIDs ACK listing 0x8087 only advertises
        Classifier c;
        vdm(c, Sop::Sop, {0xFF00A042u, 0x80870000u});
        CHECK_EQ(int(c.link().tbt), int(Tbt::None));
        CHECK_EQ(int(c.link().conf), int(Confidence::None));
    }
}

void test_dp_alt() {
    {  // DP alt mode with pin assignment E (4 lanes): SuperSpeed lanes are gone, USB 2.0 only
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpGen2});
        vdm(c, Sop::Sop, {kDpEnterReq}); vdm(c, Sop::Sop, {kDpEnterAck});
        vdm(c, Sop::Sop, {kDpConfReq, (1u << 12) | 2}); vdm(c, Sop::Sop, {kDpConfAck});
        const Link l = c.link();
        CHECK_EQ(l.dp_4lane, true);
        CHECK_EQ(int(l.family), int(Family::Usb2));
        CHECK_EQ(l.mbps, 480);
        CHECK_EQ(int(l.source), int(Source::DpAlt));
    }
    {  // pin assignment D (2 lanes) keeps USB 3
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpGen2});
        vdm(c, Sop::Sop, {kDpEnterReq}); vdm(c, Sop::Sop, {kDpEnterAck});
        vdm(c, Sop::Sop, {kDpConfReq, (1u << 11) | 2}); vdm(c, Sop::Sop, {kDpConfAck});
        CHECK_EQ(c.link().dp_4lane, false);
        CHECK_EQ(c.link().mbps, 10000);
    }
}

// Dock VID 0x042B / PID 0x0211 sniffed through the K2: Discover SVIDs -> DP + TBT, Discover Modes for both,
// DP Enter_Mode / Status / Configure, no TBT Enter_Mode. The laptop kernel linked it as USB 3.2 Gen2x1 10G.
// The exact on-wire identity words were not captured; the vectors below follow the PD 3.1 tables.
void test_dock_dp_not_tbt() {
    for (const bool ufp : {true, false}) {
        Classifier c;
        if (ufp) identity(c, Sop::Sop, kIdDockHub, {kUfpGen2});
        else identity(c, Sop::Sop, 0x4C000000u | 0x042B, {});
        vdm(c, Sop::Sop, {0xFF00A042u, 0xFF018087u});  // SVIDs: DP + TBT
        vdm(c, Sop::Sop, {kDpEnterReq}); vdm(c, Sop::Sop, {kDpEnterAck});
        vdm(c, Sop::Sop, {kDpConfReq, (1u << 11) | 2}); vdm(c, Sop::Sop, {kDpConfAck});  // pin D, 2 lanes
        const Link l = c.link();
        CHECK_EQ(int(l.tbt), int(Tbt::None));  // TBT advertised, never entered
        CHECK_EQ(l.dp, true);
        CHECK_EQ(int(l.source), int(Source::DpAlt));
        CHECK_EQ(int(l.conf), int(Confidence::Negotiated));
        char b[40];
        format(l, b);
        if (ufp) { CHECK_EQ(l.mbps, 10000); CHECK_STR(b, "USB 3.2 Gen2 10G +DP (neg)"); }
        else { CHECK_EQ(l.mbps, 0); CHECK_STR(b, "DP alt (neg)"); }
    }
    {  // cable clamps the speed under the DP session
        Classifier c;
        identity(c, Sop::Sop, kIdDockHub, {kUfpGen2});
        identity(c, Sop::SopP, kIdPassiveCable, {kCableGen1});
        vdm(c, Sop::Sop, {kDpEnterReq}); vdm(c, Sop::Sop, {kDpEnterAck});
        char b[40];
        format(c.link(), b);
        CHECK_STR(b, "USB 3.2 Gen1 5G +DP (neg)");
    }
}

void test_tbt_advertised_without_speed() {
    Classifier c;
    vdm(c, Sop::Sop, {0xFF00A042u, 0x80870000u});
    char b[40];
    format(c.link(), b);
    CHECK_EQ(c.link().mbps, 0);
    CHECK_STR(b, "--");  // advertised TBT alone claims nothing
}

// Pixel phone locked, behind a TB3 dock and a TB4 cable: the phone rejects USB, nothing is entered. The link
// reads as the advertised 10G with no TB tag (hardware report: it wrongly read "3.2 Gen2 10G TB3/4").
void test_locked_phone_no_tbt() {
    Classifier c;
    identity(c, Sop::Sop, kIdDockHub, {kUfpGen2});
    vdm(c, Sop::Sop, {0xFF00A042u, 0xFF018087u});  // SVIDs: DP + TBT
    const Link l = c.link();
    CHECK_EQ(int(l.tbt), int(Tbt::None));
    CHECK_EQ(int(l.family), int(Family::Usb32));
    CHECK_EQ(l.mbps, 10000);
    CHECK_EQ(int(l.conf), int(Confidence::Advertised));
    char b[40];
    format_ui(l, b);
    CHECK_STR(b, "3.2 Gen2 10G");
    vdm(c, Sop::Sop, {kTbtEnterReq, 3u << 16}); vdm(c, Sop::Sop, {kTbtEnterAck});  // really entered: now TB
    CHECK_EQ(int(c.link().tbt) != int(Tbt::None), true);
}

// USB / Alt split: speed alone, alt entered (green), advertised (amber), vendor SVIDs listed.
void test_split_usb_alt() {
    Classifier c;
    identity(c, Sop::Sop, kIdDockHub, {kUfpGen2});
    vdm(c, Sop::Sop, {0xFF00A042u, 0xFF018087u, 0x27170000u});  // SVIDs: DP + TBT, then Xiaomi
    Link l = c.link();
    char b[40];
    format_speed(l, b);
    CHECK_STR(b, "3.2 Gen2 10G");
    CHECK_EQ(int(format_alt(l, b)), int(AltState::Advertised));
    CHECK_STR(b, "DP, TB, Xiaomi (adv)");
    format_alt(l, b, false);
    CHECK_STR(b, "DP, TB, Xiaomi");
    CHECK_EQ(int(l.n_other), 1);
    // an unknown vendor SVID shows its hex
    Classifier u;
    identity(u, Sop::Sop, kIdDockHub, {kUfpGen2});
    vdm(u, Sop::Sop, {0xFF00A042u, 0xFF01ABCDu});
    format_alt(u.link(), b);
    CHECK_STR(b, "DP, ABCD (adv)");
    // nothing advertised or entered
    Classifier n;
    CHECK_EQ(int(format_alt(n.link(), b)), int(AltState::None));
    CHECK_STR(b, "-");
    format_speed(n.link(), b);
    CHECK_STR(b, "Unkn");
    // DP entered: pin assignment D = two lanes
    vdm(c, Sop::Sop, {kDpEnterReq}); vdm(c, Sop::Sop, {kDpEnterAck});
    vdm(c, Sop::Sop, {kDpConfReq, (1u << 11) | 2}); vdm(c, Sop::Sop, {kDpConfAck});
    CHECK_EQ(int(format_alt(c.link(), b)), int(AltState::Entered));
    CHECK_STR(b, "DP 2-lane");
    Classifier q;
    identity(q, Sop::Sop, kIdDockHub, {kUfpGen2});
    vdm(q, Sop::Sop, {kDpEnterReq}); vdm(q, Sop::Sop, {kDpEnterAck});
    vdm(q, Sop::Sop, {kDpConfReq, (1u << 12) | 2}); vdm(q, Sop::Sop, {kDpConfAck});
    format_alt(q.link(), b);
    CHECK_STR(b, "DP 4-lane");
    format_speed(q.link(), b);
    CHECK_STR(b, "2.0 480M");
}

void test_data_reset_and_reset() {
    Classifier c;
    feed(c, Sop::Sop, kEnterUsb, {kEuUsb4Gen3}); ctl(c, kAccept);
    CHECK_EQ(c.link().mbps, 40000);
    ctl(c, kDataReset);
    Link l = c.link();
    CHECK_EQ(int(l.family), int(Family::Usb2));  // back to USB 2.0 until Enter_USB runs again
    CHECK_EQ(int(l.source), int(Source::DataReset));
    CHECK_EQ(int(l.conf), int(Confidence::Hint));
    feed(c, Sop::Sop, kEnterUsb, {kEuUsb32Gen2}); ctl(c, kAccept);
    CHECK_EQ(c.link().mbps, 10000);
    c.on_reset();
    CHECK_EQ(int(c.link().conf), int(Confidence::None));
}

void test_lines() {
    Classifier c;
    CHECK_EQ(int(c.link().family), int(Family::Unknown));
    c.on_lines(3000, 40);
    Link l = c.link();
    CHECK_EQ(int(l.family), int(Family::FullSpeed));
    CHECK_EQ(int(l.conf), int(Confidence::Hint));
    CHECK_EQ(int(l.source), int(Source::DpDm));
    CHECK_EQ(l.mbps, 12);
    c.on_lines(40, 3000);
    CHECK_EQ(int(c.link().family), int(Family::LowSpeed));
    c.on_lines(0, 0);
    CHECK_EQ(int(c.link().family), int(Family::Unknown));
    c.on_lines(2900, 2900);  // both high: a charger's shorted lines, not a data device
    CHECK_EQ(int(c.link().family), int(Family::Unknown));
    // PD data wins over the pull-up
    c.on_lines(3000, 40);
    identity(c, Sop::Sop, kIdDockHub, {kUfpGen1});
    CHECK_EQ(int(c.link().family), int(Family::Usb32));
}

void test_format() {
    char b[40];
    Classifier c;
    format(c.link(), b);
    CHECK_STR(b, "--");
    feed(c, Sop::Sop, kEnterUsb, {kEuUsb4Gen3}); ctl(c, kAccept);
    vdm(c, Sop::Sop, {kTbtEnterReq, 3u << 16}); vdm(c, Sop::Sop, {kTbtEnterAck});
    format(c.link(), b);
    CHECK_STR(b, "USB4 40G TBT4 (neg)");
    Classifier d;
    identity(d, Sop::Sop, kIdDockHub, {kUfpGen2});
    format(d.link(), b);
    CHECK_STR(b, "USB 3.2 Gen2 10G (adv)");
    Classifier e;
    e.on_lines(3000, 0);
    format(e.link(), b);
    CHECK_STR(b, "USB 1.1/2.0 FS (hint)");
    char tiny[5];
    CHECK_EQ(int(format(d.link(), tiny)), 4);
    CHECK_STR(tiny, "USB ");
}

void test_format_ui() {
    char b[40];
    auto ui = [&](Family f, std::uint32_t mbps, Tbt t, bool dp) {
        Link l; l.family = f; l.mbps = mbps; l.tbt = t; l.dp = dp; l.conf = Confidence::Advertised;
        format_ui(l, b);
        return std::string(b);
    };
    CHECK_STR(ui(Family::Unknown, 0, Tbt::None, false).c_str(), "Unkn");
    CHECK_STR(ui(Family::Unknown, 0, Tbt::None, true).c_str(), "DP alt");
    CHECK_STR(ui(Family::FullSpeed, 12, Tbt::None, false).c_str(), "FS");
    {   // D+/D- hint only: undetermined, not a guess
        Classifier h; h.on_lines(3000, 0);
        format_ui(h.link(), b);
        CHECK_STR(b, "Unkn");
    }
    CHECK_STR(ui(Family::Usb2, 480, Tbt::None, false).c_str(), "2.0 480M");
    CHECK_STR(ui(Family::Usb32, 5000, Tbt::None, false).c_str(), "3.2 Gen1 5G");
    CHECK_STR(ui(Family::Usb32, 10000, Tbt::None, true).c_str(), "3.2 Gen2 10G +DP");
    CHECK_STR(ui(Family::Usb4, 20000, Tbt::None, false).c_str(), "USB4 20G");
    CHECK_STR(ui(Family::Usb4, 40000, Tbt::Tbt4, true).c_str(), "USB4 40G TB4 +DP");
    CHECK_STR(ui(Family::Usb4v2, 80000, Tbt::Tbt5, true).c_str(), "USB4v2 80G TB5 +DP");
    CHECK_STR(ui(Family::Unknown, 40000, Tbt::Tbt3, false).c_str(), "TB3/4 40G");
    // every family x tbt x dp x rate fits the 24-cell Detail field
    const Family fams[] = {Family::Unknown, Family::LowSpeed, Family::FullSpeed, Family::Usb2, Family::Usb32, Family::Usb4, Family::Usb4v2};
    const Tbt tbts[] = {Tbt::None, Tbt::Tbt3, Tbt::Tbt4, Tbt::Tbt5};
    for (Family f : fams) for (Tbt t : tbts) for (bool dp : {false, true}) for (std::uint32_t m : {0u, 2u, 480u, 5000u, 10000u, 20000u, 40000u, 80000u})
        CHECK_EQ(int(ui(f, m, t, dp).size() <= 24), 1);
}

void test_robust() {
    Classifier c;
    g_pl.assign({1, 2, 3});
    c.on_message(Sop::Sop, hdr(1, 0x0F), {g_pl.data(), g_pl.size()});  // truncated VDM
    c.on_message(Sop::Sop, hdr(1, kEnterUsb), {g_pl.data(), g_pl.size()});
    ctl(c, kAccept);
    CHECK_EQ(int(c.link().conf), int(Confidence::None));
    // extended message header bit set: ignored
    c.on_message(Sop::Sop, std::uint16_t(0x8000 | hdr(1, kEnterUsb)), {g_pl.data(), g_pl.size()});
}

}  // namespace

int main() {
    test_enter_usb();
    test_identity_partner();
    test_cable_clamp();
    test_thunderbolt();
    test_dp_alt();
    test_dock_dp_not_tbt();
    test_tbt_advertised_without_speed();
    test_locked_phone_no_tbt();
    test_split_usb_alt();
    test_data_reset_and_reset();
    test_lines();
    test_format();
    test_format_ui();
    test_robust();
    return test_report("test_usb_link");
}
