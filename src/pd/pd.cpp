// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB-PD decoding, see include/k2/pd/pd.hpp.
#include "k2/pd/pd.hpp"

#include <algorithm>

namespace k2::pd {

Rdo decode_rdo(std::uint32_t raw, const Pdo* pdo) {
    Rdo r;
    r.pos = std::uint8_t(raw >> 28);
    if (raw & (1u << 27)) r.flags |= kRdoFGiveBack;
    if (raw & (1u << 26)) r.flags |= kRdoFMismatch;
    if (raw & (1u << 25)) r.flags |= kRdoFUsbComm;
    if (raw & (1u << 24)) r.flags |= kRdoFNoSuspend;
    if (raw & (1u << 22)) r.flags |= kRdoFEpr;
    if (pdo) r.kind = pdo->type;
    if (pdo && (pdo->type == PdoType::Pps || pdo->type == PdoType::Avs)) {
        r.mv = std::uint16_t(((raw >> 9) & 0xFFF) * (pdo->type == PdoType::Avs ? 25 : 20));
        r.op_ma = std::uint16_t((raw & 0x7F) * 50);
        r.max_ma = r.op_ma;
    } else {
        r.mv = pdo ? pdo->max_mv : 0;
        r.op_ma = std::uint16_t(((raw >> 10) & 0x3FF) * 10);
        r.max_ma = std::uint16_t((raw & 0x3FF) * 10);
    }
    return r;
}

namespace {

constexpr std::uint32_t le32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24);
}

}  // namespace

namespace {
struct Vendor { std::uint16_t vid; const char* name; };
constexpr std::array<Vendor, 33> kVendors{{
    {0x05AC, "Apple"},     {0x04B4, "Cypress"},  {0x2E6B, "ChargerLAB"}, {0x0BDA, "Realtek"},
    {0x0424, "Microchip"}, {0x2970, "Analogix"}, {0x291A, "Hynetek"},    {0x2FCF, "Injoinic"},
    {0x31CA, "Leadtrend"}, {0x324B, "CP Power"}, {0x301E, "Gowin"},      {0x0716, "WITRN"},
    {0x1057, "Motorola"},  {0x04E8, "Samsung"},  {0x17EF, "Lenovo"},     {0x03F0, "HP"},
    {0x413C, "Dell"},      {0x045E, "Microsoft"}, {0x18D1, "Google"},    {0x8087, "Intel"},
    {0x0483, "STMicro"},   {0x203A, "Parade"},   {0x1A40, "Terminus"},   {0x05E3, "Genesys"},
    {0x0403, "FTDI"},      {0x1A86, "WCH"},      {0x24C6, "Power-Z"},    {0x1037, "Anker"},
    {0x1FC9, "NXP"},       {0x0451, "TI"},       {0x2E22, "BizLink"},
    {0x2109, "VIA Labs"},  {0x2095, "CE Link"},
}};

}  // namespace

const char* vendor_name(std::uint16_t vid) {
    auto it = std::ranges::find(kVendors, vid, &Vendor::vid);
    return it == kVendors.end() ? "Unknown" : it->name;
}

namespace {

// Cable VDO 1 bits 2:0. Intel's 0x8087 cables with 10G or more are Thunderbolt.
const char* speed_name(std::uint8_t bits, std::uint16_t vid) {
    if (vid == 0x8087 && bits >= 2) return "Thunderbolt (40G)";
    switch (bits) {
        case 1: return "USB 3.2 Gen1 (5G)";
        case 2: return "USB 3.2 Gen2 (10G)";
        case 3: return "USB4 Gen3 (40G)";
        case 4: return "USB4 Gen4 (80G)";
        default: return "USB 2.0 (480M)";  // 0 and the reserved values
    }
}

const char* latency_name(std::uint8_t code) {
    switch (code) {
        case 1: return "<10 ns (~1 m)";
        case 2: return "10-20 ns (~2 m)";
        case 3: return "20-30 ns (~3 m)";
        case 4: return "30-40 ns (~4 m)";
        case 5: return "40-50 ns (~5 m)";
        case 6: return "50-60 ns (~6 m)";
        case 7: return "60-70 ns (~7 m)";
        case 8: return ">70 ns";
        default: return "n/a";
    }
}

const char* termination_name(std::uint8_t t) {
    switch (t) {
        case 1: return "Passive, VCONN required";
        case 2: return "Active, one end";
        case 3: return "Active, both ends";
        default: return "Passive";
    }
}

}  // namespace

