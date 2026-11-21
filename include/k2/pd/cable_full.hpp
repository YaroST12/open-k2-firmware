// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A cable's full identity read: after Discover Identity on SOP', Discover SVIDs and Discover Modes for each SVID, and on
// an active cable the same on SOP''. Pure logic over the exchange callback of emk_learn.hpp (host-tested in
// tests/test_cable_full.cpp), plus the decoders of the words it collects: the active cable VDO 2 extras, the Thunderbolt 3
// cable mode VDO and the DisplayPort cable capabilities VDO (bit layouts of the public typec_tbt.h / typec_dp.h headers).
// A silent or refusing cable keeps what was already read; nothing here retries.
#pragma once
#include <array>
#include <cstdint>

#include "k2/pd/emk_learn.hpp"

namespace k2::cable_full {

inline constexpr unsigned kMaxSvids = 4;   // SVIDs explored per plug (a cable has one to three)
inline constexpr std::uint16_t kSvidTbt = 0x8087, kSvidDp = 0xFF01;
inline constexpr std::uint8_t kSopPrime = 1, kSopDouble = 2;   // Request::sop

// What one plug answered after its identity.
struct Plug {
    bool svids_ack = false;                         // Discover SVIDs ACKed
    std::uint8_t nsvid = 0;
    std::uint8_t mode_ack = 0;                      // bit i: svid[i] answered Discover Modes
    std::array<std::uint16_t, kMaxSvids> svid{};
    std::array<std::uint32_t, kMaxSvids> mode{};    // the first mode VDO of svid[i] (0 until it answered)
};

struct Full {
    bool valid = false;                             // a read ran on this cable
    Plug p1;                                        // SOP'
    bool id2_ack = false;                           // SOP'' answered Discover Identity
    std::uint8_t id2_n = 0;                         // VDOs after the VDM header
    std::array<std::uint32_t, 5> id2{};             // ID header, cert stat, product, cable VDO 1, cable VDO 2
    Plug p2;                                        // SOP''
};

// Runs the read through `fn`. `var` is the variant the cable answered Discover Identity in, `active` the SOP' ID header's
// active-cable type. False when SOP' stopped being acknowledged (the cable is gone); what was read stays in `out`.
bool read(emk_learn::ExchangeFn fn, void* ctx, emk_variant::Variant var, bool active, Full& out);

// The text of a U3/CLd power code (active VDO 2 bits 14:12), "?" for the invalid 7.
const char* u3_power_text(std::uint8_t code);

// Active cable VDO 2 beyond what pd::Emarker keeps. `v13`: VDO 1 version 1.3, before it bits 10:8 are reserved.
struct ActiveExtra {
    bool optical = false;       // bit 10 physical connection
    bool retimer = false;       // bit 9 active element (else re-driver)
    bool usb2 = false;          // bit 5, active low
    bool usb32 = false;         // bit 4, active low
    std::uint8_t hub_hops = 0;  // bits 7:6
    bool u3_via_u3s = false;    // bit 11
    bool asym = false;          // bit 1 (USB4 asymmetric mode)
};
ActiveExtra decode_active_extra(std::uint32_t vdo2, bool v13);

// Thunderbolt 3 cable Discover Modes VDO (SVID 0x8087).
struct Tbt {
    bool tbt_mode = false;      // bit 0
    std::uint8_t speed = 0;     // bits 18:16: 1 USB 3.1 Gen 1, 2 10 Gbit/s, 3 10 and 20 Gbit/s
    bool rounded = false;       // bit 19
    bool optical = false;       // bit 21
    bool retimer = false;       // bit 22
    bool uni_training = false;  // bit 23 link training
    bool active = false;        // bit 25 (else passive)
};
Tbt decode_tbt(std::uint32_t vdo);
const char* tbt_speed_text(std::uint8_t speed);   // "USB3", "10G", "10/20G", "?"

// DisplayPort capabilities VDO of a cable (SVID 0xFF01).
struct Dp {
    std::uint8_t rate = 0;      // bits 5:2: 1 HBR3, 2 UHBR10, 3 UHBR20, 0 or higher: not stated
    bool uhbr13_5 = false;      // bit 26
    std::uint8_t cable = 0;     // bits 29:28: 0 passive, 1 re-timer, 2 re-driver, 3 optical
    std::uint8_t pins = 0;      // DFP_D and UFP_D pin assignments, bit i = assignment A + i (6 bits)
};
Dp decode_dp(std::uint32_t vdo);
const char* dp_rate_text(const Dp& d);      // "HBR3", "UHBR10", "UHBR13.5", "UHBR20", "" when not stated
const char* dp_cable_text(std::uint8_t cable);   // "passive", "retimer", "redriver", "optical"

}  // namespace k2::cable_full
