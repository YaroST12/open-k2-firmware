// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::cable_full: the read sequence against a fake cable, and the VDO decoders (test vectors built from the field
// positions of the public typec_tbt.h / typec_dp.h headers and the PD cable VDO tables).
#include <cstring>
#include <vector>

#include "k2/pd/cable_full.hpp"
#include "test_util.h"

using namespace k2;
using emk_learn::Reply;
using emk_learn::Request;
namespace cf = k2::cable_full;

namespace {

enum class How { Ack, Nak, Busy, Silent, Lost };

struct Plug {
    bool present = true;
    How svids = How::Ack;
    std::vector<std::uint16_t> list;
    std::vector<std::pair<std::uint16_t, std::vector<std::uint32_t>>> modes;   // SVID -> mode VDOs
    How modes_how = How::Ack;
    How identity = How::Ack;
    std::vector<std::uint32_t> id;
};

struct Fake {
    Plug sop1, sop2;
    std::vector<Request> seen;
    int fail_sop1_after = -1;   // SOP' exchanges report "no GoodCRC" past this many calls
};

Reply vdm_reply(const Request& req, unsigned type, const std::vector<std::uint32_t>& vdos) {
    Reply r;
    r.got = true;
    r.n = static_cast<std::uint8_t>(1 + vdos.size());
    r.header = static_cast<std::uint16_t>(0x0F | 0x100 | (req.spec_rev << 6) | (r.n << 12));
    r.w[0] = (req.w[0] & ~(3u << 6)) | (type << 6);
    for (std::size_t i = 0; i < vdos.size(); i++) r.w[1 + i] = vdos[i];
    return r;
}

bool answer(const Plug& p, How how, const Request& req, Reply& rep, const std::vector<std::uint32_t>& vdos) {
    switch (how) {
        case How::Ack: rep = vdm_reply(req, 1, vdos); break;
        case How::Nak: rep = vdm_reply(req, 2, {}); break;
        case How::Busy: rep = vdm_reply(req, 3, {}); break;
        default: break;
    }
    (void)p;
    return how != How::Lost;
}

bool exchange(void* ctx, const Request& req, Reply& rep) {
    auto& f = *static_cast<Fake*>(ctx);
    f.seen.push_back(req);
    rep = {};
    const bool two = req.sop == cf::kSopDouble;
    const Plug& p = two ? f.sop2 : f.sop1;
    if (!two && f.fail_sop1_after >= 0 && static_cast<int>(f.seen.size()) > f.fail_sop1_after) return false;
    if (!p.present) return false;   // no GoodCRC from an absent plug
    const std::uint16_t svid = static_cast<std::uint16_t>(req.w[0] >> 16);
    const unsigned cmd = req.w[0] & 0x1F;
    if (svid == 0xFF00 && cmd == 1) return answer(p, p.identity, req, rep, p.id);
    if (svid == 0xFF00 && cmd == 2) {
        std::vector<std::uint32_t> w;
        for (std::size_t i = 0; i < p.list.size(); i += 2)
            w.push_back((std::uint32_t(p.list[i]) << 16) | (i + 1 < p.list.size() ? p.list[i + 1] : 0));
        if (p.list.size() % 2 == 0) w.push_back(0);   // the list ends with a zero SVID
        return answer(p, p.svids, req, rep, w);
    }
    if (cmd == 3) {
        for (const auto& m : p.modes)
            if (m.first == svid) return answer(p, p.modes_how, req, rep, m.second);
        return answer(p, How::Nak, req, rep, {});
    }
    return true;
}

constexpr std::uint32_t kTbtVdo = (1u << 25) | (1u << 22) | (1u << 21) | (1u << 19) | (3u << 16) | 1u;   // active, retimer, optical, rounded, 10/20G, TBT mode
constexpr std::uint32_t kDpVdo = (2u << 28) | (1u << 26) | (3u << 2) | (0x0Cu << 8) | (0x04u << 16);

}  // namespace

