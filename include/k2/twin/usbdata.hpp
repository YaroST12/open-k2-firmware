// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB data properties of a twin: decoders for the raw words it stores and the wording for the screen and the JSON. Pure logic,
// host-tested in tests/test_twin_usbdata.cpp.
//
// Two blocks, and they are told apart everywhere (the screen says "can" and "used"):
//  - Declared (twin.hpp `Declared`): what the device SAID about itself when the K2 asked it alone (Discover Identity product-type
//    VDOs, Discover SVIDs, the first mode VDO of 0xFF01 DisplayPort and 0x8087 Thunderbolt). A capability, never a link.
//  - Observed (twin.hpp `Observed`): what the K2 SAW the device and its partner negotiate while it sat inline (Enter_USB,
//    Enter_Mode, DP Configure). The speed is what the PD messages configured.
// The SuperSpeed and USB4 lanes are not wired to the K2 (it owns CC, D+/D- and VBUS), so no rate here is ever measured. USB 2.0
// on D+/D- is seen only as line activity.
// The wording comes from k2/pd/usb_link.hpp, the classifier the Home screen uses: a declared block is replayed into it as the
// Discover Identity / SVIDs messages it was read from, so the twin and the live screen can never disagree about a word.
//
// Provenance: the UFP / DFP / DP / Thunderbolt VDO bit positions and the Enter_USB data object layout are inferred from the
// words real devices sent (the same positions usb_link.cpp reads); nothing here is proven by a certification.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "k2/pd/usb_link.hpp"
#include "k2/twin/twin.hpp"

