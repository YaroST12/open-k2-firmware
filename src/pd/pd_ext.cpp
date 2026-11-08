// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_ext.hpp"

#include <algorithm>

namespace k2::pd {

namespace {
std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
}  // namespace

bool parse_source_caps_ext(std::span<const std::uint8_t> d, SourceCapsExt& o) {
    if (d.size() < 24) return false;
    o = {};
    o.valid = true;
    o.vid = le16(&d[0]);
    o.pid = le16(&d[2]);
    o.xid = le16(&d[4]) | (std::uint32_t(le16(&d[6])) << 16);
    o.fw = d[8];
    o.hw = d[9];
    o.voltage_reg = d[10];
    o.holdup = d[11];
    o.compliance = d[12];
    o.touch_current = d[13];
    for (int i = 0; i < 3; i++) o.peak_current[i] = le16(&d[14 + 2 * i]);
    o.touch_temp = d[20];
    o.source_inputs = d[21];
    o.batteries = d[22];
    o.pdp_w = d[23];
    if (d.size() >= 25) o.epr_pdp_w = d[24];
    return true;
}

bool parse_status(std::span<const std::uint8_t> d, Status& o) {
    if (d.size() < 5) return false;
    o = {};
    o.valid = true;
    o.internal_temp_c = d[0];
    o.present_input = d[1];
    o.battery_input = d[2];
    o.event_flags = d[3];
    o.temp_status = (d[4] >> 1) & 3;
    if (d.size() >= 6) o.power_status = d[5];
    return true;
}

bool parse_pps_status(std::span<const std::uint8_t> d, PpsStatus& o) {
    if (d.size() < 4) return false;
    o = {};
    o.valid = true;
    const std::uint16_t v = le16(&d[0]);
    if (v != 0xFFFF) { o.out_mv_known = true; o.out_mv = std::uint16_t(v * 20u); }
    if (d[2] != 0xFF) { o.out_ma_known = true; o.out_ma = std::uint16_t(d[2] * 50u); }
    o.ptf = (d[3] >> 1) & 3;
    o.omf = (d[3] >> 3) & 1;
    return true;
}

bool parse_manufacturer_info(std::span<const std::uint8_t> d, ManufacturerInfo& o) {
    if (d.size() < 4) return false;
    o = {};
    o.valid = true;
    o.vid = le16(&d[0]);
    o.pid = le16(&d[2]);
    const std::size_t n = std::min(d.size() - 4, sizeof o.name - 1);
    for (std::size_t i = 0; i < n && d[4 + i] != 0; i++)
        o.name[i] = d[4 + i] >= 0x20 && d[4 + i] < 0x7F ? char(d[4 + i]) : '?';
    return true;
}

bool parse_battery_caps(std::span<const std::uint8_t> d, BatteryCaps& o) {
    if (d.size() < 9) return false;
    o = {};
    o.valid = true;
    o.vid = le16(&d[0]);
    o.pid = le16(&d[2]);
    o.design_dwh = le16(&d[4]);
    o.last_full_dwh = le16(&d[6]);
    o.invalid_ref = d[8] & 1;
    return true;
}

bool parse_revision(std::span<const std::uint8_t> d, Revision& o) {
    if (d.size() < 4) return false;
    o = {};
    o.valid = true;
    o.rev_major = d[3] >> 4;
    o.rev_minor = d[3] & 0xF;
    o.ver_major = d[2] >> 4;
    o.ver_minor = d[2] & 0xF;
    return true;
}

}  // namespace k2::pd