static void test_passive_identity_only() {
    Fake f;
    f.sop1.svids = How::Nak;
    cf::Full out;
    CHECK_EQ(cf::read(exchange, &f, emk_variant::kFirst, false, out), true);
    CHECK_EQ(out.valid, true);
    CHECK_EQ(out.p1.svids_ack, false);
    CHECK_EQ(out.p1.nsvid, 0);
    CHECK_EQ(f.seen.size(), 1u);                // a NAK on SVIDs ends the plug, no modes asked
    CHECK_EQ(f.seen[0].sop, cf::kSopPrime);
    CHECK_EQ(out.id2_ack, false);               // passive: SOP'' never asked
}

static void test_tbt_cable() {
    Fake f;
    f.sop1.list = {0x8087, 0xFF01};
    f.sop1.modes = {{0x8087, {kTbtVdo}}, {0xFF01, {kDpVdo, 0x11}}};
    cf::Full out;
    CHECK_EQ(cf::read(exchange, &f, emk_variant::kFirst, false, out), true);
    CHECK_EQ(out.p1.svids_ack, true);
    CHECK_EQ(out.p1.nsvid, 2);
    CHECK_EQ(out.p1.svid[0], 0x8087);
    CHECK_EQ(out.p1.svid[1], 0xFF01);
    CHECK_EQ(out.p1.mode_ack, 3);
    CHECK_EQ(out.p1.mode[0], kTbtVdo);
    CHECK_EQ(out.p1.mode[1], kDpVdo);           // the first mode VDO only
    CHECK_EQ(f.seen.size(), 3u);                // SVIDs + two Modes
    CHECK_EQ(f.seen[1].w[0] >> 16, 0x8087u);    // Discover Modes: the SVID is in the VDM header
    CHECK_EQ(f.seen[1].w[0] & 0x1F, 3u);
    CHECK_EQ(f.seen[2].w[0] >> 16, 0xFF01u);
}

static void test_variant_is_kept() {
    Fake f;
    f.sop1.list = {0x8087};
    f.sop1.modes = {{0x8087, {kTbtVdo}}};
    cf::Full out;
    const emk_variant::Variant v1{2, 1};        // PD 3.x header, SVDM 1.0: what a Baseus-style cable answers
    CHECK_EQ(cf::read(exchange, &f, v1, false, out), true);
    for (const Request& r : f.seen) {
        CHECK_EQ(r.spec_rev, 2);
        CHECK_EQ((r.w[0] >> 13) & 3u, 0u);      // SVDM version 1.0
    }
}

static void test_unknown_svid_and_modes_nak() {
    Fake f;
    f.sop1.list = {0x2717, 0x8087, 0xFF01};
    f.sop1.modes = {{0x8087, {kTbtVdo}}};       // 2717 and FF01 NAK their Discover Modes
    cf::Full out;
    CHECK_EQ(cf::read(exchange, &f, emk_variant::kFirst, false, out), true);
    CHECK_EQ(out.p1.nsvid, 3);
    CHECK_EQ(out.p1.mode_ack, 2);               // only the second one answered
    CHECK_EQ(out.p1.mode[1], kTbtVdo);
    CHECK_EQ(out.p1.mode[0], 0u);
}

static void test_svid_cap() {
    Fake f;
    f.sop1.list = {1, 2, 3, 4, 5, 6};
    cf::Full out;
    cf::read(exchange, &f, emk_variant::kFirst, false, out);
    CHECK_EQ(out.p1.nsvid, cf::kMaxSvids);
    CHECK_EQ(f.seen.size(), 1u + cf::kMaxSvids);   // no paging, no retries
}

