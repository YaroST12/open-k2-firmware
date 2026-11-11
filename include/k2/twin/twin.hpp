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

// Sink PDO 1 (the vSafe5V one) carries the flags; bit 28 = Higher Capability: the device needs more than 5 V for full function.
inline constexpr std::uint32_t kPdoHigherCapability = 1u << 28;

struct Twin {
    std::array<char, kNameMax + 1> name{};
    std::uint16_t vid = 0, pid = 0;  // 0 = not read
    std::uint16_t id_hi = 0;         // Discover Identity ID Header bits 31:23: USB comms host / device, UFP type, modal, DFP type (0 = not read)
    std::uint8_t pd_rev = 0;         // wire revision of its messages: 1 PD 2.0, 2 PD 3.x, 0 = not read
    std::uint8_t num_pdos = 0;       // sink PDOs held in `pdos`
    std::array<std::uint32_t, kMaxSinkPdos> pdos{};
    // The last Request seen on the wire from this device. rdo == 0: none. rdo_pdo = the source PDO word it pointed at.
    std::uint32_t rdo = 0, rdo_pdo = 0;

    bool has_request() const { return rdo != 0 && rdo_pdo != 0; }
    std::uint8_t ufp_type() const { return std::uint8_t((id_hi >> 4) & 7u); }   // ID Header 29:27: 1 hub, 2 peripheral, 3 power bank
    std::uint8_t dfp_type() const { return std::uint8_t(id_hi & 7u); }          // ID Header 25:23: 1 hub, 2 host, 3 charger
};

}  // namespace k2::twin