namespace k2::twin {

// ---- decoders -----------------------------------------------------------------------------------------------------------

struct UfpVdo {
    bool present = false;
    std::uint8_t cap = 0;          // device capability 27:24: b0 USB 2.0, b1 Billboard, b2 USB 3.2, b3 USB4
    std::uint8_t alt = 0;          // alternate modes 5:3: b0 Thunderbolt 3
    std::uint8_t speed = 0;        // USB highest speed 2:0: 0 USB 2.0, 1 Gen1 5G, 2 Gen2 10G (20G when USB4 capable), 3 USB4 40G, 4 USB4 Gen4 80G
    bool vconn_required = false;   // bit 7
    bool vbus_required = false;    // bit 6
    std::uint8_t vconn_power = 0;  // 10:8, the code as sent
    bool usb4() const { return (cap & 8) != 0; }
    bool billboard() const { return (cap & 2) != 0; }
};

struct DfpVdo {
    bool present = false;
    std::uint8_t cap = 0;   // host capability 26:24: b0 USB 2.0, b1 USB 3.2, b2 USB4
    std::uint8_t port = 0;  // port number 4:0
};

// Which of the stored product-type words is the UFP VDO and which the DFP VDO follows Discover Identity: the ID Header's UFP
// type (hub, peripheral) means a UFP VDO comes first; a DFP type (hub, host) means a DFP VDO follows it (or comes first).
inline UfpVdo ufp_vdo(std::uint16_t id_hi, const Declared& d) {
    UfpVdo u;
    const unsigned type = (id_hi >> 4) & 7u;
    if ((type != 1 && type != 2) || d.n_pt < 1) return u;
    const std::uint32_t w = d.pt[0];
    if ((w & 7u) > 4) return u;
    u.present = true;
    u.cap = std::uint8_t((w >> 24) & 0xF);
    u.alt = std::uint8_t((w >> 3) & 7);
    u.speed = std::uint8_t(w & 7);
    u.vconn_required = ((w >> 7) & 1) != 0;
    u.vbus_required = ((w >> 6) & 1) != 0;
    u.vconn_power = std::uint8_t((w >> 8) & 7);
    return u;
}

inline DfpVdo dfp_vdo(std::uint16_t id_hi, const Declared& d) {
    DfpVdo f;
    const unsigned ufp = (id_hi >> 4) & 7u, dfp = id_hi & 7u;
    if (dfp != 1 && dfp != 2) return f;
    const std::size_t at = (ufp == 1 || ufp == 2) ? 1 : 0;
    if (d.n_pt <= at) return f;
    const std::uint32_t w = d.pt[at];
    f.present = true;
    f.cap = std::uint8_t((w >> 24) & 7);
    f.port = std::uint8_t(w & 0x1F);
    return f;
}

// The DisplayPort mode VDO (Discover Modes ACK of SVID 0xFF01).
struct DpCaps {
    std::uint8_t port = 0;         // 1:0: b0 UFP_D capable (a DP sink), b1 DFP_D capable (a DP source)
    bool receptacle = false;       // bit 6
    bool usb2_unused = false;      // bit 7: USB 2.0 signalling not used
    std::uint8_t dfp_pins = 0;     // 15:8 pin assignments the DFP_D side supports, A = b0 .. F = b5
    std::uint8_t ufp_pins = 0;     // 23:16 the same for the UFP_D side
    bool ufp_d() const { return (port & 1) != 0; }
    bool dfp_d() const { return (port & 2) != 0; }
    std::uint8_t pins() const { return std::uint8_t(((port & 1) ? ufp_pins : 0) | ((port & 2) ? dfp_pins : 0)); }
    bool four_lane() const { return (pins() & 0x14) != 0; }   // pin assignment C (b2) or E (b4)
};
inline DpCaps dp_caps(std::uint32_t w) {
    DpCaps c;
    c.port = std::uint8_t(w & 3);
    c.receptacle = ((w >> 6) & 1) != 0;
    c.usb2_unused = ((w >> 7) & 1) != 0;
    c.dfp_pins = std::uint8_t((w >> 8) & 0x3F);
    c.ufp_pins = std::uint8_t((w >> 16) & 0x3F);
    return c;
}

// The Thunderbolt mode VDO (Discover Modes ACK of SVID 0x8087).
struct TbtMode {
    std::uint16_t alt_mode = 0;   // 15:0, 1 = Thunderbolt 3 compatible
    bool adapter = false;         // bit 16
};
inline TbtMode tbt_mode(std::uint32_t w) { return {std::uint16_t(w & 0xFFFF), ((w >> 16) & 1) != 0}; }

// The Enter_USB data object the host sends before it turns the SuperSpeed lanes to USB 3.2 or USB4.
struct EnterUsb {
    std::uint8_t mode = 0;           // 30:28: 0 USB 2.0, 1 USB 3.2, 2 USB4
    std::uint8_t cable_speed = 0;    // 23:21: 0 USB 2.0, 1 Gen1, 2 Gen2, 3 USB4 Gen3, 4 USB4 Gen4 (80G)
    std::uint8_t cable_type = 0;     // 20:19 as sent
    std::uint8_t cable_current = 0;  // 18:17 as sent
    bool pcie = false;               // 16: the host offers PCIe tunnelling
    bool dp = false;                 // 15: DisplayPort tunnelling
    bool tbt = false;                // 14: Thunderbolt 3 compatibility
    bool host_present = false;       // 13
};
inline EnterUsb enter_usb(std::uint32_t w) {
    EnterUsb e;
    e.mode = std::uint8_t((w >> 28) & 7);
    e.cable_speed = std::uint8_t((w >> 21) & 7);
    e.cable_type = std::uint8_t((w >> 19) & 3);
    e.cable_current = std::uint8_t((w >> 17) & 3);
    e.pcie = ((w >> 16) & 1) != 0;
    e.dp = ((w >> 15) & 1) != 0;
    e.tbt = ((w >> 14) & 1) != 0;
    e.host_present = ((w >> 13) & 1) != 0;
    return e;
}

// ---- links ----------------------------------------------------------------------------------------------------------------

namespace detail {
inline void put_le(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (8 * i)); }
// Feeds one PD message to a classifier as if it came off the wire from the partner (SOP).
inline void feed(usblink::Classifier& c, unsigned type, std::span<const std::uint32_t> w) {
    std::uint8_t b[32];
    const std::size_t n = w.size() < 8 ? w.size() : 8;
    for (std::size_t i = 0; i < n; ++i) put_le(&b[4 * i], w[i]);
    c.on_message(usblink::Sop::Sop, std::uint16_t((n << 12) | (2u << 6) | type), {b, 4 * n});
}
constexpr unsigned kMsgVdm = 0x0F, kMsgEnterUsb = 8, kCtlAccept = 3;
constexpr std::uint32_t kHdrIdentityAck = 0xFF00A041u, kHdrSvidsAck = 0xFF00A042u;

// Cuts the confidence tag " (adv)" / " (neg)" / " (hint)" that usblink::format() appends.
inline void strip_conf(char* s) {
    char* cut = nullptr;
    for (char* p = s; *p != 0; ++p)
        if (p[0] == ' ' && p[1] == '(') cut = p;
    if (cut != nullptr) *cut = 0;
}
inline void put_text(std::span<char> out, const char* s) {
    if (out.empty()) return;
    std::size_t n = 0;
    for (; s[n] != 0 && n + 1 < out.size(); ++n) out[n] = s[n];
    out[n] = 0;
}
}  // namespace detail

