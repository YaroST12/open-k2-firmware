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
#include "k2/pd/pd_interrogate.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "k2/twin/twin.hpp"

namespace k2::twin {

constexpr std::uint16_t id_hi_of(std::uint32_t id_header) { return std::uint16_t((id_header >> 23) & 0x1FFu); }

// The name a twin gets when nobody named it: "<vendor> <max watts>W" ("Google 33W", "Lenovo 65W"), the vendor alone when no
// sink level was read, "VID xxxx 33W" when the VID is not in the vendor table, "Device 33W" with no VID. `want_w` is the most
// any of its sink PDOs takes (twin::want_mw, whole watts). The vendor part is shortened so the watts and the library's " 2",
// " 3" ... suffix always fit. Always a name the rename rule accepts.
inline constexpr std::size_t kAutoVendorMax = kNameMax - 5 - 3;   // " 240W" and " 12"
inline std::array<char, kNameMax + 1> auto_name(std::uint16_t vid, std::uint32_t want_w) {
    std::array<char, kNameMax + 1> out{};
    std::size_t n = 0;
    auto put = [&](const char* s, std::size_t cap) {
        while (*s != 0 && n < cap) out[n++] = *s++;
    };
    if (vid == 0) {
        put("Device", kAutoVendorMax);
    } else if (const char* vendor = pd::vendor_name(vid); std::strcmp(vendor, "Unknown") == 0) {
        put("VID ", kAutoVendorMax);
        for (int sh = 12; sh >= 0; sh -= 4) out[n++] = "0123456789ABCDEF"[(vid >> sh) & 0xF];
    } else {
        put(vendor, kAutoVendorMax);
    }
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
    if (want_w != 0) {
        char t[8];
        int k = 0;
        std::uint32_t v = want_w > 999 ? 999 : want_w;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v != 0);
        out[n++] = ' ';
        while (k != 0) out[n++] = t[--k];
        out[n++] = 'W';
    }
    out[n] = 0;
    return out;
}
// The most watts any of the twin's sink PDOs takes (fixed V x I, PPS top voltage x current), rounded to whole watts.
inline std::uint32_t want_w(const Twin& t) {
    std::uint64_t best = 0;
    for (std::size_t i = 0; i < t.num_pdos && i < t.pdos.size(); ++i) {
        const pd::Pdo s = pd::decode_pdo(t.pdos[i]);
        if (s.type == pd::PdoType::Fixed || s.type == pd::PdoType::Pps)
            best = std::max<std::uint64_t>(best, std::uint64_t(s.max_mv) * s.max_ma);
    }
    return std::uint32_t((best + 500000u) / 1000000u);
}
inline std::array<char, kNameMax + 1> auto_name(const Twin& t) { return auto_name(t.vid, want_w(t)); }

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

// A twin from an inline interrogation (k2/pd/pd_interrogate.hpp): what the device answered while the K2 asked it on its own
// charger's link (identity and USB words, sink PDOs, Revision, Sink_Capabilities_Extended) plus the contract the K2 sniffed
// before asking (the second one, after the final reset, when no first was seen). The device's revision is its Request's.
// False while the run is still going, or when it read neither a contract nor sink PDOs; an aborted run keeps what it read.
static_assert(kSinkExtBytes == pd::kIqSinkExtBytes, "the twin keeps the whole extended block the interrogation reads");
inline bool from_interrogated(const pd::IqStatus& st, Twin& out) {
    out = Twin{};
    if (!pd::iq_finished(st.phase)) return false;
    const pd::IqContract& c = st.first.ready ? st.first : st.second;
    if (!c.ready && st.num_sink_pdos == 0) return false;
    if (st.id.valid) {
        out.vid = st.id.vid;
        out.pid = st.id.pid;
        out.id_hi = id_hi_of(st.id.id_header);
        constexpr std::size_t kFirstPt = 4;   // as from_identify: VDM header, ID Header, cert stat, product VDO, then these
        const std::size_t have = std::min<std::size_t>(st.id.num_vdos, st.id.vdos.size());
        for (std::size_t i = kFirstPt; i < have && out.decl.n_pt < kMaxProductVdos; ++i) out.decl.pt[out.decl.n_pt++] = st.id.vdos[i];
    }
    for (std::size_t i = 0; i < st.num_svids && i < st.svids.size() && out.decl.n_svids < kMaxDeclSvids; ++i)
        out.decl.svids[out.decl.n_svids++] = st.svids[i];
    const std::size_t nm = std::min<std::size_t>(st.num_modes, kMaxDeclModes);
    for (std::size_t i = 0; i < nm; ++i) {
        out.decl.mode_svid[i] = st.mode_svid[i];
        out.decl.mode_vdo[i] = st.mode_vdo[i];
    }
    out.decl.n_modes = std::uint8_t(nm);
    out.pd_rev = c.ready && c.dev_rev != 0 ? c.dev_rev : st.rev;
    out.num_pdos = std::uint8_t(std::min<std::size_t>(st.num_sink_pdos, kMaxSinkPdos));
    for (std::size_t i = 0; i < out.num_pdos; ++i) out.pdos[i] = st.sink_pdos[i];
    if (c.ready && c.rdo != 0 && c.src_pdo != 0) {
        out.rdo = c.rdo;
        out.rdo_pdo = c.src_pdo;
    }
    out.revision = st.revision;
    out.sink_ext_len = std::uint8_t(std::min<std::size_t>(st.sink_ext_len, kSinkExtBytes));
    std::copy_n(st.sink_ext.begin(), out.sink_ext_len, out.sink_ext.begin());
    return true;
}

// The merge key: is `in` a new observation of the saved twin's device? (docs/DEVICE_TWIN.md section 4, "Same device")
//  1. Both carry an identity (VID != 0): the same VID and PID, nothing else.
//  2. One carries an identity, the other not: never. A sink PDO list or a Request is no proof of WHICH identified device it
//     was (two Google phones list the same PDOs; two phones ask the same charger for the same 9 V 3 A).
//  3. Neither carries one: equal sink PDO lists when both have one; else the sniffed Request (the RDO and the source PDO it
//     pointed at) when both have one. Nothing to compare = never the same device: two devices that never said who they are
//     do not merge just because both read 0000:0000.
inline bool same_pdos(const Twin& a, const Twin& b) {
    if (a.num_pdos == 0 || a.num_pdos != b.num_pdos) return false;
    for (std::size_t i = 0; i < a.num_pdos && i < a.pdos.size(); ++i)
        if (a.pdos[i] != b.pdos[i]) return false;
    return true;
}
inline bool same_device(const Twin& saved, const Twin& in) {
    if (saved.vid != 0 && in.vid != 0) return saved.vid == in.vid && saved.pid == in.pid;
    if (saved.vid != 0 || in.vid != 0) return false;
    if (saved.num_pdos != 0 && in.num_pdos != 0) return same_pdos(saved, in);
    return saved.has_request() && in.has_request() && saved.rdo == in.rdo && saved.rdo_pdo == in.rdo_pdo;
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
    if (in.revision != 0) saved.revision = in.revision;
    if (in.sink_ext_len != 0) {
        saved.sink_ext_len = in.sink_ext_len;
        saved.sink_ext = in.sink_ext;
    }
}

}  // namespace k2::twin
