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
inline constexpr std::uint8_t kExtGetBatteryCap = 0x03;
inline constexpr std::uint8_t kExtGetBatteryStatus = 0x04;
inline constexpr std::uint8_t kExtBatteryCaps = 0x05;
inline constexpr std::uint8_t kExtGetManufacturerInfo = 0x06;
inline constexpr std::uint8_t kExtManufacturerInfo = 0x07;
inline constexpr std::uint8_t kExtPpsStatus = 0x0C;
inline constexpr std::uint8_t kExtEprSinkCaps = 0x12;  // EPR_Sink_Capabilities (kExtEprSourceCaps is 0x11: pd_engine.hpp)
inline constexpr std::uint8_t kDataBatteryStatus = 0x05;  // data message, not extended (BSDO)
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

struct BatteryCaps {  // BCDB, 9 bytes (USB PD 3.2 table 6.55)
    bool valid = false;
    std::uint16_t vid = 0, pid = 0;
    std::uint16_t design_dwh = 0, last_full_dwh = 0;  // 0.1 Wh; 0x0000 = not present, 0xFFFF = unknown
    bool invalid_ref = false;
    bool design_known() const { return valid && !invalid_ref && design_dwh != 0 && design_dwh != 0xFFFFu; }
    bool last_full_known() const { return valid && !invalid_ref && last_full_dwh != 0 && last_full_dwh != 0xFFFFu; }
};

struct BatteryStatus {  // BSDO, 4 bytes (USB PD 3.2 table 6.24)
    bool valid = false;
    bool present = false;
    bool invalid_ref = false;
    std::uint8_t charging = 3;      // 0 = charging, 1 = discharging, 2 = idle, 3 = unknown/invalid
    std::uint16_t present_dwh = 0;  // 0.1 Wh; 0xFFFF = unknown
    bool capacity_known() const { return valid && present && !invalid_ref && present_dwh != 0xFFFFu; }
};

// State of charge in 0..100 % from Battery_Capabilities + Battery_Status; -1 when unknown.
constexpr int battery_soc_pct(const BatteryCaps& cap, const BatteryStatus& st) {
    if (!st.capacity_known()) return -1;
    const std::uint32_t full = cap.last_full_known() ? cap.last_full_dwh : cap.design_known() ? cap.design_dwh : 0u;
    if (full == 0) return -1;
    const std::uint32_t pct = (std::uint32_t(st.present_dwh) * 100u + full / 2u) / full;
    return pct > 100u ? 100 : int(pct);
}

// Battery health in % (last_full / design); -1 when either capacity is unknown.
constexpr int battery_health_pct(const BatteryCaps& cap) {
    if (!cap.design_known() || !cap.last_full_known()) return -1;
    const std::uint32_t pct = (std::uint32_t(cap.last_full_dwh) * 100u + cap.design_dwh / 2u) / cap.design_dwh;
    return pct > 200u ? 200 : int(pct);
}

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
    BatteryStatus battery_status;
    Revision revision;
};

bool parse_source_caps_ext(std::span<const std::uint8_t> d, SourceCapsExt& out);
bool parse_status(std::span<const std::uint8_t> d, Status& out);
bool parse_pps_status(std::span<const std::uint8_t> d, PpsStatus& out);
bool parse_manufacturer_info(std::span<const std::uint8_t> d, ManufacturerInfo& out);
bool parse_battery_caps(std::span<const std::uint8_t> d, BatteryCaps& out);
bool parse_battery_status(std::span<const std::uint8_t> d, BatteryStatus& out);  // 4-byte BSDO
bool parse_revision(std::span<const std::uint8_t> d, Revision& out);  // 4-byte data object

}  // namespace k2::pd
