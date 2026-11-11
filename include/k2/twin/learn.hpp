// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Learning a twin: what the K2 keeps from a successful Identify (device alone) or from a Request it sniffed (inline), the
// auto name, and how a new observation merges into a twin that is already saved. Pure logic, host-tested in
// tests/test_twin_lib.cpp.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "k2/pd/pd.hpp"
#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "k2/twin/twin.hpp"

namespace k2::twin {

constexpr std::uint16_t id_hi_of(std::uint32_t id_header) { return std::uint16_t((id_header >> 23) & 0x1FFu); }

// The name a twin gets when nobody named it: "<vendor> <product type>" ("Apple Peripheral", "Lenovo Host"), the vendor alone
// when the type was not read, "VID xxxx" when the VID is not in the vendor table, "Device" with no VID. Always a name the
// rename rule accepts. The library adds " 2", " 3" ... when another twin already has it.
inline std::array<char, kNameMax + 1> auto_name(std::uint16_t vid, std::uint16_t id_hi) {
    std::array<char, kNameMax + 1> out{};
    auto put = [&](const char* s, std::size_t& n) {
        while (*s != 0 && n < kNameMax) out[n++] = *s++;
    };
    std::size_t n = 0;
    if (vid == 0) {
        put("Device", n);
        return out;
    }
    const char* vendor = pd::vendor_name(vid);
    if (std::strcmp(vendor, "Unknown") == 0) {
        put("VID ", n);
        for (int sh = 12; sh >= 0; sh -= 4) out[n++] = "0123456789ABCDEF"[(vid >> sh) & 0xF];
        return out;
    }
    put(vendor, n);
    const char* type = pd::id_product_type(std::uint32_t(id_hi) << 23);
    if (type[0] != 0) {
        put(" ", n);
        put(type, n);
    }
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
    return out;
}

// A twin from a finished Identify of a device that sat alone on the K2: its identity, its Sink_Capabilities, the wire
// revision. False when the run did not finish or learned no sink PDOs (nothing to predict from). A missing identity still
// gives a twin, without a VID (the library then never merges it with another one).
//
// `svids` is the device's Discover SVIDs answer (pd::VdmInfo::svids, as many as were read). With it the twin also keeps the
// declared USB data (twin.hpp `Declared`): the product-type VDOs (Discover Identity words 4 and 5, after the VDM header, ID
// Header, cert stat and product VDO), the first six SVIDs, and the mode VDO of each Discover Modes ACK the run recorded.
static_assert(kMaxDeclModes == pd::kIdModeSvids, "the twin keeps every mode VDO the Identify run records");
inline bool from_identify(const pd::Identity& id, const pd::IdStatus& st, std::uint8_t pd_rev, std::span<const std::uint16_t> svids,
                          Twin& out) {
    out = Twin{};
    if (st.state != pd::IdState::Done || st.num_sink_pdos == 0) return false;
    if (id.valid) {
        out.vid = id.vid;
        out.pid = id.pid;
        out.id_hi = id_hi_of(id.id_header);
        constexpr std::size_t kFirstPt = 4;   // vdos[0] VDM header, [1] ID Header, [2] cert stat, [3] product VDO
        const std::size_t have = std::min<std::size_t>(id.num_vdos, id.vdos.size());
        for (std::size_t i = kFirstPt; i < have && out.decl.n_pt < kMaxProductVdos; ++i) out.decl.pt[out.decl.n_pt++] = id.vdos[i];
    }
    for (std::size_t i = 0; i < svids.size() && out.decl.n_svids < kMaxDeclSvids; ++i) out.decl.svids[out.decl.n_svids++] = svids[i];
    const std::size_t nm = std::min<std::size_t>(st.num_modes, kMaxDeclModes);
    for (std::size_t i = 0; i < nm; ++i) {
        out.decl.mode_svid[i] = st.mode_svid[i];
        out.decl.mode_vdo[i] = st.mode_vdo[i];
    }
    out.decl.n_modes = std::uint8_t(nm);
    out.pd_rev = pd_rev;
    out.num_pdos = std::uint8_t(std::min<std::size_t>(st.num_sink_pdos, kMaxSinkPdos));
    for (std::size_t i = 0; i < out.num_pdos; ++i) out.pdos[i] = st.sink_pdos[i];
    return true;
}

// Without the SVIDs (the engine kept none): identity words and the recorded modes only.
inline bool from_identify(const pd::Identity& id, const pd::IdStatus& st, std::uint8_t pd_rev, Twin& out) {
    return from_identify(id, st, pd_rev, std::span<const std::uint16_t>{}, out);
}

// A twin from what the K2 saw on the wire between a device and its own charger: the Request the charger accepted (PS_RDY
// seen) and the source PDO it pointed at, the device's identity when its Discover Identity was seen, and the device's own
// Sink_Capabilities when the charger asked for them while the K2 watched. False without a ready contract on a position
// inside the source list.
inline bool from_sniffed(const pd::Contract& c, const pd::SourceCaps& caps, const pd::VdmInfo& vdm, const pd::SinkCapsSeen& seen, Twin& out) {
    out = Twin{};
    if (!c.valid || !c.ready || c.rdo == 0 || !caps.valid) return false;
    if (c.pos == 0 || c.pos > caps.num_pdos) return false;
    out.rdo = c.rdo;
    out.rdo_pdo = caps.pdos[c.pos - 1].raw;
    if (out.rdo_pdo == 0) return false;
    if (vdm.identity.valid) {
        out.vid = vdm.identity.vid;
        out.pid = vdm.identity.pid;
        out.id_hi = id_hi_of(vdm.identity.id_header);
    }
    if (seen.valid && seen.n > 0) {
        out.num_pdos = std::uint8_t(std::min<std::size_t>(seen.n, kMaxSinkPdos));
        for (std::size_t i = 0; i < out.num_pdos; ++i) out.pdos[i] = seen.raw[i];
    }
    return true;
}

// Folds a new observation `in` into the saved twin of the same device. The saved name stays (a rename wins). What `in` knows
// replaces what the twin had; what `in` does not know (no sink PDOs from a sniff, no Request from an Identify, no identity
// words) leaves the saved value alone, so the two ways of learning add up instead of undoing each other.
inline void merge(Twin& saved, const Twin& in) {
    if (in.num_pdos > 0) {
        saved.num_pdos = in.num_pdos;
        saved.pdos = in.pdos;
    }
    if (in.pd_rev != 0) saved.pd_rev = in.pd_rev;
    if (in.id_hi != 0) saved.id_hi = in.id_hi;
    if (in.has_request()) {
        saved.rdo = in.rdo;
        saved.rdo_pdo = in.rdo_pdo;
    }
    if (in.decl.any()) saved.decl = in.decl;   // a device that answered no USB words leaves the saved block alone
    if (in.obs.valid) saved.obs = in.obs;
}

}  // namespace k2::twin
