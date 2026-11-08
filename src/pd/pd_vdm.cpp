// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_vdm.hpp"

#include <algorithm>

#include "k2/pd/pd.hpp"

namespace k2::pd {

const char* id_product_type(std::uint32_t h) {
    switch ((h >> 27) & 7u) {
        case 1: return "Hub";
        case 2: return "Peripheral";
        case 3: return "Power bank";
        default: break;
    }
    switch ((h >> 23) & 7u) {
        case 1: return "Hub";
        case 2: return "Host";
        case 3: return "Charger";
        default: return "";
    }
}

const char* id_usb_role(std::uint32_t h) {
    const bool host = h >> 31, dev = (h >> 30) & 1u;
    return host && dev ? "Host+Device" : host ? "Host" : dev ? "Device" : "";
}

namespace {

constexpr std::uint32_t le32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24);
}

struct Named { std::uint16_t id; const char* name; };

// SVIDs of phone/PC vendors and charging protocols, for labels; 0x5FC9 is the UFCS SVID.
constexpr std::array<Named, 22> kSvids{{
    {0x2717, "Xiaomi"},  {0x12D1, "Huawei"},  {0x22D9, "OPPO"},    {0x2D95, "Vivo"},
    {0x17EF, "Lenovo"},  {0x22B8, "Motorola"}, {0x04E8, "Samsung"}, {0x18D1, "Google"},
    {0x413C, "Dell"},    {0x03F0, "HP"},      {0x0B05, "ASUS"},    {0x0489, "Foxconn"},
    {0x05AC, "Apple"},   {0x8086, "Intel"},   {0x8087, "Intel/TB"}, {0xFF00, "PD SID"},
    {0xFF01, "DisplayPort"}, {0x2E87, "Cuktech"}, {0x291A, "Anker"}, {0x2B89, "Baseus"},
    {0x2B16, "UGREEN"},  {0x5FC9, "UFCS"},
}};

constexpr std::array<Named, 43> kApple{{
    {0x7108, "iPad Air 4"}, {0x710C, "iPad mini 6"}, {0x710D, "iPad Air 5"}, {0x710E, "iPad Pro 11"},
    {0x7112, "iPad 10"}, {0x7116, "iPad mini A17 Pro"}, {0x0707, "iPad Air"}, {0x711D, "iPad Air"},
    {0x7117, "iPad Pro"}, {0x711E, "iPad Pro"}, {0x711B, "iPad A16"}, {0x711C, "iPad Air 11 M2"},
    {0x7309, "MacBook Pro 16"}, {0x7315, "MacBook Pro 16"}, {0x731E, "MacBook Pro 16"},
    {0x730B, "MacBook Air"}, {0x7310, "MacBook Air"}, {0x7318, "MacBook Air"}, {0x731B, "MacBook Air"},
    {0x7311, "MacBook Pro 14"}, {0x7319, "MacBook Pro 14"}, {0x731D, "MacBook Pro 14"},
    {0x731A, "MacBook Pro 14/16"}, {0x7321, "MacBook Neo"}, {0x7323, "MacBook Air 13"},
    {0x7507, "iPhone 13 mini"}, {0x7509, "iPhone 13"}, {0x750A, "iPhone 13 PM"},
    {0x750D, "iPhone 14 Plus"}, {0x750E, "iPhone 14"}, {0x750F, "iPhone 14 PM"},
    {0x7510, "iPhone 15"}, {0x7511, "iPhone 15 Plus"}, {0x7512, "iPhone 15 Pro"},
    {0x7513, "iPhone 15 PM"}, {0x7515, "iPhone 16/16Plus"}, {0x7517, "iPhone 16 P/PM"},
    {0x7518, "iPhone Air"}, {0x7519, "iPhone 17 P/PM"}, {0x751B, "iPhone 17"},
    {0x0159, "iPhone 17e"}, {0x751C, "iPhone 16e"}, {0x751F, "iPhone 18 P/PM"},
}};

template <std::size_t N>
const char* find(const std::array<Named, N>& t, std::uint16_t id) {
    const auto it = std::ranges::find(t, id, &Named::id);
    return it == t.end() ? nullptr : it->name;
}

void on_identity(VdmInfo& info, std::span<const std::uint32_t> v) {
    if (v.size() < 2) return;
    Identity id;
    id.valid = true;
    id.id_header = v[1];
    id.vid = v[1] & 0xFFFF;
    id.vendor = svid_name(id.vid);
    if (v.size() >= 3) id.cert_stat = v[2];
    if (v.size() >= 4) {
        id.pid = v[3] >> 16;
        id.bcd = v[3] & 0xFFFF;
        if (id.vid == 0x05AC) id.model = apple_model(id.pid);
    }
    id.num_vdos = std::uint8_t(std::min(v.size(), id.vdos.size()));
    std::copy_n(v.begin(), id.num_vdos, id.vdos.begin());
    info.identity = id;
}

void on_svids(VdmInfo& info, std::span<const std::uint32_t> v) {
    info.num_svids = 0;
    for (std::size_t i = 1; i < v.size(); i++) {
        for (const std::uint16_t s : {std::uint16_t(v[i] >> 16), std::uint16_t(v[i] & 0xFFFF)}) {
            if (s == 0) return;  // a zero SVID ends the list
            if (info.num_svids < kMaxSvids) info.svids[info.num_svids++] = s;
        }
    }
}

}  // namespace

const char* svid_name(std::uint16_t svid) {
    if (const char* n = find(kSvids, svid)) return n;
    return vendor_name(svid);
}

const char* apple_model(std::uint16_t pid) {
    if (pid == 0x0000 || pid == 0xFFFF || pid == 0x00FF || pid == 0xFF00) return nullptr;
    return find(kApple, pid);
}

void parse_vdm(VdmInfo& info, std::span<const std::uint8_t> payload) {
    if (payload.size() < 4) return;
    std::array<std::uint32_t, 7> v{};
    const std::size_t n = std::min(payload.size() / 4, v.size());
    for (std::size_t i = 0; i < n; i++) v[i] = le32(&payload[4 * i]);
    const std::span<const std::uint32_t> vdos(v.data(), n);

    const std::uint32_t hdr = v[0];
    const std::uint16_t svid = hdr >> 16;
    const bool structured = (hdr >> 15) & 1;
    const unsigned cmd_type = (hdr >> 6) & 3, cmd = hdr & 0x1F;

    if (svid == 0x2717 && ((cmd >= 1 && cmd <= 8) || cmd == 0x0A)) {  // Xiaomi proprietary commands
        info.xiaomi = true;
        info.xiaomi_cmd = std::uint8_t(cmd);
    }
    if (svid == 0x2E87 && n >= 3 && v[1] == 0x30383041 && v[2] == 0x424E0000) info.cuktech = true;

    if (!structured) return;
    if (cmd_type == 0) {  // REQ from the source: count it, we do not reply
        info.requests++;
        return;
    }
    if (cmd_type != 1 || svid != kSvidPd) {
        if (cmd_type == 1 && cmd == 3) {  // Discover Modes ACK for a vendor SVID
            info.modes_svid = svid;
            info.num_modes = std::uint8_t(std::min(n - 1, info.modes.size()));
            std::copy_n(vdos.begin() + 1, info.num_modes, info.modes.begin());
        }
        return;
    }
    if (cmd == 1) on_identity(info, vdos);
    else if (cmd == 2) on_svids(info, vdos);
}

}  // namespace k2::pd
