// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Vendor_Defined message decoding for what a source sends on SOP: Discover Identity,
// Discover SVIDs and Discover Modes ACKs, the SVID name table and the Apple Product-VDO
// model lookup. Pure logic, host-tested in tests/test_pd_vdm.cpp.
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace k2::pd {

inline constexpr std::uint8_t kMsgVendorDefined = 0x0F;
inline constexpr std::uint16_t kSvidPd = 0xFF00;  // the PD SID: Discover Identity/SVIDs

// SVID or vendor name: the SVID table first, then the cable-vendor VID table, else "Unknown".
const char* svid_name(std::uint16_t svid);

// Apple device name from the Product VDO PID (VID 0x05AC); nullptr for an unknown PID and for
// the placeholder values 0x0000, 0xFFFF, 0x00FF and 0xFF00.
const char* apple_model(std::uint16_t pid);

// Discover Identity ID header (PD 3.x): "Peripheral", "Hub", "Power bank", "Charger" (DFP power brick),
// "Host"; "" when the product type is undefined. The UFP type (29:27) wins over the DFP type (25:23).
const char* id_product_type(std::uint32_t id_header);
// USB capability bits 31 (host) and 30 (device): "Host+Device", "Host", "Device" or "".
const char* id_usb_role(std::uint32_t id_header);

struct Identity {  // Discover Identity ACK: VDM header, ID header, cert stat, product VDO, ...
    bool valid = false;
    std::uint16_t vid = 0, pid = 0, bcd = 0;
    std::uint32_t id_header = 0, cert_stat = 0;
    const char* vendor = nullptr;  // set whenever valid; null keeps the engine out of .data
    const char* model = nullptr;  // Apple only
    std::array<std::uint32_t, 6> vdos{};
    std::uint8_t num_vdos = 0;
};

inline constexpr std::size_t kMaxSvids = 12;
inline constexpr std::size_t kMaxModes = 6;

struct VdmInfo {
    Identity identity;
    std::array<std::uint16_t, kMaxSvids> svids{};
    std::uint8_t num_svids = 0;
    std::uint16_t modes_svid = 0;  // SVID of the last Discover Modes ACK
    std::array<std::uint32_t, kMaxModes> modes{};
    std::uint8_t num_modes = 0;
    bool cuktech = false;      // SVID 0x2E87 with the "A080" / 0x424E0000 signature
    bool xiaomi = false;       // SVID 0x2717 proprietary command 1..8 seen
    std::uint8_t xiaomi_cmd = 0;
    std::uint16_t requests = 0;  // structured REQs from the source (we never answer them)
};

// One Vendor_Defined message: `payload` = the VDOs (VDM header first), little endian.
void parse_vdm(VdmInfo& info, std::span<const std::uint8_t> payload);

}  // namespace k2::pd
