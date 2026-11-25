// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB-PD decoding, see include/k2/pd/pd.hpp.
#include "k2/pd/pd.hpp"

#include <algorithm>
#include <cstring>

#include "k2/core/hot.h"
#include "k2/pd/vendor_pack.hpp"
#include "k2/pd/vendor_table.hpp"
#include "k2/core/strpack.hpp"

namespace k2::pd {

K2_HOT Rdo decode_rdo(std::uint32_t raw, const Pdo* pdo) {
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

VendorName vendor_find(std::uint16_t vid, bool* weak) {
    namespace vt = vendor_table;
    unsigned lo = 0, hi = vt::kCount;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (vt::kVid[mid] < vid) lo = mid + 1; else hi = mid;
    }
    const bool hit = lo < vt::kCount && vt::kVid[lo] == vid;
    if (weak) *weak = hit && ((vt::kWeak[lo >> 3] >> (lo & 7)) & 1) != 0;
    VendorName name;
    if (!hit) return name;
    unsigned bit = vt::kSkip[lo / vt::kStride];
    for (unsigned i = lo - lo % vt::kStride; i < lo; i++) bit = vendor_pack::unpack(vt::kPool, bit, nullptr);
    vendor_pack::unpack(vt::kPool, bit, name.s);
    return name;
}

VendorName vendor_name(std::uint16_t vid) {
    const VendorName n = vendor_find(vid);
    return n ? n : VendorName(K2_P("Unknown"));
}

namespace {

// Cable VDO 1 bits 2:0, already held to the layout's highest valid code. Intel's 0x8087 cables with 10G or more are
// Thunderbolt (a label only).
// k2-plain-begin: speed_short slices it, ui_emarker strncmps it
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
// k2-plain-end

// Cable VDO 1 bits 16:13. 1000b is "> 70 ns" on a passive cable; an active cable goes on to 1000 / 2000 / 3000 ns.
// k2-plain-begin: latency_short walks it byte by byte
const char* latency_name(std::uint8_t code, bool active) {
    if (active) {
        switch (code) {
            case 8: return "1000 ns (~100 m)";
            case 9: return "2000 ns (~200 m)";
            case 10: return "3000 ns (~300 m)";
            default: break;
        }
    }
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

// k2-plain-end
const char* termination_name(std::uint8_t t) {
    switch (t) {
        case 1: return K2_P("Passive, VCONN required");
        case 2: return K2_P("Active, one end");
        case 3: return K2_P("Active, both ends");
        default: return K2_P("Passive");
    }
}

}  // namespace

K2_HOT Pdo decode_pdo(std::uint32_t raw) {
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

K2_HOT std::uint16_t current_limit_ma(const Pdo& p, std::uint16_t mv) {
    if (p.type == PdoType::Avs && (p.raw >> 28) == 0xE)
        return static_cast<std::uint16_t>((mv <= 15000 ? (p.raw >> 10) & 0x3FF : p.raw & 0x3FF) * 10);
    return p.max_ma;
}

const char* wire_revision_name(std::uint8_t rev) {
    constexpr const char* names[] = {"unknown", "PD2.0", "PD3.x", "reserved"};   // k2-plain: tests compare the view
    return rev < 4 ? names[rev] : "unknown";   // k2-plain: tests compare the view
}

K2_HOT CableLimits cable_limits(const Emarker& e) {
    CableLimits out;
    if (!e.valid || e.num_vdos < 5 || (e.spec_rev != 1 && e.spec_rev != 2)) return out;
    out.observed = true;
    out.max_ma = e.max_current_a == 5 ? 5000 : 3000;
    // Above 20 V only for an EPR cable (parse_emarker: the EPR Capable bit beside 50 V and 5 A) answering in PD 3.x; an
    // active one also has to carry its VDO 2.
    if (e.spec_rev == 2 && e.epr_capable && e.max_voltage_v == 50 && out.max_ma == 5000 && (!e.is_active || e.vdo2_valid))
        out.max_mv = 50000;
    return out;
}

K2_HOT void decode_source_caps(SourceCaps& caps, std::uint16_t header, std::span<const std::uint8_t> payload,
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

K2_HOT Emarker parse_emarker(std::span<const std::uint8_t> body, unsigned num_do) {
    Emarker e;
    e.valid = true;
    e.max_current_a = 3;
    e.max_voltage_v = 20;
    e.max_power_w = 60;
    e.usb_lanes = 1;
    e.vendor_str = "Unknown";   // k2-plain: hot path
    e.speed_str = speed_name(0, 0);
    e.latency_str = latency_name(0, false);
    e.termination_str = termination_name(0);
    e.num_vdos = std::uint8_t(std::min<unsigned>({num_do, 6u, unsigned(body.size() / 4)}));
    for (unsigned i = 0; i < e.num_vdos; i++) e.raw_vdos[i] = le32(&body[4 * i]);
    const auto& v = e.raw_vdos;

    // VDO 0 is the VDM header; VDO 1 the ID header (VID, product type).
    // The layout of the cable VDOs follows the answer's Structured VDM version (VDM header bits 14:13): 00b is SVDM 1.0
    // (and PD 2.0), whose cable VDO has no Max VBUS field at 10:9 and no EPR Capable bit; 01b, and the invalid 10b / 11b
    // that a receiver reads as 01b, is SVDM 2.x.
    const bool svdm2 = num_do >= 1 && ((v[0] >> 13) & 3) != 0;
    std::uint8_t product = 0;   // ID header bits 29:27 on SOP': 3 passive cable, 4 active cable, 6 VPD
    if (num_do >= 2) {
        e.vid = v[1] & 0xFFFF;
        product = (v[1] >> 27) & 7;
        e.is_active = product == 4;
        e.vendor_str = vendor_name(e.vid);
    }
    if (num_do >= 3) e.xid = v[2];  // cert stat
    if (num_do >= 4) {              // product VDO
        e.pid = v[3] >> 16;
        e.hw_rev = (v[3] >> 8) & 0xFF;
        e.fw_rev = v[3] & 0xFF;
    }
    // Cable VDO 1, passive and active cables only: a VPD VDO (product type 6) carries impedances where a cable VDO has its
    // rating, so it keeps the defaults.
    if (num_do >= 5 && (product == 3 || product == 4)) {
        const std::uint32_t c = v[4];
        // Max VBUS: 11b = 50 V; 00b = 20 V; 01b / 10b are deprecated and read as 20 V. The legacy layouts are 20 V.
        e.max_voltage_v = svdm2 && ((c >> 9) & 3) == 3 ? 50 : 20;
        e.max_current_a = ((c >> 5) & 3) == 2 ? 5 : 3;
        const bool a5 = e.max_current_a >= 5;
        e.max_power_w = a5 ? (e.max_voltage_v >= 48 ? 240 : 100) : 60;
        e.vdo_version = svdm2 ? (c >> 21) & 7 : 0;   // SVDM 1.0 keeps Max VBUS in 23:21
        // EPR Capable (bit 17) exists in the passive VDO version 1.0 and the active VDO1 version 1.3, the active one with
        // VBUS through the cable (bit 4). An EPR cable is that bit with 50 V and 5 A.
        const bool epr_bit = svdm2 && ((c >> 17) & 1) &&
                             (e.is_active ? e.vdo_version == 3 && ((c >> 4) & 1) : e.vdo_version == 0);
        e.epr_capable = epr_bit && a5 && e.max_voltage_v == 50;
        // USB Highest Speed: a code above the layout's highest valid one counts as that one (Gen4 in SVDM 2.x; Gen2 in
        // SVDM 1.0 and in the active VDO1 before version 1.3).
        const std::uint8_t top = svdm2 && (!e.is_active || e.vdo_version >= 3) ? 4 : 2;
        e.speed_str = speed_name(std::min<std::uint8_t>(c & 7, top), e.vid);
        e.cable_hw = (c >> 28) & 0xF;
        e.cable_fw = (c >> 24) & 0xF;
        e.type_c_plug = ((c >> 18) & 3) == 2;
        e.latency_code = (c >> 13) & 0xF;
        e.latency_str = latency_name(e.latency_code, e.is_active);
        e.termination = (c >> 11) & 3;
        e.termination_str = termination_name(e.termination);
        e.vconn_required = e.termination != 0;
    }
    if (num_do >= 6 && e.is_active) {  // active cable VDO 2
        const std::uint32_t c = v[5];
        // Before VDO1 version 1.3, bits 10:8 and 2 are reserved (no USB4 Supported, no Optically Isolated) and bits 1:0
        // are the SuperSpeed signalling (01b = Gen 2 or higher), whose bit 0 is the later USB Gen bit.
        const bool v13 = e.vdo_version >= 3;
        e.vdo2_valid = true;
        e.max_temp_c = (c >> 24) & 0xFF;
        e.shutdown_temp_c = (c >> 16) & 0xFF;
        e.u3_power = (c >> 12) & 7;
        e.usb4_supported = v13 && !((c >> 8) & 1);   // active low
        e.usb_lanes = ((c >> 3) & 1) ? 2 : 1;
        e.optical_isolated = v13 && ((c >> 2) & 1);
        e.usb_gen2 = c & 1;
    }
    return e;
}

// k2-plain-begin: logged per message from the PD hot path
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
// k2-plain-end

}  // namespace k2::pd
