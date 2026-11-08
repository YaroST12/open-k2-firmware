// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB Power Delivery decoding, pure logic (no I/O, host-tested in tests/test_pd.cpp):
// Source_Capabilities PDOs, the SOP' Discover Identity answer of an e-marker cable, and
// message names. Field layouts follow USB PD 3.1 section 6.4.1 (PDOs) and 6.4.4.3 (VDOs).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::pd {

enum class PdoType : std::uint8_t { Unknown, Fixed, Battery, Variable, Pps, Avs };

struct Pdo {
    PdoType type = PdoType::Unknown;
    std::uint32_t raw = 0;
    std::uint16_t min_mv = 0, max_mv = 0, max_ma = 0;
    std::uint32_t max_mw = 0;
};

// 7 SPR positions (the 3-bit "number of data objects" field) plus 4 EPR positions: an
// EPR_Source_Capabilities message carries up to 11 PDOs, EPR ones from position 8 on.
inline constexpr std::size_t kMaxPdos = 11;

struct SourceCaps {
    bool valid = false;
    std::uint8_t num_pdos = 0;
    std::uint8_t active_cc = 0;  // 0 none, 1 CC1, 2 CC2
    std::uint8_t spec_rev = 0;   // wire header: 1 PD 2.0, 2 PD 3.x; no exact minor revision
    std::uint16_t raw_header = 0;
    bool epr = false;          // built from EPR_Source_Capabilities chunks
    bool epr_partial = false;  // not every chunk seen (sniffing is passive, we never request one)
    std::array<Pdo, kMaxPdos> pdos{};  // by position; raw == 0 / Unknown marks an unused slot
    std::uint32_t rx_count = 0;  // Source_Capabilities messages seen since boot
};

// Cable identity from the Discover Identity ACK (VDM header, ID header, cert stat, product,
// cable VDO 1 [, active cable VDO 2]). Missing VDOs leave the defaults: 3 A / 20 V / 60 W,
// USB 2.0. Field layout: USB PD 3.1 6.4.4.3.1 (cable VDO 1) and 6.4.4.3.2 (active VDO 2).
struct Emarker {
    bool valid = false;
    bool is_active = false;  // ID header product type 4 (active cable)
    std::uint16_t vid = 0, pid = 0;
    std::uint32_t xid = 0;
    std::uint8_t hw_rev = 0, fw_rev = 0;
    std::uint8_t max_current_a = 0, max_voltage_v = 0;  // parse_emarker defaults: 3 A / 20 V / 60 W
    std::uint16_t max_power_w = 0;
    const char* speed_str = nullptr;  // set by parse_emarker; null keeps the engine out of .data
    const char* vendor_str = nullptr;
    std::array<std::uint32_t, 6> raw_vdos{};
    std::uint8_t num_vdos = 0;

    // Cable VDO 1 detail.
    std::uint8_t cable_hw = 0, cable_fw = 0;  // bits 31:28, 27:24
    std::uint8_t vdo_version = 0;             // bits 23:21
    bool type_c_plug = false;                 // bits 19:18 == 2
    std::uint8_t latency_code = 0;            // bits 16:13
    const char* latency_str = nullptr;
    std::uint8_t termination = 0;             // bits 12:11
    const char* termination_str = nullptr;
    bool vconn_required = false;              // termination 01, and active cables (10, 11)
    bool epr_capable = false;                 // 5 A and 50 V

    // Active cable VDO 2 (product type 4 only).
    bool vdo2_valid = false;
    std::uint8_t max_temp_c = 0, shutdown_temp_c = 0;  // bits 31:24, 23:16
    std::uint8_t u3_power = 0;                // bits 14:12, U3/CLd power code
    bool usb4_supported = false;              // bit 8 clear
    std::uint8_t usb_lanes = 0;               // bit 3: 1 = two lanes (parse sets 1 or 2)
    bool usb_gen2 = false;                    // bit 0: Gen 2 or higher
    bool optical_isolated = false;            // bit 2

    std::uint8_t sop = 0;       // 1 = answered on SOP', 2 = on SOP''; 0 = none yet
    std::uint8_t spec_rev = 0;  // wire header of the answer: 1 PD 2.0, 2 PD 3.x
};

struct CableLimits {
    std::uint16_t max_mv = 20000, max_ma = 3000;
    bool observed = false;
};
// Apply only to a physical cable's wire ACK, never a saved/spoofed identity.
CableLimits cable_limits(const Emarker&);

// Request Data Object (USB PD 3.1 6.4.2), as the sink sent it. `flags` are the kRdo* bits.
inline constexpr std::uint8_t kRdoFGiveBack = 1, kRdoFMismatch = 2, kRdoFUsbComm = 4, kRdoFNoSuspend = 8, kRdoFEpr = 16;
struct Rdo {
    std::uint8_t pos = 0;                 // 1-based object position
    PdoType kind = PdoType::Unknown;      // of the PDO it points at; Unknown when that PDO was not seen
    std::uint16_t mv = 0;                 // fixed: the PDO voltage when known; PPS/AVS: the requested voltage
    std::uint16_t op_ma = 0, max_ma = 0;  // PPS/AVS: max_ma == op_ma
    std::uint8_t flags = 0;
};
// `pdo` is the offer at rdo.pos (nullptr when unknown). Bits: 27 give-back, 26 capability mismatch,
// 25 USB communications capable, 24 no USB suspend, 22 EPR capable; fixed/variable 19:10 operating and
// 9:0 maximum current in 10 mA; PPS/AVS 20:9 output voltage (20 / 25 mV), 6:0 operating current (50 mA).
Rdo decode_rdo(std::uint32_t raw, const Pdo* pdo);

// Decode one 32-bit Power Data Object.
Pdo decode_pdo(std::uint32_t raw);
std::uint16_t current_limit_ma(const Pdo&, std::uint16_t mv);
const char* wire_revision_name(std::uint8_t);

// Fill `caps` from a Source_Capabilities message: header from the wire, `payload` the data
// objects (little endian). Bumps rx_count, records the CC line the message arrived on.
void decode_source_caps(SourceCaps& caps, std::uint16_t header, std::span<const std::uint8_t> payload,
                        std::uint8_t active_cc);

// Parse `num_do` VDOs (little endian) from `body`.
Emarker parse_emarker(std::span<const std::uint8_t> body, unsigned num_do);

// Name of a cable/chip vendor by USB VID; "Unknown" when not listed.
const char* vendor_name(std::uint16_t vid);

// Name of a control message type (header bits 4:0); "OTHER" when not listed.
const char* control_name(std::uint8_t msg_type);

}  // namespace k2::pd