// The link a device DECLARES: its Discover Identity and SVIDs replayed into the live classifier. Confidence Advertised.
inline usblink::Link declared_link(std::uint16_t id_hi, const Declared& d) {
    usblink::Classifier c;
    if (d.n_pt > 0 || id_hi != 0) {
        std::uint32_t w[8] = {detail::kHdrIdentityAck, std::uint32_t(id_hi) << 23, 0, 0};
        std::size_t n = 4;
        for (std::size_t i = 0; i < d.n_pt && i < kMaxProductVdos; ++i) w[n++] = d.pt[i];
        detail::feed(c, detail::kMsgVdm, {w, n});
    }
    if (d.n_svids > 0) {
        std::uint32_t w[1 + (kMaxDeclSvids + 1) / 2] = {detail::kHdrSvidsAck};
        std::size_t n = 1;
        for (std::size_t i = 0; i < d.n_svids && i < kMaxDeclSvids; i += 2)
            w[n++] = (std::uint32_t(d.svids[i]) << 16) | (i + 1 < d.n_svids ? d.svids[i + 1] : 0u);
        detail::feed(c, detail::kMsgVdm, {w, n});
    }
    return c.link();
}

// The link an Enter_USB data object configures (the host asked, the partner Accepted).
inline usblink::Link enter_usb_link(std::uint32_t word) {
    usblink::Classifier c;
    detail::feed(c, detail::kMsgEnterUsb, {&word, 1});
    detail::feed(c, detail::kCtlAccept, {});
    return c.link();
}

inline void enter_usb_text(std::uint32_t word, std::span<char> out) {
    char b[40];
    usblink::format(enter_usb_link(word), b);
    detail::strip_conf(b);
    detail::put_text(out, b);
}

// ---- observed -------------------------------------------------------------------------------------------------------------

// Captures what a classifier negotiated. False (and `out` untouched) when nothing was negotiated: advertised capabilities and
// line hints are not an observation.
inline bool make_observed(const usblink::Link& l, const usblink::Raw& raw, std::uint16_t partner_vid, Observed& out) {
    if (l.conf != usblink::Confidence::Negotiated) return false;
    if (!raw.eu_ok && !raw.tbt_active && !raw.dp_active) return false;
    Observed o;
    o.valid = true;
    o.partner_vid = partner_vid;
    o.family = std::uint8_t(l.family);
    o.tbt = std::uint8_t(l.tbt);
    o.mbps = l.mbps;
    o.dp_pin = raw.dp_pin;
    if (l.dp) o.flags |= kObsDp;
    if (l.dp_4lane) o.flags |= kObsDp4;
    if (l.dp_2lane) o.flags |= kObsDp2;
    if (l.cable_limited) o.flags |= kObsCableLimited;
    if (raw.eu_ok) { o.flags |= kObsEnterUsb; o.enter_usb = raw.eu_vdo; }
    if (raw.tbt_active) { o.flags |= kObsTbtEnter; o.tbt_enter = raw.tbt_enter_vdo; }
    out = o;
    return true;
}

