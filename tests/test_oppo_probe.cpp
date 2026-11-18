// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the pure half of the OPPO (SVID 0x22D9) lab probe: the command words, the answer classes, the POWER / EXAPDO
// decoders, the caps diff, the start gate and the `pd oppo-power` pick. The sequence on the wire is tests/test_pd_sink_oppo.cpp.
#include "k2/pd/oppo_probe.hpp"
#include "test_util.h"

using namespace k2::pd;
using namespace k2::pd::oppo;

namespace {

// The words the phone sends (GPL kernel: PD_SVDM_HDR(0x22D9, 31) filled to SVDM 2.0 / object 1 by tcpm; PD_UVDM_HDR(0x22D9, n)
// = 0x22D9 << 16 | n), written out as literals, not rebuilt from the encoder.
void test_command_words() {
    CHECK_EQ(kAuthHeader, 0x22D9A11Fu);
    CHECK_EQ(kPowerHeader, 0x22D90001u);
    CHECK_EQ(kExapdoHeader, 0x22D90002u);
    CHECK_EQ(kSvid, 0x22D9);
}

void test_answer_classes() {
    CHECK_EQ(int(classify(0x22D9A15Fu)), int(Outcome::Ack));    // the AUTH ACK as the cable sends it
    CHECK_EQ(int(classify(0x22D9A19Fu)), int(Outcome::Nak));
    CHECK_EQ(int(classify(0x22D9A1DFu)), int(Outcome::Busy));
    CHECK_EQ(int(classify(0x22D9A11Fu)), int(Outcome::Reply));  // a REQ echoed back is no ACK
    CHECK_EQ(int(classify(0x22D90001u)), int(Outcome::Reply));  // unstructured: no command type
    CHECK_EQ(int(classify(0x22D90041u)), int(Outcome::Reply));  // ... even with bits 7..6 set
}

void test_power_decoder() {
    // 0x07D0 = 2000 x 10 mV = 20 V, 0x64 = 100 x 50 mA = 5 A
    const PowerInfo p = decode_power(0x07D00064u);
    CHECK_EQ(p.imax_ma, 5000);
    CHECK_EQ(p.vmax_mv, 20000);
    const PowerInfo q = decode_power(0x044C0041u);   // 1100 x 10 mV = 11 V, 65 x 50 mA = 3.25 A
    CHECK_EQ(q.imax_ma, 3250);
    CHECK_EQ(q.vmax_mv, 11000);
    CHECK_EQ(decode_power(0).imax_ma, 0);
    CHECK_EQ(decode_power(0xFFFF00FFu).vmax_mv, 655350);
    CHECK_EQ(decode_power(0xFFFF00FFu).imax_ma, 12750);
}

void test_exapdo_decoder() {
    const ExapdoInfo yes = decode_exapdo(0x47000000u);   // 01 0 00111: num 1, bit 7
    CHECK_EQ(yes.num, 1);
    CHECK_EQ(yes.bit, 7);
    CHECK_COND(yes.extra_pps);
    CHECK_COND(!decode_exapdo(0x87000000u).extra_pps);   // num 2
    CHECK_COND(!decode_exapdo(0x46000000u).extra_pps);   // bit 6
    CHECK_COND(!decode_exapdo(0).extra_pps);
    const ExapdoInfo top = decode_exapdo(0xDF123456u);   // 11 0 11111: bits 29 and 23..0 are not part of it
    CHECK_EQ(top.num, 3);
    CHECK_EQ(top.bit, 31);
    CHECK_COND(decode_exapdo(0x67FFFFFFu).extra_pps);    // bit 29 set beside num 1 / bit 7: still the ladder
}

void test_caps_diff() {
    const std::uint32_t a[] = {0x0001912C, 0x0002D12C, 0xC0DC2141};
    const std::uint32_t b[] = {0x0001912C, 0x0002D12C, 0xC0DC2164};   // the PPS current went up
    const std::uint32_t c[] = {0x0001912C, 0x0002D12C, 0xC0DC2141, 0xC1A42164};
    CHECK_EQ(caps_diff(a, a), 0);
    CHECK_EQ(caps_diff(a, b), 0x04);
    CHECK_EQ(caps_diff(a, c), 0x08);   // a new slot 4
    CHECK_EQ(caps_diff(c, a), 0x08);   // a slot gone
    CHECK_EQ(caps_diff({}, a), 0x07);
    const std::uint32_t nine[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK_EQ(caps_diff({}, nine), 0x7F);   // SPR slots only
    const Words w = words_of(nine);
    CHECK_EQ(w.n, 7);
    CHECK_EQ(w.w[6], 7u);
}

void test_start_gate() {
    Facts ok;
    ok.contract = true;
    ok.five_v = true;
    CHECK_EQ(int(refusal(ok)), int(Refusal::None));
    Facts f = ok; f.ports_busy = true;      CHECK_EQ(int(refusal(f)), int(Refusal::Busy));
    f = ok; f.engine_busy = true;           CHECK_EQ(int(refusal(f)), int(Refusal::Busy));
    f = ok; f.inline_mode = true;           CHECK_EQ(int(refusal(f)), int(Refusal::Inline));
    f = ok; f.trigger_refusal = true;       CHECK_EQ(int(refusal(f)), int(Refusal::Inline));
    f = ok; f.answers_sop = false;          CHECK_EQ(int(refusal(f)), int(Refusal::NotPartner));
    f = ok; f.contract = false;             CHECK_EQ(int(refusal(f)), int(Refusal::NoContract));
    f = ok; f.five_v = false;               CHECK_EQ(int(refusal(f)), int(Refusal::NotFiveV));
    f = ok; f.running = true;               CHECK_EQ(int(refusal(f)), int(Refusal::Running));
    // A charger test beats a missing contract: the busy owner is named first
    f = Facts{}; f.ports_busy = true;       CHECK_EQ(int(refusal(f)), int(Refusal::Busy));
    CHECK_STR(refusal_name(Refusal::NotFiveV), "not at 5 V");
    CHECK_STR(outcome_name(Outcome::NotSupported), "not_supported");
    CHECK_STR(end_name(End::Stalled), "stalled");
}

SourceCaps op_caps() {
    // The OnePlus 120 W brick's C-port list: 5/9/12/15/20 V 3.25 A, PPS 5-21 V 3.25 A
    SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    const std::uint32_t raw[] = {0x00019145, 0x0002D145, 0x0003C145, 0x0004B145, 0x00064145, 0xC1A43241};
    c.num_pdos = 6;
    for (int i = 0; i < 6; i++) c.pdos[i] = decode_pdo(raw[i]);
    return c;
}

void test_pick_power() {
    const SourceCaps c = op_caps();
    Pick p = pick_power(c, 9000, 3000);
    CHECK_EQ(p.pos, 2);
    CHECK_EQ(p.mv, 9000);
    CHECK_EQ(p.ma, 3000);
    p = pick_power(c, 20000, 0);               // 0 mA = what the slot offers
    CHECK_EQ(p.pos, 5);
    CHECK_EQ(p.ma, 3250);
    CHECK_EQ(pick_power(c, 20000, 3300).pos, 0);   // above the advertised current: refused, never clamped up
    CHECK_EQ(pick_power(c, 10000, 1000).pos, 6);   // no fixed 10 V: the PPS range serves it
    CHECK_EQ(pick_power(c, 11000, 3250).pos, 6);
    CHECK_EQ(pick_power(c, 11000, 3300).pos, 0);
    CHECK_EQ(pick_power(c, 22000, 1000).pos, 0);   // above every range
    CHECK_EQ(pick_power(c, 4000, 1000).pos, 0);
    SourceCaps none;
    CHECK_EQ(pick_power(none, 5000, 0).pos, 0);
    // An EPR AVS slot: the current at the point is the PDP over the voltage
    SourceCaps e = c;
    e.num_pdos = 7;
    e.pdos[6] = decode_pdo(0xD3C0968C);   // EPR AVS 15-48 V 140 W
    CHECK_EQ(pick_power(e, 28000, 5000).pos, 7);
    CHECK_EQ(pick_power(e, 28000, 5100).pos, 0);
}

}  // namespace

int main() {
    test_command_words();
    test_answer_classes();
    test_power_decoder();
    test_exapdo_decoder();
    test_caps_diff();
    test_start_gate();
    test_pick_power();
    return test_report("test_oppo_probe");
}