static void test_active_gets_sop2() {
    Fake f;
    f.sop1.list = {0x8087};
    f.sop1.modes = {{0x8087, {kTbtVdo}}};
    f.sop2.id = {0x1C00203C, 0x00000123, 0x00010000, 0x00082052, 0x41100000};
    f.sop2.list = {0xFF01};
    f.sop2.modes = {{0xFF01, {kDpVdo}}};
    cf::Full out;
    CHECK_EQ(cf::read(exchange, &f, emk_variant::kFirst, true, out), true);
    CHECK_EQ(out.id2_ack, true);
    CHECK_EQ(out.id2_n, 5);
    CHECK_EQ(out.id2[0], 0x1C00203Cu);
    CHECK_EQ(out.id2[4], 0x41100000u);
    CHECK_EQ(out.p2.svids_ack, true);
    CHECK_EQ(out.p2.nsvid, 1);
    CHECK_EQ(out.p2.mode[0], kDpVdo);
    int n2 = 0;
    for (const Request& r : f.seen) if (r.sop == cf::kSopDouble) n2++;
    CHECK_EQ(n2, 3);                            // identity, SVIDs, one Modes
    CHECK_EQ(f.seen[f.seen.size() - 3].w[0] >> 16, 0xFF00u);
    CHECK_EQ(f.seen[f.seen.size() - 3].w[0] & 0x1F, 1u);
}

static void test_sop2_silent_keeps_sop1() {
    Fake f;
    f.sop1.list = {0x8087};
    f.sop1.modes = {{0x8087, {kTbtVdo}}};
    f.sop2.present = false;                     // the plug does not GoodCRC at all
    cf::Full out;
    CHECK_EQ(cf::read(exchange, &f, emk_variant::kFirst, true, out), true);
    CHECK_EQ(out.p1.mode[0], kTbtVdo);
    CHECK_EQ(out.id2_ack, false);
    CHECK_EQ(out.p2.nsvid, 0);
}

static void test_sop2_nak_identity_stops() {
    Fake f;
    f.sop2.identity = How::Nak;                 // the spec has no Discover Identity on SOP'': a NAK is normal
    f.sop2.list = {0xFF01};
    cf::Full out;
    CHECK_EQ(cf::read(exchange, &f, emk_variant::kFirst, true, out), true);
    CHECK_EQ(out.id2_ack, false);
    CHECK_EQ(out.p2.nsvid, 0);                  // no SVIDs asked after a refused identity
}

static void test_silence_on_svids_keeps_state() {
    Fake f;
    f.sop1.svids = How::Silent;
    cf::Full out;
    CHECK_EQ(cf::read(exchange, &f, emk_variant::kFirst, false, out), true);
    CHECK_EQ(out.valid, true);
    CHECK_EQ(out.p1.svids_ack, false);
}

static void test_link_lost_reports_false() {
    Fake f;
    f.sop1.list = {0x8087, 0xFF01};
    f.sop1.modes = {{0x8087, {kTbtVdo}}, {0xFF01, {kDpVdo}}};
    f.fail_sop1_after = 2;                      // SVIDs and the first Modes go through, the second does not
    cf::Full out;
    CHECK_EQ(cf::read(exchange, &f, emk_variant::kFirst, true, out), false);
    CHECK_EQ(out.p1.mode[0], kTbtVdo);          // what was read stays
    CHECK_EQ(out.p1.mode_ack, 1);
    int n2 = 0;
    for (const Request& r : f.seen) if (r.sop == cf::kSopDouble) n2++;
    CHECK_EQ(n2, 0);
}

