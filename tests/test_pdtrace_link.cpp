// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The link a capture (or the live ring) shows: Enter_USB, Enter Mode for TBT3 and DisplayPort, the
// DP pin assignment, the e-marker cable and the partner's USB capability, from sniffed messages only.
#include <string>
#include <vector>

#include "k2/pd/pdtrace.hpp"
#include "k2/pd/pdtrace_link.hpp"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2::pdtrace;
namespace v = k2::pdtrace::view;

namespace {
using Buf = std::vector<std::uint8_t>;
void add(Buf& b, Type t, std::uint32_t ts, std::vector<std::uint8_t> p) {
    b.push_back(std::uint8_t(6 + p.size()));
    b.push_back(std::uint8_t(t));
    for (int i = 0; i < 4; i++) b.push_back(std::uint8_t(ts >> (8 * i)));
    b.insert(b.end(), p.begin(), p.end());
}
void rx(Buf& b, std::uint32_t ts, std::uint8_t token, unsigned hdr, std::vector<std::uint32_t> objs) {
    std::vector<std::uint8_t> p = {token, std::uint8_t(hdr), std::uint8_t(hdr >> 8)};
    for (auto o : objs) for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(o >> (8 * i)));
    p.insert(p.end(), 4, 0);
    add(b, Type::Rx, ts, p);
}
void tx(Buf& b, std::uint32_t ts, unsigned hdr, std::vector<std::uint32_t> objs) {
    std::vector<std::uint8_t> p = {std::uint8_t(hdr), std::uint8_t(hdr >> 8)};
    for (auto o : objs) for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(o >> (8 * i)));
    add(b, Type::Tx, ts, p);
}
constexpr unsigned kSrc = 1u << 8;
constexpr unsigned hdr(unsigned type, unsigned n, unsigned role = 0) { return type | role | n << 12; }
constexpr std::uint32_t kEnter = 4, kReq = 0, kAck = 1u << 6, kNak = 2u << 6, kStruct = 1u << 15, kPos1 = 1u << 8;
constexpr std::uint32_t vdm(std::uint32_t svid, std::uint32_t type, std::uint32_t cmd) { return svid << 16 | kStruct | kPos1 | type | cmd; }
constexpr std::uint32_t kEudo(unsigned mode, unsigned speed, unsigned ctype, unsigned cur) {
    return mode << 28 | speed << 21 | ctype << 19 | cur << 17;
}
std::string text(const v::Link& l) {
    char o[96];
    return std::string(o, v::format_link(l, o));
}
std::string json(const v::Link& l) {
    char o[v::kJsonLine];
    v::json_link(l, std::span<char, v::kJsonLine>(o));
    std::string s(o, v::kJsonLine);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
}  // namespace

