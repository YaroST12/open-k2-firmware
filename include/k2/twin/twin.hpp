// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Device twin: what the K2 remembers about one device it has met, enough to answer "what would this device get from THIS
// charger" without the device (k2/twin/predict.hpp) and to negotiate as that device (the live proof). Pure data, host-tested in
// tests/test_twin_predict.cpp and tests/test_twin_lib.cpp.
//
// Where the words come from (reverse-engineered on the bench, never from a document):
//  - sink PDOs: the Sink_Capabilities the device answered to Get_Sink_Cap (Identify alone, pd::IdStatus::sink_pdos, at most 7);
//  - product type, VID/PID: its Discover Identity answer;
//  - the Request: sniffed on the wire while the K2 sat inline between this device and its own charger, plus the Source_Capabilities
//    PDO that Request pointed at (the PDO tells whether the device asked for less than it was offered).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace k2::twin {

inline constexpr std::size_t kMaxSinkPdos = 7;  // the 3-bit object count of a Sink_Capabilities message
inline constexpr std::size_t kNameMax = 24;     // same rule and size as the saved-cable names (k2/pd/emlib.hpp)
inline constexpr std::size_t kSinkExtBytes = 24; // a Sink_Capabilities_Extended data block (PD 3.1 and later: 24 bytes)

// Sink PDO 1 (the vSafe5V one) carries the flags; bit 28 = Higher Capability: the device needs more than 5 V for full function.
inline constexpr std::uint32_t kPdoHigherCapability = 1u << 28;

// What the device DECLARES about its USB data side, read with Identify while it sat alone on the K2 (raw words, decoded by
// k2/twin/usbdata.hpp). The product-type VDOs are Discover Identity words 4 and 5 as the engine keeps them (UFP VDO, then
// DFP VDO, the order the ID Header's product types give); the SVIDs are its Discover SVIDs answer (the first six); the modes
// are the first mode VDO of the Discover Modes ACKs (DisplayPort capabilities, Thunderbolt mode).
inline constexpr std::size_t kMaxProductVdos = 2, kMaxDeclSvids = 6, kMaxDeclModes = 2;
struct Declared {
    std::uint8_t n_pt = 0, n_svids = 0, n_modes = 0;
    std::array<std::uint32_t, kMaxProductVdos> pt{};
    std::array<std::uint16_t, kMaxDeclSvids> svids{};
    std::array<std::uint16_t, kMaxDeclModes> mode_svid{};
    std::array<std::uint32_t, kMaxDeclModes> mode_vdo{};
    bool any() const { return n_pt != 0 || n_svids != 0 || n_modes != 0; }
};

// What the K2 SAW this device negotiate while it sat inline between the device (or its host) and the other end: the
// usblink summary plus the raw words that decided it. A speed here is what the PD messages configured, never a measured rate.
inline constexpr std::uint8_t kObsDp = 1, kObsDp4 = 2, kObsDp2 = 4, kObsCableLimited = 8, kObsEnterUsb = 16, kObsTbtEnter = 32;
struct Observed {
    bool valid = false;
    std::uint16_t partner_vid = 0;    // the VID of the other end ("with <partner>"), 0 = not seen
    std::uint8_t family = 0;          // usblink::Family
    std::uint8_t tbt = 0;             // usblink::Tbt
    std::uint8_t flags = 0;           // kObs*
    std::uint8_t dp_pin = 0;          // the DP Configure pin assignment the partner ACKed (A = b0 .. F = b5), 0 = none
    std::uint32_t mbps = 0;           // the nominal rate of the negotiated speed
    std::uint32_t enter_usb = 0;      // the Enter_USB data object (flag kObsEnterUsb)
    std::uint32_t tbt_enter = 0;      // the Thunderbolt Enter_Mode VDO (flag kObsTbtEnter)
};

// Which KIND of source level the device chose the last time a sniff saw it choose while both a fixed level above 5 V and a PPS
// range were offered (k2/twin/learn.hpp from_sniffed). None = never seen choosing between them (the policy's default applies).
enum class KindPref : std::uint8_t { None, Pps, Fixed };

struct Twin {
    std::array<char, kNameMax + 1> name{};
    std::uint16_t vid = 0, pid = 0;  // 0 = not read
    std::uint16_t id_hi = 0;         // Discover Identity ID Header bits 31:23: USB comms host / device, UFP type, modal, DFP type (0 = not read)
    std::uint8_t pd_rev = 0;         // wire revision of its messages: 1 PD 2.0, 2 PD 3.x, 0 = not read
    std::uint8_t num_pdos = 0;       // sink PDOs held in `pdos`
    std::array<std::uint32_t, kMaxSinkPdos> pdos{};
    // The last Request seen on the wire from this device. rdo == 0: none. rdo_pdo = the source PDO word it pointed at.
    std::uint32_t rdo = 0, rdo_pdo = 0;
    Declared decl;   // USB data, declared (device alone)
    Observed obs;    // USB data, observed (sniffed inline)
    KindPref pref = KindPref::None;   // the kind it chose when it had both (flag bits of the record, k2/twin/library.hpp)
    // PD 3 answers read by an inline interrogation (k2/pd/pd_interrogate.hpp), raw: the Revision data object (0 = not read)
    // and the Sink_Capabilities_Extended data block (sink_ext_len bytes, 0 = not read).
    std::uint32_t revision = 0;
    std::uint8_t sink_ext_len = 0;
    std::array<std::uint8_t, kSinkExtBytes> sink_ext{};

    bool has_request() const { return rdo != 0 && rdo_pdo != 0; }
    std::uint8_t ufp_type() const { return std::uint8_t((id_hi >> 4) & 7u); }   // ID Header 29:27: 1 hub, 2 peripheral, 3 power bank
    std::uint8_t dfp_type() const { return std::uint8_t(id_hi & 7u); }          // ID Header 25:23: 1 hub, 2 host, 3 charger
};

}  // namespace k2::twin
