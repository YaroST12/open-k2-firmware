// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Decoders for the extended messages and the Revision data object a source may send on SOP
// (USB PD 3.1 6.5). Pure logic, host-tested in tests/test_pd_ext.cpp. Each parser takes the
// data block (without the 2-byte extended header) and returns false when it is too short.
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace k2::pd {

// Extended message types (header bits 4:0 with the extended bit set).
inline constexpr std::uint8_t kExtSourceCapsExt = 0x01;
inline constexpr std::uint8_t kExtStatus = 0x02;
inline constexpr std::uint8_t kExtBatteryCaps = 0x05;
inline constexpr std::uint8_t kExtManufacturerInfo = 0x07;
inline constexpr std::uint8_t kExtPpsStatus = 0x0C;
inline constexpr std::uint8_t kDataRevision = 0x0C;  // data message, not extended

struct SourceCapsExt {  // SCEDB, 24 bytes (25 with the PD 3.1 EPR rating)
    bool valid = false;
    std::uint16_t vid = 0, pid = 0;
    std::uint32_t xid = 0;
    std::uint8_t fw = 0, hw = 0;
    std::uint8_t voltage_reg = 0, holdup = 0, compliance = 0, touch_current = 0;
    std::array<std::uint16_t, 3> peak_current{};
    std::uint8_t touch_temp = 0, source_inputs = 0, batteries = 0;
    std::uint8_t pdp_w = 0, epr_pdp_w = 0;
};

struct Status {  // SDB, 5 to 6 bytes
    bool valid = false;
    std::uint8_t internal_temp_c = 0;  // 0 = not reported, 1 = below 2 C
    std::uint8_t present_input = 0, battery_input = 0, event_flags = 0;
    std::uint8_t temp_status = 0;  // 0 n/a, 1 normal, 2 warning, 3 over temperature
    std::uint8_t power_status = 0;
};

struct PpsStatus {  // PPSSDB, 4 bytes
    bool valid = false;
    bool out_mv_known = false, out_ma_known = false;
    std::uint16_t out_mv = 0, out_ma = 0;
    std::uint8_t ptf = 0;  // 0 n/a, 1 normal, 2 warning, 3 over temperature
    bool omf = false;      // operating in current limit
};

struct ManufacturerInfo {  // MIDB: VID, PID, up to 22 name bytes in the first chunk
    bool valid = false;
    std::uint16_t vid = 0, pid = 0;
    char name[27] = {};
};

struct BatteryCaps {  // BCDB, 9 bytes
    bool valid = false;
    std::uint16_t vid = 0, pid = 0;
    std::uint16_t design_dwh = 0, last_full_dwh = 0;  // 0.1 Wh
    bool invalid_ref = false;
};

struct Revision {  // RMDO
    bool valid = false;
    std::uint8_t rev_major = 0, rev_minor = 0, ver_major = 0, ver_minor = 0;
};

struct ExtInfo {
    SourceCapsExt caps_ext;
    Status status;
    PpsStatus pps;
    ManufacturerInfo mfr;
    BatteryCaps battery;
    Revision revision;
};

bool parse_source_caps_ext(std::span<const std::uint8_t> d, SourceCapsExt& out);
bool parse_status(std::span<const std::uint8_t> d, Status& out);
bool parse_pps_status(std::span<const std::uint8_t> d, PpsStatus& out);
bool parse_manufacturer_info(std::span<const std::uint8_t> d, ManufacturerInfo& out);
bool parse_battery_caps(std::span<const std::uint8_t> d, BatteryCaps& out);
bool parse_revision(std::span<const std::uint8_t> d, Revision& out);  // 4-byte data object

}  // namespace k2::pd