inline usblink::Link observed_link(const Observed& o) {
    usblink::Link l;
    l.family = usblink::Family(o.family);
    l.tbt = usblink::Tbt(o.tbt);
    l.mbps = o.mbps;
    l.conf = usblink::Confidence::Negotiated;
    l.dp = (o.flags & kObsDp) != 0;
    l.dp_4lane = (o.flags & kObsDp4) != 0;
    l.dp_2lane = (o.flags & kObsDp2) != 0;
    l.cable_limited = (o.flags & kObsCableLimited) != 0;
    return l;
}

// "USB4 40G TBT4", "USB 3.2 Gen2 10G +DP 2-lane", "USB 2.0 480M +DP 4-lane": what the PD messages configured.
inline void observed_text(const Observed& o, std::span<char> out) {
    char b[48];
    usblink::format(observed_link(o), b);
    detail::strip_conf(b);
    const std::size_t n = std::strlen(b);
    const char* tail = (o.flags & kObsDp4) ? " 4-lane" : (o.flags & kObsDp2) ? " 2-lane" : "";
    if (n + std::strlen(tail) + 1 <= sizeof b) std::memcpy(b + n, tail, std::strlen(tail) + 1);
    detail::put_text(out, b);
}

// ---- declared wording -------------------------------------------------------------------------------------------------------

// "USB4 40G, DP, TB": the best speed the device declared and the alternate modes it listed; "-" when it declared nothing.
inline void declared_text(std::uint16_t id_hi, const Declared& d, std::span<char> out) {
    const usblink::Link l = declared_link(id_hi, d);
    char sp[40], alt[24];
    sp[0] = 0;
    if (l.family != usblink::Family::Unknown) {
        usblink::format(l, sp);
        detail::strip_conf(sp);
    }
    const bool has_alt = usblink::format_alt(l, alt, false) != usblink::AltState::None;
    char b[72];
    std::size_t n = 0;
    auto put = [&](const char* s) { for (; *s != 0 && n + 1 < sizeof b; ++s) b[n++] = *s; };
    put(sp);
    if (has_alt) {
        if (sp[0] != 0) put(", ");
        put(alt);
    }
    if (n == 0) put("-");
    b[n] = 0;
    detail::put_text(out, b);
}

// What the K2 may say about the Thunderbolt level. "TB4" and "TB5" are certification levels it cannot prove: it infers
// "TB4 class" from USB4 at 40G or more with the 0x8087 mode listed (or entered), "TB5 class" from USB4 Gen4 80G.
inline const char* tb_class(const usblink::Link& l) {
    if (l.family == usblink::Family::Usb4v2) return "TB5 class";
    if (l.family == usblink::Family::Usb4 && l.mbps >= 40000 && (l.tbt_adv || l.tbt != usblink::Tbt::None)) return "TB4 class";
    return "";
}

// "DP sink, pins C D": the role the DisplayPort mode VDO declares and the pin assignments it supports.
inline void dp_caps_text(const DpCaps& c, std::span<char> out) {
    char b[40];
    std::size_t n = 0;
    auto put = [&](const char* s) { for (; *s != 0 && n + 1 < sizeof b; ++s) b[n++] = *s; };
    put(c.port == 3 ? "DP source+sink" : c.port == 2 ? "DP source" : c.port == 1 ? "DP sink" : "DP");
    if (c.pins() != 0) {
        put(", pins");
        for (unsigned i = 0; i < 6; ++i)
            if (c.pins() & (1u << i)) { put(" "); char l[2] = {char('A' + i), 0}; put(l); }
    }
    b[n] = 0;
    detail::put_text(out, b);
}

}  // namespace k2::twin