int main() {
    {   // nothing sniffed
        Buf b;
        const auto l = v::link_of(b);
        CHECK_STR(text(l).c_str(), "not seen");
        CHECK_STR(json(l).c_str(), "{\"link\":{}}");
    }
    {   // Enter_USB: USB4 on a Gen3 active-retimer cable with 5 A
        Buf b;
        tx(b, 100, hdr(8, 1, kSrc), {kEudo(2, 3, 1, 3)});
        const auto l = v::link_of(b);
        CHECK_EQ(l.eusb, 1); CHECK_EQ(l.usb_mode, 2); CHECK_EQ(l.usb_speed, 3);
        CHECK_STR(text(l).c_str(), "USB4 Gen3 40G | cable 40G 5A retimer");
        CHECK_STR(json(l).c_str(), "{\"link\":{\"usb\":\"USB4 Gen3 40G\",\"cable\":\"40G 5A retimer\"}}");
    }
    {   // other Enter_USB modes
        Buf a, c, d;
        rx(a, 1, 0xE0, hdr(8, 1, kSrc), {kEudo(1, 2, 0, 2)});
        CHECK_STR(text(v::link_of(a)).c_str(), "USB3.2 Gen2 10G | cable 10G 3A");
        rx(c, 1, 0xE0, hdr(8, 1, kSrc), {kEudo(0, 0, 0, 0)});
        CHECK_STR(text(v::link_of(c)).c_str(), "USB2 | cable USB2");
        rx(d, 1, 0xE0, hdr(8, 1, kSrc), {kEudo(2, 4, 3, 3)});
        CHECK_STR(text(v::link_of(d)).c_str(), "USB4 Gen4 80G | cable 80G 5A optical");
    }
    {   // Thunderbolt 3: the request carries the cable, the ACK says the mode was entered
        Buf b;
        rx(b, 100, 0xE0, hdr(15, 2, kSrc), {vdm(0x8087, kReq, kEnter), 3u << 16 | 1u << 24});
        CHECK_EQ(v::link_of(b).tbt, 0);                    // not entered until acknowledged
        rx(b, 200, 0xE0, hdr(15, 1), {vdm(0x8087, kAck, kEnter)});
        const auto l = v::link_of(b);
        CHECK_EQ(l.tbt, 1); CHECK_EQ(l.tbt_active, 1); CHECK_EQ(l.tbt_speed, 3);
        CHECK_STR(text(l).c_str(), "TBT3 40G act");
    }
    {   // a NAK means no mode
        Buf b;
        rx(b, 100, 0xE0, hdr(15, 2, kSrc), {vdm(0x8087, kReq, kEnter), 3u << 16});
        rx(b, 200, 0xE0, hdr(15, 1), {vdm(0x8087, kNak, kEnter)});
        CHECK_EQ(v::link_of(b).tbt, 0);
        CHECK_STR(text(v::link_of(b)).c_str(), "not seen");
    }
    {   // DisplayPort: Enter Mode ACK, then Configure with pin assignment C
        Buf b;
        rx(b, 100, 0xE0, hdr(15, 1, kSrc), {vdm(0xFF01, kReq, kEnter)});
        rx(b, 200, 0xE0, hdr(15, 1), {vdm(0xFF01, kAck, kEnter)});
        CHECK_STR(text(v::link_of(b)).c_str(), "DP");
        rx(b, 300, 0xE0, hdr(15, 2, kSrc), {vdm(0xFF01, kReq, 17), 1u << 10 | 1u << 2 | 2u});
        CHECK_EQ(v::link_of(b).dp_pin, 0);                 // Configure not acknowledged yet
        rx(b, 400, 0xE0, hdr(15, 1), {vdm(0xFF01, kAck, 17)});
        const auto l = v::link_of(b);
        CHECK_EQ(l.dp, 1); CHECK_EQ(l.dp_pin, 'C');
        CHECK_STR(text(l).c_str(), "DP alt C");
        CHECK_STR(json(l).c_str(), "{\"link\":{\"dp\":\"C\"}}");
    }
    {   // Configure with several pins selected: the lowest letter... pin D (bit 3) alone
        Buf b;
        rx(b, 100, 0xE0, hdr(15, 1, kSrc), {vdm(0xFF01, kAck, kEnter)});
        rx(b, 200, 0xE0, hdr(15, 2, kSrc), {vdm(0xFF01, kReq, 17), 1u << 11 | 2u});
        rx(b, 300, 0xE0, hdr(15, 1), {vdm(0xFF01, kAck, 17)});
        CHECK_EQ(v::link_of(b).dp_pin, 'D');
    }
    {   // e-marker on SOP': passive cable, USB4 Gen3, 5 A; replaces what Enter_USB said
        Buf b;
        tx(b, 50, hdr(8, 1, kSrc), {kEudo(2, 2, 0, 2)});
        rx(b, 100, 0xC0, hdr(15, 5), {vdm(0xFF00, kAck, 1), 3u << 27 | 0x2109, 0, 0, 3u | 2u << 5});
        const auto l = v::link_of(b);
        CHECK_EQ(l.cable, 1); CHECK_EQ(l.cable_speed, 3); CHECK_EQ(l.cable_amps, 5); CHECK_EQ(l.cable_active, 0);
        CHECK_STR(text(l).c_str(), "USB4 Gen2 20G | cable 40G 5A");
        Buf c;                                              // the e-marker came first: same result
        rx(c, 100, 0xC0, hdr(15, 5), {vdm(0xFF00, kAck, 1), 4u << 27 | 0x2109, 0, 0, 3u | 2u << 5});
        tx(c, 150, hdr(8, 1, kSrc), {kEudo(2, 2, 1, 2)});
        CHECK_STR(text(v::link_of(c)).c_str(), "USB4 Gen2 20G | cable 40G 5A act");
    }
    {   // partner Discover Identity: USB host and device capability from the ID header (SOP, sink role)
        Buf b;
        rx(b, 100, 0xE0, hdr(15, 4), {vdm(0xFF00, kAck, 1), 1u << 31 | 1u << 30 | 0x05AC, 0, 0x1234u << 16});
        const auto l = v::link_of(b);
        CHECK_EQ(l.peer, 1); CHECK_EQ(l.peer_host, 1); CHECK_EQ(l.peer_dev, 1);
        CHECK_STR(text(l).c_str(), "partner USB host+dev");
        Buf c, d;
        rx(c, 100, 0xE0, hdr(15, 4), {vdm(0xFF00, kAck, 1), 1u << 30 | 0x05AC, 0, 0});
        CHECK_STR(text(v::link_of(c)).c_str(), "partner USB dev");
        rx(d, 100, 0xE0, hdr(15, 4), {vdm(0xFF00, kAck, 1), 0x05AC, 0, 0});
        CHECK_STR(text(v::link_of(d)).c_str(), "partner no USB");
        // a source-role answer is the charger, not the partner: only the sink role counts, else the source
        Buf e;
        rx(e, 100, 0xE0, hdr(15, 4, kSrc), {vdm(0xFF00, kAck, 1), 1u << 31 | 0x2717, 0, 0});
        rx(e, 200, 0xE0, hdr(15, 4), {vdm(0xFF00, kAck, 1), 1u << 30 | 0x18D1, 0, 0});
        CHECK_STR(text(v::link_of(e)).c_str(), "partner USB dev");
    }
    {   // everything, and the widest JSON line fits
        Buf b;
        tx(b, 10, hdr(8, 1, kSrc), {kEudo(2, 4, 3, 3)});
        rx(b, 20, 0xE0, hdr(15, 2, kSrc), {vdm(0x8087, kReq, kEnter), 3u << 16 | 1u << 24});
        rx(b, 30, 0xE0, hdr(15, 1), {vdm(0x8087, kAck, kEnter)});
        rx(b, 40, 0xE0, hdr(15, 1, kSrc), {vdm(0xFF01, kAck, kEnter)});
        rx(b, 50, 0xE0, hdr(15, 2, kSrc), {vdm(0xFF01, kReq, 17), 1u << 10});
        rx(b, 60, 0xE0, hdr(15, 1), {vdm(0xFF01, kAck, 17)});
        rx(b, 70, 0xC0, hdr(15, 5), {vdm(0xFF00, kAck, 1), 4u << 27, 0, 0, 4u | 2u << 5});
        rx(b, 80, 0xE0, hdr(15, 4), {vdm(0xFF00, kAck, 1), 1u << 31 | 1u << 30, 0, 0});
        const auto l = v::link_of(b);
        CHECK_STR(text(l).c_str(), "USB4 Gen4 80G | TBT3 40G act | DP alt C | cable 80G 5A act | partner USB host+dev");
        CHECK_EQ(json(l).size() < v::kJsonLine, 1);
    }
    {   // the accumulator gives the same answer, and forgets on reset
        Buf b;
        tx(b, 100, hdr(8, 1, kSrc), {kEudo(2, 3, 0, 3)});
        v::LinkAcc acc;
        acc.feed(b);
        CHECK_STR(text(acc.link()).c_str(), "USB4 Gen3 40G | cable 40G 5A");
        acc.reset();
        CHECK_STR(text(acc.link()).c_str(), "not seen");
    }
    {   // live: the recorder keeps the link as messages arrive and forgets it on a hard reset or detach
        k2::pdtrace::init();
        const std::uint32_t d = kEudo(2, 3, 0, 3);
        k2::pdtrace::tx(hdr(8, 1, kSrc), {reinterpret_cast<const std::uint8_t*>(&d), 4});
        CHECK_STR(text(k2::pdtrace::live_link()).c_str(), "USB4 Gen3 40G | cable 40G 5A");
        k2::pdtrace::reset(k2::pdtrace::ResetKind::HardRx);
        CHECK_STR(text(k2::pdtrace::live_link()).c_str(), "not seen");
        k2::pdtrace::tx(hdr(8, 1, kSrc), {reinterpret_cast<const std::uint8_t*>(&d), 4});
        k2::pdtrace::cc(k2::pdtrace::CcEvent::Detach, 0, 0);
        CHECK_STR(text(k2::pdtrace::live_link()).c_str(), "not seen");
    }
    return test_report("test_pdtrace_link");
}
