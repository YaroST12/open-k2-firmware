// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/cable_full.hpp"
#include "k2/core/strpack.hpp"

namespace k2::cable_full {
namespace {

using emk_learn::Cls;
using emk_learn::Reply;
using emk_learn::Request;

enum class Got : std::uint8_t { Lost, Silent, Ack, Refused };

// One structured request to `sop` in the answered variant; the reply is classified against its header.
Got ask(emk_learn::ExchangeFn fn, void* ctx, emk_variant::Variant var, std::uint8_t sop, std::uint16_t svid, std::uint8_t cmd,
        Reply& rep) {
    Request req;
    req.sop = sop;
    req.spec_rev = var.spec_rev;
    req.n = 1;
    req.w[0] = emk_learn::vdm_request(svid, cmd, var);
    rep = {};
    if (!fn(ctx, req, rep)) return Got::Lost;
    switch (emk_learn::classify(req.w[0], rep)) {
        case Cls::None: return Got::Silent;
        case Cls::Ack: return Got::Ack;
        default: return Got::Refused;
    }
}

// Discover SVIDs, then Discover Modes of each SVID listed. False when the plug stopped being acknowledged.
bool explore(emk_learn::ExchangeFn fn, void* ctx, emk_variant::Variant var, std::uint8_t sop, Plug& p) {
    Reply rep;
    const Got g = ask(fn, ctx, var, sop, 0xFF00, 2, rep);
    if (g == Got::Lost) return false;
    if (g != Got::Ack) return true;
    p.svids_ack = true;
    // Two SVIDs per data object, upper half first; a zero half ends the list.
    bool more = true;
    for (unsigned i = 1; i < rep.n && more; i++) {
        for (const std::uint32_t half : {rep.w[i] >> 16, rep.w[i] & 0xFFFFu}) {
            more = half != 0 && p.nsvid < kMaxSvids;
            if (!more) break;
            p.svid[p.nsvid++] = static_cast<std::uint16_t>(half);
        }
    }
    for (unsigned i = 0; i < p.nsvid; i++) {
        const Got m = ask(fn, ctx, var, sop, p.svid[i], 3, rep);
        if (m == Got::Lost) return false;
        if (m != Got::Ack || rep.n < 2) continue;
        p.mode[i] = rep.w[1];
        p.mode_ack = static_cast<std::uint8_t>(p.mode_ack | (1u << i));
    }
    return true;
}

}  // namespace

bool read(emk_learn::ExchangeFn fn, void* ctx, emk_variant::Variant var, bool active, Full& out) {
    out = {};
    out.valid = true;
    if (!explore(fn, ctx, var, kSopPrime, out.p1)) return false;
    if (!active) return true;
    // The spec has no Discover Identity on SOP'': a refusal or silence is the normal answer, and nothing here is fatal.
    Reply rep;
    if (ask(fn, ctx, var, kSopDouble, 0xFF00, 1, rep) != Got::Ack) return true;
    out.id2_ack = true;
    out.id2_n = static_cast<std::uint8_t>(rep.n - 1u > out.id2.size() ? out.id2.size() : rep.n - 1u);
    for (unsigned i = 0; i < out.id2_n; i++) out.id2[i] = rep.w[1 + i];
    explore(fn, ctx, var, kSopDouble, out.p2);
    return true;
}

const char* u3_power_text(std::uint8_t code) {
    static constexpr const char* kText[] = {K2_P(">10mW"), K2_P("5-10mW"), K2_P("1-5mW"), K2_P("0.5-1mW"), K2_P("0.2-0.5mW"), K2_P("50-200uW"), K2_P("<50uW")};
    return code < 7 ? kText[code] : "?";
}

ActiveExtra decode_active_extra(std::uint32_t v, bool v13) {
    ActiveExtra a;
    a.optical = v13 && (v >> 10 & 1u);
    a.retimer = v13 && (v >> 9 & 1u);
    a.usb2 = !(v >> 5 & 1u);
    a.usb32 = !(v >> 4 & 1u);
    a.hub_hops = static_cast<std::uint8_t>(v >> 6 & 3u);
    a.u3_via_u3s = v >> 11 & 1u;
    a.asym = v >> 1 & 1u;
    return a;
}

Tbt decode_tbt(std::uint32_t v) {
    Tbt t;
    t.tbt_mode = v & 1u;
    t.speed = static_cast<std::uint8_t>(v >> 16 & 7u);
    t.rounded = v >> 19 & 1u;
    t.optical = v >> 21 & 1u;
    t.retimer = v >> 22 & 1u;
    t.uni_training = v >> 23 & 1u;
    t.active = v >> 25 & 1u;
    return t;
}

const char* tbt_speed_text(std::uint8_t speed) {
    switch (speed) {
        case 1: return "USB3";
        case 2: return "10G";
        case 3: return K2_P("10/20G");
        default: return "?";
    }
}

Dp decode_dp(std::uint32_t v) {
    Dp d;
    d.rate = static_cast<std::uint8_t>(v >> 2 & 15u);
    d.uhbr13_5 = v >> 26 & 1u;
    d.cable = static_cast<std::uint8_t>(v >> 28 & 3u);
    d.pins = static_cast<std::uint8_t>(((v >> 8) | (v >> 16)) & 0x3Fu);
    return d;
}

const char* dp_rate_text(const Dp& d) {
    switch (d.rate) {
        case 1: return "HBR3";
        case 2: return d.uhbr13_5 ? K2_P("UHBR13.5") : K2_P("UHBR10");
        case 3: return K2_P("UHBR20");
        default: return "";
    }
}

const char* dp_cable_text(std::uint8_t cable) {
    static constexpr const char* kText[] = {K2_P("passive"), K2_P("retimer"), K2_P("redriver"), K2_P("optical")};
    return kText[cable & 3];
}

}  // namespace k2::cable_full