Pdo decode_pdo(std::uint32_t raw) {
    Pdo p;
    p.raw = raw;
    if (raw == 0) return p;  // unused slot (EPR messages zero the unused SPR positions)
    switch (raw >> 30) {
        case 0:  // Fixed: 50 mV / 10 mA units
            p.type = PdoType::Fixed;
            p.max_mv = p.min_mv = ((raw >> 10) & 0x3FF) * 50;
            p.max_ma = (raw & 0x3FF) * 10;
            p.max_mw = std::uint32_t(p.max_mv) * p.max_ma / 1000;
            break;
        case 1:  // Battery: 50 mV / 250 mW units
            p.type = PdoType::Battery;
            p.max_mv = ((raw >> 20) & 0x3FF) * 50;
            p.min_mv = ((raw >> 10) & 0x3FF) * 50;
            p.max_mw = (raw & 0x3FF) * 250;
            p.max_ma = p.min_mv ? p.max_mw * 1000 / p.min_mv : 0;
            break;
        case 2:  // Variable: 50 mV / 10 mA units
            p.type = PdoType::Variable;
            p.max_mv = ((raw >> 20) & 0x3FF) * 50;
            p.min_mv = ((raw >> 10) & 0x3FF) * 50;
            p.max_ma = (raw & 0x3FF) * 10;
            p.max_mw = std::uint32_t(p.max_mv) * p.max_ma / 1000;
            break;
        default:  // Augmented: bits 29:28 select PPS (SPR) or AVS (EPR); 100 mV / 50 mA units
            if (((raw >> 28) & 3) == 0) {
                p.type = PdoType::Pps;
                p.max_mv = ((raw >> 17) & 0xFF) * 100;
                p.min_mv = ((raw >> 8) & 0xFF) * 100;
            } else if (((raw >> 28) & 3) == 1) {
                p.type = PdoType::Avs;
                // PD 3.1 6.4.1.3.4: max V 25:17, min V 15:8 (100 mV), PDP 7:0 in watts
                p.max_mv = ((raw >> 17) & 0x1FF) * 100;
                p.min_mv = ((raw >> 8) & 0xFF) * 100;
                p.max_mw = (raw & 0xFF) * 1000u;
                p.max_ma = p.max_mv ? std::uint16_t(p.max_mw * 1000u / p.max_mv) : 0;
                return p;
            } else if (((raw >> 28) & 3) == 2) {
                // PD 3.2 SPR AVS: 9 V up, 15 V limit with
                // the current in 19:10, or 20 V limit with the current in 9:0 (10 mA units)
                p.type = PdoType::Avs;
                p.min_mv = 9000;
                const bool v20 = (raw & 0x3FF) != 0;
                p.max_mv = v20 ? 20000 : 15000;
                p.max_ma = (v20 ? raw & 0x3FF : (raw >> 10) & 0x3FF) * 10;
                p.max_mw = std::uint32_t(p.max_mv) * p.max_ma / 1000;
                return p;
            } else {
                return p;  // reserved APDO type stays Unknown
            }
            p.max_ma = (raw & 0x7F) * 50;
            p.max_mw = std::uint32_t(p.max_mv) * p.max_ma / 1000;
    }
    return p;
}

std::uint16_t current_limit_ma(const Pdo& p, std::uint16_t mv) {
    if (p.type == PdoType::Avs && (p.raw >> 28) == 0xE)
        return static_cast<std::uint16_t>((mv <= 15000 ? (p.raw >> 10) & 0x3FF : p.raw & 0x3FF) * 10);
    return p.max_ma;
}

const char* wire_revision_name(std::uint8_t rev) {
    constexpr const char* names[] = {"unknown", "PD2.0", "PD3.x", "reserved"};
    return rev < 4 ? names[rev] : "unknown";
}

CableLimits cable_limits(const Emarker& e) {
    CableLimits out;
    if (!e.valid || e.num_vdos < 5 || (e.spec_rev != 1 && e.spec_rev != 2)) return out;
    out.observed = true;
    out.max_ma = e.max_current_a == 5 ? 5000 : 3000;
    const auto vdm = e.raw_vdos[0], id = e.raw_vdos[1], cable = e.raw_vdos[4];
    const auto connector = (cable >> 18) & 3, latency = (cable >> 13) & 15;
    const auto termination = (cable >> 11) & 3, current = (cable >> 5) & 3;
    constexpr std::uint32_t reserved = (1u << 20) | (1u << 17) | (3u << 7) | (3u << 3);
    // Trust 30 V only from a well-formed PD3 passive-cable identity.
    const bool passive30 = e.spec_rev == 2 && !e.is_active && e.vdo_version == 0 &&
        e.max_voltage_v == 30 && (vdm >> 16) == 0xff00 && (vdm & (1u << 15)) &&
        ((vdm >> 6) & 3) == 1 && (vdm & 31) == 1 && ((id >> 27) & 7) == 3 &&
        ((cable >> 21) & 7) == 0 && connector >= 2 && latency >= 1 && latency <= 8 &&
        termination <= 1 && !(cable & reserved) && ((cable >> 9) & 3) == 1 &&
        ((current == 1 && e.max_current_a == 3) || (current == 2 && e.max_current_a == 5));
    if (passive30) out.max_mv = 30000;
    // A passive PD3.1 EPR cable carries VDO version 1.0 (000b) and the EPR Mode Capable bit 17 beside 50 V / 5 A
    // (bit layout from memory of PD 3.1 "Passive Cable VDO": unverified). The VDO version 2 form is kept as before.
    const bool epr_bit = e.raw_vdos[4] >> 17 & 1;
    const bool epr_cable = (e.vdo_version == 2 && e.epr_capable) || (e.vdo_version == 0 && !e.is_active && epr_bit);
    if (e.spec_rev == 2 && epr_cable && e.max_voltage_v == 50 && out.max_ma == 5000 && (!e.is_active || e.vdo2_valid))
        out.max_mv = 50000;
    return out;
}