static void test_decode_active_extra() {
    // Copper re-timer, USB2 and USB3.2 supported (bits 5, 4 clear), hub hops 2, U3 through U3S.
    cf::ActiveExtra a = cf::decode_active_extra((1u << 11) | (1u << 9) | (2u << 6), true);
    CHECK_EQ(a.optical, false);
    CHECK_EQ(a.retimer, true);
    CHECK_EQ(a.usb2, true);
    CHECK_EQ(a.usb32, true);
    CHECK_EQ(a.hub_hops, 2);
    CHECK_EQ(a.u3_via_u3s, true);
    // Optical re-driver, USB2 and USB3.2 declared unsupported.
    a = cf::decode_active_extra((1u << 10) | (1u << 5) | (1u << 4), true);
    CHECK_EQ(a.optical, true);
    CHECK_EQ(a.retimer, false);
    CHECK_EQ(a.usb2, false);
    CHECK_EQ(a.usb32, false);
    // Before VDO 1 version 1.3 bits 10:8 are reserved: no optical or re-timer claim.
    a = cf::decode_active_extra((1u << 10) | (1u << 9), false);
    CHECK_EQ(a.optical, false);
    CHECK_EQ(a.retimer, false);
}

static void test_u3_power_text() {
    CHECK_STR(cf::u3_power_text(0), ">10mW");
    CHECK_STR(cf::u3_power_text(2), "1-5mW");
    CHECK_STR(cf::u3_power_text(5), "50-200uW");
    CHECK_STR(cf::u3_power_text(6), "<50uW");
    CHECK_STR(cf::u3_power_text(7), "?");
}

static void test_decode_tbt() {
    const cf::Tbt t = cf::decode_tbt(kTbtVdo);
    CHECK_EQ(t.tbt_mode, true);
    CHECK_EQ(t.speed, 3);
    CHECK_EQ(t.rounded, true);
    CHECK_EQ(t.optical, true);
    CHECK_EQ(t.retimer, true);
    CHECK_EQ(t.uni_training, false);
    CHECK_EQ(t.active, true);
    const cf::Tbt p = cf::decode_tbt(0x00020001);   // passive 10G, no flags
    CHECK_EQ(p.speed, 2);
    CHECK_EQ(p.active, false);
    CHECK_EQ(p.rounded, false);
    CHECK_EQ(cf::decode_tbt(1u << 23).uni_training, true);
    CHECK_STR(cf::tbt_speed_text(1), "USB3");
    CHECK_STR(cf::tbt_speed_text(2), "10G");
    CHECK_STR(cf::tbt_speed_text(3), "10/20G");
    CHECK_STR(cf::tbt_speed_text(0), "?");
    CHECK_STR(cf::tbt_speed_text(5), "?");
}

static void test_decode_dp() {
    const cf::Dp d = cf::decode_dp(kDpVdo);
    CHECK_EQ(d.rate, 3);
    CHECK_EQ(d.uhbr13_5, true);
    CHECK_EQ(d.cable, 2);
    CHECK_EQ(d.pins, 0x0C | 0x04);              // C and D from the DFP_D field, C from the UFP_D field
    CHECK_STR(cf::dp_rate_text(d), "UHBR20");
    cf::Dp e = cf::decode_dp(2u << 2);
    CHECK_STR(cf::dp_rate_text(e), "UHBR10");
    e.uhbr13_5 = true;
    CHECK_STR(cf::dp_rate_text(e), "UHBR13.5");
    CHECK_STR(cf::dp_rate_text(cf::decode_dp(1u << 2)), "HBR3");
    CHECK_STR(cf::dp_rate_text(cf::decode_dp(0)), "");
    CHECK_STR(cf::dp_cable_text(0), "passive");
    CHECK_STR(cf::dp_cable_text(1), "retimer");
    CHECK_STR(cf::dp_cable_text(2), "redriver");
    CHECK_STR(cf::dp_cable_text(3), "optical");
}

int main() {
    test_passive_identity_only();
    test_tbt_cable();
    test_variant_is_kept();
    test_unknown_svid_and_modes_nak();
    test_svid_cap();
    test_active_gets_sop2();
    test_sop2_silent_keeps_sop1();
    test_sop2_nak_identity_stops();
    test_silence_on_svids_keeps_state();
    test_link_lost_reports_false();
    test_decode_active_extra();
    test_u3_power_text();
    test_decode_tbt();
    test_decode_dp();
    return test_report("test_cable_full");
}