void decode_source_caps(SourceCaps& caps, std::uint16_t header, std::span<const std::uint8_t> payload,
                        std::uint8_t active_cc) {
    const std::size_t n = std::min<std::size_t>((header >> 12) & 7, kMaxPdos);
    caps.valid = true;
    caps.num_pdos = std::uint8_t(n);
    caps.active_cc = active_cc;
    caps.spec_rev = (header >> 6) & 3;
    caps.raw_header = header;
    caps.epr = false;
    caps.epr_partial = false;
    caps.pdos = {};  // drop rows left by an earlier, longer message
    caps.rx_count++;
    for (std::size_t i = 0; i < n && 4 * i + 4 <= payload.size(); i++)
        caps.pdos[i] = decode_pdo(le32(&payload[4 * i]));
}

Emarker parse_emarker(std::span<const std::uint8_t> body, unsigned num_do) {
    Emarker e;
    e.valid = true;
    e.max_current_a = 3;
    e.max_voltage_v = 20;
    e.max_power_w = 60;
    e.usb_lanes = 1;
    e.vendor_str = "Unknown";
    e.speed_str = speed_name(0, 0);
    e.latency_str = latency_name(0);
    e.termination_str = termination_name(0);
    e.num_vdos = std::uint8_t(std::min<unsigned>({num_do, 6u, unsigned(body.size() / 4)}));
    for (unsigned i = 0; i < e.num_vdos; i++) e.raw_vdos[i] = le32(&body[4 * i]);
    const auto& v = e.raw_vdos;

    // VDO 0 is the VDM header; VDO 1 the ID header (VID, product type).
    if (num_do >= 2) {
        e.vid = v[1] & 0xFFFF;
        e.is_active = ((v[1] >> 27) & 7) == 4;
        e.vendor_str = vendor_name(e.vid);
    }
    if (num_do >= 3) e.xid = v[2];  // cert stat
    if (num_do >= 4) {              // product VDO
        e.pid = v[3] >> 16;
        e.hw_rev = (v[3] >> 8) & 0xFF;
        e.fw_rev = v[3] & 0xFF;
    }
    if (num_do >= 5) {  // cable VDO 1
        const std::uint32_t c = v[4];
        constexpr std::uint8_t kVolts[4] = {20, 30, 40, 50};
        e.max_voltage_v = kVolts[(c >> 9) & 3];
        e.max_current_a = ((c >> 5) & 3) == 2 ? 5 : 3;
        const bool a5 = e.max_current_a >= 5;
        e.epr_capable = a5 && e.max_voltage_v >= 50;
        e.max_power_w = a5 ? (e.max_voltage_v >= 48 ? 240 : e.max_voltage_v >= 40 ? 200 : 100) : 60;
        e.speed_str = speed_name(c & 7, e.vid);
        e.cable_hw = (c >> 28) & 0xF;
        e.cable_fw = (c >> 24) & 0xF;
        e.vdo_version = (c >> 21) & 7;
        e.type_c_plug = ((c >> 18) & 3) == 2;
        e.latency_code = (c >> 13) & 0xF;
        e.latency_str = latency_name(e.latency_code);
        e.termination = (c >> 11) & 3;
        e.termination_str = termination_name(e.termination);
        e.vconn_required = e.termination != 0;
    }
    if (num_do >= 6 && e.is_active) {  // active cable VDO 2
        const std::uint32_t c = v[5];
        e.vdo2_valid = true;
        e.max_temp_c = (c >> 24) & 0xFF;
        e.shutdown_temp_c = (c >> 16) & 0xFF;
        e.u3_power = (c >> 12) & 7;
        e.usb4_supported = !((c >> 8) & 1);
        e.usb_lanes = ((c >> 3) & 1) ? 2 : 1;
        e.optical_isolated = (c >> 2) & 1;
        e.usb_gen2 = c & 1;
    }
    return e;
}

const char* control_name(std::uint8_t msg_type) {
    switch (msg_type) {
        case 0x01: return "GoodCRC";
        case 0x02: return "GotoMin";
        case 0x03: return "Accept";
        case 0x04: return "Reject";
        case 0x05: return "Ping";
        case 0x06: return "PS_RDY";
        case 0x07: return "Get_Source_Cap";
        case 0x08: return "Get_Sink_Cap";
        case 0x0C: return "Wait";
        case 0x0D: return "Soft_Reset";
        default: return "OTHER";
    }
}

}  // namespace k2::pd
