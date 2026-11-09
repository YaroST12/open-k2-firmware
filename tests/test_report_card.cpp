// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger report card (k2/chargertest/report_card.hpp): fingerprint, building a card from a finished run, the
// fixed-size record codec and the delta against an earlier card.
#include <cstring>
#include <string>
#include <vector>

#include "k2/chargertest/report_card.hpp"
#include "test_util.h"

using namespace k2;
namespace rc = k2::report_card;
namespace ct = k2::charger_test;
using A = ct::Algorithm;
using V = ct::Verdict;

static ct::Step step(A a, ct::Group g, uint8_t pos, unsigned mv, V v, unsigned measured = 0, ct::Reason why = ct::Reason::None,
                     uint32_t pdo = 0, bool cont = false, bool avs = false, uint8_t detail = 0) {
    ct::Step s;
    s.algorithm = a; s.group = g; s.pos = pos; s.mv = uint16_t(mv); s.verdict = v; s.measured_mv = uint16_t(measured);
    s.reason = why; s.pdo = pdo; s.continuous = cont; s.avs = avs; s.detail = detail;
    return s;
}
// PPS APDO: 3.3..21 V (min 33, max 210 in 100 mV), 3 A.
static constexpr uint32_t kPps = (3u << 30) | (210u << 17) | (33u << 8) | 60;

static std::vector<ct::Step> run_a() {
    return {
        step(A::Pd3Fixed, ct::Group::Pd, 1, 5000, V::Pass, 5020),
        step(A::Pd3Fixed, ct::Group::Pd, 2, 9000, V::Pass, 9010),
        step(A::Pd3Fixed, ct::Group::Pd, 3, 20000, V::Pass, 19950),
        step(A::Pps, ct::Group::Pd, 4, 21000, V::Pass, 20990, ct::Reason::None, kPps, true),
        step(A::Pps, ct::Group::Pd, 4, 13000, V::Fail, 5000, ct::Reason::Voltage, kPps, true, false, 7),
        step(A::Qc3, ct::Group::Qc3, 0, 9000, V::Pass, 8900),
        step(A::Afc, ct::Group::Afc, 0, 9000, V::Skip, 0, ct::Reason::NoOffer),
    };
}
static ct::Evidence evidence() {
    ct::Evidence e; e.wire_rev = 2; e.num_pdos = 4; e.flags = ct::PpsOffer;
    return e;
}

static void test_fingerprint() {
    const uint32_t a[] = {0x0001912C, 0x0002D12C, 0xC0DC213C};
    const uint32_t f1 = rc::fingerprint(a, 2);
    CHECK_EQ(f1 == rc::fingerprint(a, 2), 1);
    CHECK_EQ(f1 == rc::fingerprint(a, 1), 0);          // spec revision is part of the identity
    const uint32_t b[] = {0x0001912C, 0x0002D12D, 0xC0DC213C};
    CHECK_EQ(f1 == rc::fingerprint(b, 2), 0);          // one raw PDO bit changes it
    CHECK_EQ(rc::fingerprint({}, 2) == 0, 0);          // never 0: 0 means "no identity"
    // Known answer: FNV-1a 32 over the little endian bytes of the PDOs, then the revision (python3).
    const uint32_t one[] = {0x01020304};
    CHECK_EQ(rc::fingerprint(one, 2), 0xACBED7F5u);
}

static void test_build_groups_and_ranges() {
    rc::Identity id{.fp = 0xAABBCCDD, .vid = 0x2717, .pid = 0x1234};
    const auto steps = run_a();
    rc::Context cx{.fw = 0x1234ABCD, .boots = 7, .uptime_s = 400};
    const rc::Card c = rc::build(steps, evidence(), {}, id, cx, ct::State::Done, ct::Reason::None);
    CHECK_EQ(c.id.fp, 0xAABBCCDDu); CHECK_EQ(c.id.vid, 0x2717); CHECK_EQ(c.num_pdos, 4); CHECK_EQ(c.spec_rev, 2);
    const auto& pd = c.group[unsigned(ct::Group::Pd)];
    CHECK_EQ(int(pd.verdict), int(rc::GV::Fail));      // one PPS step failed
    CHECK_EQ(pd.max_mv, 20000);      // fixed PDOs only (the PPS 21 V step belongs to its range); kept in 100 mV: 19.95 V -> 20 V
    CHECK_EQ(pd.fail_mv, 0);         // the only failure is a range step, which has its own fail_dv
    CHECK_EQ(pd.detail, 7);
    CHECK_EQ(pd.reason, int(ct::Reason::Voltage));
    CHECK_EQ(int(c.group[unsigned(ct::Group::Qc3)].verdict), int(rc::GV::Pass));
    CHECK_EQ(c.group[unsigned(ct::Group::Qc3)].max_mv, 8900);
    CHECK_EQ(int(c.group[unsigned(ct::Group::Afc)].verdict), int(rc::GV::Skip));
    CHECK_EQ(c.group[unsigned(ct::Group::Afc)].reason, int(ct::Reason::NoOffer));
    CHECK_EQ(int(c.group[unsigned(ct::Group::Qc2)].verdict), int(rc::GV::None));  // not in the run
    CHECK_EQ(c.n_ranges, 1);
    CHECK_EQ(c.range[0].alg, int(A::Pps)); CHECK_EQ(c.range[0].lo_dv, 33); CHECK_EQ(c.range[0].hi_dv, 210);
    CHECK_EQ(c.range[0].max_dv, 210);          // 20.99 V measured, kept in 100 mV units, rounded
    CHECK_EQ(c.range[0].fail_dv, 130);         // first failing target 13 V
    const auto n = c.counts();
    CHECK_EQ(n.pass, 1); CHECK_EQ(n.fail, 1); CHECK_EQ(n.skip, 1);   // groups: Qc3 pass, Pd fail, Afc skip
    CHECK_EQ(c.fw, 0x1234ABCDu); CHECK_EQ(c.boots, 7); CHECK_EQ(c.uptime_s, 400u);
}

// Baseus CJ21 run shape (tests/fixtures/charger_run3_baseus.json): five fixed PDOs, PPS 5-11 V at position 6 and
// PPS 5-21 V at position 7, everything passing. Both ranges share lo = 5 V, so the position is the key.
static void test_two_pps_ranges_and_fixed_max() {
    constexpr uint32_t kP1 = (3u << 30) | (110u << 17) | (50u << 8) | 100;   // 5-11 V 5 A
    constexpr uint32_t kP2 = (3u << 30) | (210u << 17) | (50u << 8) | 64;    // 5-21 V 3.2 A
    const std::vector<ct::Step> steps = {
        step(A::Pd3Fixed, ct::Group::Pd, 1, 5000, V::Pass, 4765),
        step(A::Pd3Fixed, ct::Group::Pd, 4, 15000, V::Pass, 14990),
        step(A::Pd3Fixed, ct::Group::Pd, 5, 20000, V::Pass, 19951),
        step(A::Pps, ct::Group::Pd, 6, 5000, V::Pass, 5088, ct::Reason::None, kP1),
        step(A::Pps, ct::Group::Pd, 6, 8000, V::Pass, 7983, ct::Reason::None, kP1, true),
        step(A::Pps, ct::Group::Pd, 6, 11000, V::Pass, 10976, ct::Reason::None, kP1, true),
        step(A::Pps, ct::Group::Pd, 6, 8000, V::Pass, 8010, ct::Reason::None, kP1, true),
        step(A::Pps, ct::Group::Pd, 6, 5000, V::Pass, 5088, ct::Reason::None, kP1, true),
        step(A::Pps, ct::Group::Pd, 7, 5000, V::Pass, 5088, ct::Reason::None, kP2),
        step(A::Pps, ct::Group::Pd, 7, 13000, V::Pass, 12981, ct::Reason::None, kP2, true),
        step(A::Pps, ct::Group::Pd, 7, 21000, V::Pass, 20964, ct::Reason::None, kP2, true),
        step(A::Pps, ct::Group::Pd, 7, 13000, V::Pass, 13010, ct::Reason::None, kP2, true),
        step(A::Pps, ct::Group::Pd, 7, 5000, V::Pass, 5088, ct::Reason::None, kP2, true),
    };
    const rc::Card c = rc::build(steps, evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None);
    CHECK_EQ(c.group[unsigned(ct::Group::Pd)].max_mv, 20000);
    CHECK_EQ(c.n_ranges, 2);
    CHECK_EQ(c.range[0].lo_dv, 50); CHECK_EQ(c.range[0].hi_dv, 110); CHECK_EQ(c.range[0].max_dv, 110);
    CHECK_EQ(c.range[1].lo_dv, 50); CHECK_EQ(c.range[1].hi_dv, 210); CHECK_EQ(c.range[1].max_dv, 210);
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, c);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d == c, 1);                                   // the APDO position survives the codec
}

// Baseus FH22 100 W (hardware 2026-11-08): the PPS 5-21 V APDO stops at 19.4 V. The Capped step has a reason of its own, the
// range keeps the ceiling as the highest level reached, and both survive the card codec (the reason fits the 6-bit field).
static void test_capped_range_keeps_its_ceiling_and_reason() {
    constexpr uint32_t kP = (3u << 30) | (210u << 17) | (50u << 8) | 100;   // 5-21 V 5 A
    const std::vector<ct::Step> steps = {
        step(A::Pd3Fixed, ct::Group::Pd, 5, 20000, V::Pass, 19413),
        step(A::Pps, ct::Group::Pd, 6, 13000, V::Pass, 12965, ct::Reason::None, kP, true),
        step(A::Pps, ct::Group::Pd, 6, 21000, V::Fail, 19419, ct::Reason::Capped, kP, true),
        step(A::Pps, ct::Group::Pd, 6, 13000, V::Pass, 12970, ct::Reason::None, kP, true),
        step(A::Pps, ct::Group::Pd, 6, 5000, V::Pass, 5040, ct::Reason::None, kP, true),
    };
    const rc::Card c = rc::build(steps, evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None);
    CHECK_EQ(c.n_ranges, 1);
    CHECK_EQ(c.range[0].max_dv, 194);   // 19.419 V rounded to 100 mV: the ceiling, not the 13 V that passed
    CHECK_EQ(c.range[0].fail_dv, 210);
    CHECK_EQ(c.group[unsigned(ct::Group::Pd)].reason, int(ct::Reason::Capped));
    CHECK_EQ(int(c.group[unsigned(ct::Group::Pd)].verdict), int(rc::GV::Fail));   // a Capped level counts as a fail for the verdict
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, c);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d == c, 1);
    CHECK_EQ(d.group[unsigned(ct::Group::Pd)].reason, int(ct::Reason::Capped));
}

// QC2 5/9 pass and 12 fails (voltage not reached), AFC 9 passes and 12 fails (no response), VIVO 5 passes and
// 9/11 fail. Every group keeps the highest passing voltage and the first failing target with its reason.
static void test_group_keeps_max_pass_and_first_fail() {
    constexpr uint8_t kNoResponse = uint8_t(k2::fc::Fail::NoResponse), kNoBand = uint8_t(k2::fc::Fail::NoBand);
    const std::vector<ct::Step> steps = {
        step(A::Qc2, ct::Group::Qc2, 0, 5000, V::Pass, 5010),
        step(A::Qc2, ct::Group::Qc2, 0, 9000, V::Pass, 9020),
        step(A::Qc2, ct::Group::Qc2, 0, 12000, V::Fail, 9000, ct::Reason::Request, 0, false, false, kNoBand),
        step(A::Afc, ct::Group::Afc, 0, 9000, V::Pass, 9100),
        step(A::Afc, ct::Group::Afc, 0, 12000, V::Fail, 0, ct::Reason::Request, 0, false, false, kNoResponse),
        step(A::Vivo, ct::Group::Vivo, 0, 5000, V::Pass, 5000),
        step(A::Vivo, ct::Group::Vivo, 0, 9000, V::Fail, 0, ct::Reason::Request, 0, false, false, kNoResponse),
        step(A::Vivo, ct::Group::Vivo, 0, 11000, V::Fail, 0, ct::Reason::Timeout),
        step(A::Scp, ct::Group::Scp, 0, 5000, V::Fail, 0, ct::Reason::Timeout),
    };
    const rc::Card c = rc::build(steps, evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None);
    const auto& q = c.group[unsigned(ct::Group::Qc2)];
    CHECK_EQ(int(q.verdict), int(rc::GV::Fail));
    CHECK_EQ(q.max_mv, 9000); CHECK_EQ(q.fail_mv, 12000); CHECK_EQ(q.detail, kNoBand);
    CHECK_STR(rc::group_why(unsigned(ct::Group::Qc2), q), "voltage not reached");
    const auto& a = c.group[unsigned(ct::Group::Afc)];
    CHECK_EQ(a.max_mv, 9100); CHECK_EQ(a.fail_mv, 12000);
    CHECK_STR(rc::group_why(unsigned(ct::Group::Afc), a), "no response");
    const auto& v = c.group[unsigned(ct::Group::Vivo)];
    CHECK_EQ(v.max_mv, 5000); CHECK_EQ(v.fail_mv, 9000);            // the first failing target, not the last
    CHECK_EQ(v.reason, int(ct::Reason::Request));
    const auto& s = c.group[unsigned(ct::Group::Scp)];
    CHECK_EQ(s.max_mv, 0); CHECK_EQ(s.fail_mv, 5000);
    CHECK_STR(rc::group_why(unsigned(ct::Group::Scp), s), ct::name(ct::Reason::Timeout));   // no detail: the reason names it
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, c);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d == c, 1);
}

// Detected / handshake ok passes name the group; a real voltage pass in the same group (SCP 10 V after the handshake) wins.
static void test_group_detail_for_detected_and_handshake_ok() {
    using GV = rc::GV;
    auto card = [](std::vector<ct::Step> steps) { return rc::build(steps, evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None); };
    const auto g = unsigned(ct::Group::Scp);
    const uint8_t det = uint8_t(k2::fc::Fail::Detected), hs = uint8_t(k2::fc::Fail::NoVoltage);
    rc::Card c = card({step(A::Scp, ct::Group::Scp, 0, 5000, V::Pass, 5000, ct::Reason::None, 0, false, false, det)});
    CHECK_EQ(bool(c.group[g].verdict == GV::Pass && rc::group_detected(c.group[g])), true);
    c = card({step(A::Scp, ct::Group::Scp, 0, 5000, V::Pass, 5000, ct::Reason::None, 0, false, false, det),
              step(A::Scp, ct::Group::Scp, 0, 10000, V::Pass, 10000)});
    CHECK_EQ(bool(c.group[g].verdict == GV::Pass && !rc::group_detected(c.group[g])), true);
    c = card({step(A::Scp, ct::Group::Scp, 0, 5000, V::Pass, 5000, ct::Reason::None, 0, false, false, det),
              step(A::Scp, ct::Group::Scp, 0, 5000, V::Pass, 5000, ct::Reason::None, 0, false, false, hs)});
    CHECK_EQ(bool(rc::group_handshake_ok(c.group[g]) && !rc::group_detected(c.group[g])), true);
}
static void test_group_why_names() {
    rc::GroupResult r{rc::GV::Fail, 0, uint8_t(128 + int(k2::pd::Err::OverCap)), uint8_t(ct::Reason::Request)};
    CHECK_STR(rc::group_why(unsigned(ct::Group::Pd), r), k2::pd::err_name(k2::pd::Err::OverCap));
    r.detail = uint8_t(k2::pd::SinkResult::Rejected);
    CHECK_STR(rc::group_why(unsigned(ct::Group::Pd), r), k2::pd::result_name(k2::pd::SinkResult::Rejected));
    r.detail = uint8_t(k2::fc::Fail::Inline);
    CHECK_STR(rc::group_why(unsigned(ct::Group::Qc3), r), k2::fc::name(k2::fc::Fail::Inline));
    r.detail = uint8_t(k2::fc::Fail::NeedsLoad);                   // the last enum values are named too
    CHECK_STR(rc::group_why(unsigned(ct::Group::Vooc), r), "needs load");
    r.detail = uint8_t(k2::fc::Fail::NoVoltage);
    CHECK_STR(rc::group_why(unsigned(ct::Group::Vooc), r), "handshake ok");
    r.detail = uint8_t(k2::fc::Fail::Detected);
    CHECK_STR(rc::group_why(unsigned(ct::Group::SuperVooc), r), "detected");
    r.detail = 77;                                                 // not a known code: fall back to the reason
    CHECK_STR(rc::group_why(unsigned(ct::Group::Qc3), r), ct::name(ct::Reason::Request));
    // An EPR skip's detail sits above 128 + pd::Err: the PD group names it, it is never taken for an engine code
    r = {rc::GV::Skip, 0, ct::kDetailMonitor36, uint8_t(ct::Reason::OverCap)};
    CHECK_STR(rc::group_why(unsigned(ct::Group::Pd), r), "EPR: above K2's 36V monitor");
    r = {rc::GV::Skip, 0, uint8_t(ct::kDetailEprFail + 3), uint8_t(ct::Reason::EprRefused)};
    CHECK_STR(rc::group_why(unsigned(ct::Group::Pd), r), "EPR: refused (RDO)");
    r = {rc::GV::Skip, 0, ct::kDetailEprOffered, uint8_t(ct::Reason::CableLimit)};
    CHECK_STR(rc::group_why(unsigned(ct::Group::Pd), r), "EPR offered: needs EPR cable");
    // ... and survives the card's six-bit detail field
    CHECK_EQ(int(rc::detail::unpack_detail(rc::detail::pack_detail(ct::kDetailEprOffered))), int(ct::kDetailEprOffered));
    CHECK_EQ(int(rc::detail::unpack_detail(rc::detail::pack_detail(ct::kDetailMonitor48))), int(ct::kDetailMonitor48));
    CHECK_EQ(int(rc::detail::unpack_detail(rc::detail::pack_detail(ct::kDetailEprFail + 5))), int(ct::kDetailEprFail + 5));
}

// QC3+ whose output never left 5 V: the 5 V baseline row passed, the raised rows are skipped "no QC3+ response". The group is a
// skip (not a pass at 5 V, not a fail) and says why; a QC3+ row that really failed stays a failure.
static void test_qc3plus_without_response_is_a_skipped_group() {
    const unsigned g = unsigned(ct::Group::Qc3Plus);
    std::vector<ct::Step> steps = {
        step(A::Qc3Plus, ct::Group::Qc3Plus, 0, 5000, V::Pass, 5100),
        step(A::Qc3Plus, ct::Group::Qc3Plus, 0, 9000, V::Skip, 5000, ct::Reason::NoQc3Plus, 0, true, false, uint8_t(k2::fc::Fail::NoResponse)),
        step(A::Qc3Plus, ct::Group::Qc3Plus, 0, 12000, V::Skip, 5000, ct::Reason::NoQc3Plus, 0, true, false, uint8_t(k2::fc::Fail::NoResponse)),
    };
    const rc::Card c = rc::build(steps, evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None);
    CHECK_EQ(int(c.group[g].verdict), int(rc::GV::Skip));
    CHECK_EQ(int(c.group[g].reason), int(ct::Reason::NoQc3Plus));
    CHECK_STR(rc::group_why(g, c.group[g]), "no QC3+");   // one neutral word: the group does not do QC3+ (the 5 V baseline does not count)
    CHECK_EQ(int(c.counts().pass), 0);                    // a skipped group adds no pass to the card's group totals
    steps[1] = step(A::Qc3Plus, ct::Group::Qc3Plus, 0, 9000, V::Fail, 9900, ct::Reason::Voltage, 0, true);
    steps[2] = step(A::Qc3Plus, ct::Group::Qc3Plus, 0, 12000, V::Skip, 9900, ct::Reason::Voltage, 0, true);
    CHECK_EQ(int(rc::build(steps, evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None).group[g].verdict), int(rc::GV::Fail));
}

// A Class A charger declines the 20 V Class B level: the QC2 group still passes on its own levels, and a group made only
// of the Class B skip is a skip that says "Class A only", never a failure.
static void test_class_b_on_class_a_is_a_skip_not_a_fail() {
    const unsigned g = unsigned(ct::Group::Qc2);
    std::vector<ct::Step> steps = {
        step(A::Qc2, ct::Group::Qc2, 0, 12000, V::Pass, 12100),
        step(A::Qc2ClassB, ct::Group::Qc2, 0, 20000, V::Skip, 12100, ct::Reason::ClassAOnly),
    };
    CHECK_EQ(int(rc::build(steps, evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None).group[g].verdict), int(rc::GV::Pass));
    steps.erase(steps.begin());
    const rc::Card c = rc::build(steps, evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None);
    CHECK_EQ(int(c.group[g].verdict), int(rc::GV::Skip));
    CHECK_STR(rc::group_why(g, c.group[g]), "Class A only (12 V max)");
}

// QC4+/QC5 label (passive, fact based): Qualcomm USB VID 0x05C6 plus a PPS APDO is "QC4+"; a rated power of 100 W or
// more (or an EPR AVS range) makes it "QC5". Never a behaviour key, so a missing VID or PPS just means no label.
static void test_qc_label() {
    const auto steps = run_a();   // advertises one PPS APDO, 3.3..21 V
    auto make = [&](uint16_t vid, uint8_t pdp, const std::vector<ct::Step>& s) {
        rc::Identity id{.fp = 1, .vid = vid, .pid = 1, .pdp_w = pdp};
        return rc::build(s, evidence(), {}, id, {}, ct::State::Done, ct::Reason::None);
    };
    auto label = [&](uint16_t vid, uint8_t pdp, const std::vector<ct::Step>& s) { return int(rc::qc_label(make(vid, pdp, s))); };
    CHECK_EQ(label(0x05C6, 0, steps), int(rc::QcLabel::Qc4Plus));
    CHECK_EQ(label(0x05C6, 45, steps), int(rc::QcLabel::Qc4Plus));
    CHECK_EQ(label(0x05C6, 99, steps), int(rc::QcLabel::Qc4Plus));
    CHECK_EQ(label(0x05C6, 100, steps), int(rc::QcLabel::Qc5));
    CHECK_EQ(label(0x05C6, 140, steps), int(rc::QcLabel::Qc5));
    // Another VID (or none): never a QC label however strong; the PPS 3.3-21 V 3 A range reads as Samsung SFC 3.0 (capability only).
    CHECK_EQ(label(0x2717, 140, steps), int(rc::QcLabel::Sfc3));
    CHECK_EQ(label(0, 140, steps), int(rc::QcLabel::Sfc3));
    const std::vector<ct::Step> no_pps = {step(A::Pd3Fixed, ct::Group::Pd, 1, 5000, V::Pass, 5020),
                                          step(A::Pd3Fixed, ct::Group::Pd, 2, 9000, V::Pass, 9010)};
    CHECK_EQ(label(0x05C6, 140, no_pps), int(rc::QcLabel::None));  // Qualcomm VID without a PPS APDO
    // An EPR AVS range without a rated power still counts as 100 W class.
    rc::Card c = make(0x05C6, 0, steps);
    c.range[c.n_ranges++] = rc::Range{uint8_t(A::EprAvs), 8, 150, 480, 0, 0, 0};
    CHECK_EQ(int(rc::qc_label(c)), int(rc::QcLabel::Qc5));
    // The label comes from stored fields only, so it survives the record.
    std::array<uint8_t, rc::kMaxRecord> buf{};
    rc::encode_card(buf, make(0x05C6, 140, steps));
    rc::Card back;
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, back), 1);
    CHECK_EQ(int(rc::qc_label(back)), int(rc::QcLabel::Qc5));
    // The fact line: QC4+ shows via PPS with V/A; QC5 shows via PD when pd_ma is available, else falls back to PPS span.
    // In run_a(), fixed PDO steps have pdo=0, so pd_ma=0 -> QC5 falls back to PPS span.
    char t[48];
    CHECK_EQ(rc::qc_label_text(back, t, sizeof t) > 0, 1);
    CHECK_EQ(std::string(t) == "QC5 (Qualcomm VID, PPS 3.3-21V)", true);
    CHECK_EQ(rc::qc_label_text(make(0x05C6, 45, steps), t, sizeof t) > 0, 1);
    // run_a() has PPS range with 3 A (kPps has 60 x 50 mA = 3 A), so QC4+ shows "via PPS 3.3-21V 3A"
    CHECK_EQ(std::string(t) == "QC4+ (via PPS 3.3-21V 3A)", true);
    rc::Card weak = make(0x2717, 45, steps); weak.range[0].ma = 2000;   // another VID, no capability: no label
    CHECK_EQ(int(rc::qc_label_text(weak, t, sizeof t)), 0);
    CHECK_EQ(t[0], 0);
}

// QC4+/QC5 label justification with PPS V/A and PD V/A text.
static void test_qc_label_justification() {
    const auto steps = run_a();   // advertises one PPS APDO, 3.3..21 V, 3 A (kPps has 60 x 50 mA = 3 A)
    auto make = [&](uint16_t vid, uint8_t pdp, const std::vector<ct::Step>& s) {
        rc::Identity id{.fp = 1, .vid = vid, .pid = 1, .pdp_w = pdp};
        return rc::build(s, evidence(), {}, id, {}, ct::State::Done, ct::Reason::None);
    };
    // QC4+: Qualcomm VID + PPS APDO, pdp_w < 100 -> should show "QC4+ (via PPS 3.3-21V 3A)"
    rc::Card c4 = make(0x05C6, 45, steps);
    CHECK_EQ(int(rc::qc_label(c4)), int(rc::QcLabel::Qc4Plus));
    char t[48];
    CHECK_EQ(rc::qc_label_text(c4, t, sizeof t) > 0, 1);
    CHECK_EQ(std::string(t) == "QC4+ (via PPS 3.3-21V 3A)", true);
    // Also check the justification part alone
    CHECK_EQ(rc::qc_label_justification(c4, t, sizeof t) > 0, 1);
    CHECK_EQ(std::string(t) == "via PPS 3.3-21V 3A", true);
    // QC5: Qualcomm VID + PPS APDO + pdp_w >= 100 -> should show "QC5 (via PD 20V 3A)"
    // run_a() has fixed PDOs: 5V, 9V, 20V. The highest voltage is 20V, and pd_ma should be 3A (from the PDO).
    rc::Card c5 = make(0x05C6, 140, steps);
    CHECK_EQ(int(rc::qc_label(c5)), int(rc::QcLabel::Qc5));
    CHECK_EQ(rc::qc_label_text(c5, t, sizeof t) > 0, 1);
    // The highest fixed PDO voltage is 20000 mV -> 20V, and pd_ma should be the max current from fixed PDOs.
    // In run_a(), the fixed PDOs are at positions 1,2,3. The PDO word for position 3 (20V) is not provided, but the step
    // has pdo=0 for fixed steps. The pd_ma is computed from the PDO word of fixed steps.
    // In run_a(), the first step is step(A::Pd3Fixed, ct::Group::Pd, 1, 5000, V::Pass, 5020) with pdo=0.
    // So pd_ma might be 0. We need to ensure the steps have pdo set for fixed PDOs.
    // Let's create a more complete run with fixed PDO currents.
    // For QC5 test, we need a card with pd_ma > 0. Let's create a custom card.
    rc::Card c5_custom = make(0x05C6, 140, steps);
    // Manually set pd_ma and group[pd].max_mv to known values
    c5_custom.pd_ma = 5000; // 5 A
    c5_custom.group[unsigned(ct::Group::Pd)].max_mv = 20000; // 20 V
    CHECK_EQ(rc::qc_label_text(c5_custom, t, sizeof t) > 0, 1);
    CHECK_EQ(std::string(t) == "QC5 (via PD 20V 5A)", true);
    CHECK_EQ(rc::qc_label_justification(c5_custom, t, sizeof t) > 0, 1);
    CHECK_EQ(std::string(t) == "via PD 20V 5A", true);
    // Non-Qualcomm VID => no label
    rc::Card no_label = make(0x2717, 140, steps);
    no_label.range[0].ma = 2000;   // a 2 A APDO: no SFC reading either
    CHECK_EQ(int(rc::qc_label(no_label)), int(rc::QcLabel::None));
    CHECK_EQ(int(rc::qc_label_text(no_label, t, sizeof t)), 0);
    CHECK_EQ(int(rc::qc_label_justification(no_label, t, sizeof t)), 0);
    // Qualcomm VID without PPS => no label
    const std::vector<ct::Step> no_pps = {step(A::Pd3Fixed, ct::Group::Pd, 1, 5000, V::Pass, 5020),
                                          step(A::Pd3Fixed, ct::Group::Pd, 2, 9000, V::Pass, 9010)};
    rc::Card no_pps_card = make(0x05C6, 140, no_pps);
    CHECK_EQ(int(rc::qc_label(no_pps_card)), int(rc::QcLabel::None));
    CHECK_EQ(int(rc::qc_label_text(no_pps_card, t, sizeof t)), 0);
    // QC4+ with fractional current: PPS at 3.25 A
    rc::Card c4_frac = make(0x05C6, 45, steps);
    c4_frac.range[0].ma = 3250; // 3.25 A
    CHECK_EQ(rc::qc_label_text(c4_frac, t, sizeof t) > 0, 1);
    CHECK_EQ(std::string(t) == "QC4+ (via PPS 3.3-21V 3.25A)", true);
}

// Passive OEM labels read off the card's own fields: Motorola TurboPower (a Motorola / Lenovo vendor ID plus a PPS APDO above
// 5 A), Samsung SFC 2.0 / 3.0 (PPS capability, no vendor ID: Samsung's modes are plain PD PPS). No probe, no behaviour key.
static rc::Card pps_card(uint16_t vid, unsigned lo_dv, unsigned hi_dv, unsigned ma, unsigned cable_ma) {
    rc::Card c; c.id.vid = vid; c.n_ranges = 1;
    c.range[0] = rc::Range{uint8_t(A::Pps), 1, uint16_t(lo_dv), uint16_t(hi_dv), uint16_t(hi_dv), 0, uint16_t(ma)};
    c.cable.max_ma = uint16_t(cable_ma);
    return c;
}
static void test_moto_turbopower_label() {
    char t[48];
    for (const uint16_t vid : {0x22B8, 0x17EF}) {
        const rc::Card c = pps_card(vid, 50, 200, 6250, 5000);
        CHECK_EQ(int(rc::qc_label(c)), int(rc::QcLabel::MotoTurbo));
        CHECK_EQ(rc::qc_label_text(c, t, sizeof t) > 0, 1);
        CHECK_STR(t, "Moto TurboPower (via PPS 5-20V 6.25A)");
    }
    CHECK_EQ(int(rc::qc_label(pps_card(0x22B8, 50, 200, 2000, 5000))), int(rc::QcLabel::None));       // a Motorola VID without the >5 A APDO
    CHECK_EQ(int(rc::qc_label(pps_card(0x2717, 50, 200, 6250, 5000))) != int(rc::QcLabel::MotoTurbo), 1);   // another VID: no TurboPower label
    CHECK_EQ(int(rc::qc_label(pps_card(0, 50, 200, 6250, 5000))) != int(rc::QcLabel::MotoTurbo), 1);
    CHECK_STR(rc::qc_label_name(rc::QcLabel::MotoTurbo), "Moto TurboPower");
    // It survives the record (the label rests on stored fields only).
    std::array<uint8_t, rc::kMaxRecord> buf{};
    rc::Card back; const rc::Card c = pps_card(0x22B8, 50, 200, 6250, 5000);
    rc::encode_card(buf, c);
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, back), 1);
    CHECK_EQ(int(rc::qc_label(back)), int(rc::QcLabel::MotoTurbo));
}
static void test_oppo_svid_note() {
    // The Runner's evidence carries it into the card; the card stores it in the cable block's spare byte (payload 177).
    ct::Evidence ev; ev.seen = ct::SeenOppoSvid;
    const rc::Card built = rc::build({}, ev, {}, {}, {}, ct::State::Done, ct::Reason::None);
    CHECK_EQ(built.src_flags, rc::kSrcOppoSvid);
    CHECK_STR(rc::oppo_note(built), "OPPO SVID 0x22D9 (discovery only)");
    CHECK_EQ(rc::oppo_note(rc::build({}, ct::Evidence{}, {}, {}, {}, ct::State::Done, ct::Reason::None)) == nullptr, 1);
    std::array<uint8_t, rc::kMaxRecord> buf{};
    rc::Card back;
    rc::encode_card(buf, built);
    CHECK_EQ(buf[4 + 177], rc::kSrcOppoSvid);
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, back), 1);
    CHECK_STR(rc::oppo_note(back), "OPPO SVID 0x22D9 (discovery only)");
    CHECK_EQ(back == built, 1);
    // A card from before the byte meant anything (v4 and older: zero padding) never shows the note.
    buf[4 + 33] = 4; buf[4 + 177] = 1;
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, back), 1);
    CHECK_EQ(rc::oppo_note(back) == nullptr, 1);
}
// The adapter's SCP current limit (read by the unit's first session) rides in src_flags bits 1..7 (50 mA units, as pd_ma); the
// voltage of the note is the highest SCP level that passed, so the same words come from a stored card as from the live run.
static void test_scp_capability_note() {
    ct::Evidence ev; ev.scp_ma = 6000;
    rc::Card built = rc::build({}, ev, {}, {}, {}, ct::State::Done, ct::Reason::None);
    CHECK_EQ(built.src_flags, 120 << 1);
    CHECK_EQ(rc::scp_cap_ma(built), 6000);
    char t[32];
    // No SCP group result: the current alone.
    CHECK_EQ(rc::scp_note(built, t, sizeof t), 1);
    CHECK_STR(t, "SCP cap 6A");
    built.group[unsigned(ct::Group::Scp)] = {rc::GV::Pass, 20000, 0, 0, 0};
    CHECK_EQ(rc::scp_note(built, t, sizeof t), 1);
    CHECK_STR(t, "SCP cap 20V 6A");
    built.group[unsigned(ct::Group::Scp)].max_mv = 5500; ev.scp_ma = 2250;
    built.src_flags = uint8_t(45 << 1);
    CHECK_EQ(rc::scp_note(built, t, sizeof t), 1);
    CHECK_STR(t, "SCP cap 5.5V 2.25A");
    // A failed group has no level to name; the oppo bit and the current are independent.
    built.group[unsigned(ct::Group::Scp)] = {rc::GV::Fail, 0, 0, uint8_t(ct::Reason::Timeout), 100};
    built.src_flags |= rc::kSrcOppoSvid;
    CHECK_EQ(rc::scp_note(built, t, sizeof t), 1);
    CHECK_STR(t, "SCP cap 2.25A");
    CHECK_STR(rc::oppo_note(built), "OPPO SVID 0x22D9 (discovery only)");
    // Nothing read: no note. The current clamps at 6.35 A (7 bits).
    CHECK_EQ(rc::scp_note(rc::build({}, ct::Evidence{}, {}, {}, {}, ct::State::Done, ct::Reason::None), t, sizeof t), 0);
    ct::Evidence big; big.scp_ma = 9000;
    CHECK_EQ(rc::scp_cap_ma(rc::build({}, big, {}, {}, {}, ct::State::Done, ct::Reason::None)), 6350);
    // It survives the record.
    std::array<uint8_t, rc::kMaxRecord> buf{};
    rc::Card back;
    rc::encode_card(buf, built);
    CHECK_EQ(buf[4 + 177], built.src_flags);
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, back), 1);
    CHECK_EQ(back == built, 1);
    CHECK_EQ(rc::scp_cap_ma(back), 2250);
    // A card from before the bits meant anything (v4 and older) names no capability.
    buf[4 + 33] = 4;
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, back), 1);
    CHECK_EQ(rc::scp_note(back, t, sizeof t), 0);
}
static void test_samsung_sfc_labels() {
    char t[48];
    auto label = [](unsigned lo, unsigned hi, unsigned ma, unsigned cable) { return int(rc::qc_label(pps_card(0, lo, hi, ma, cable))); };
    CHECK_EQ(label(33, 110, 4050, 5000), int(rc::QcLabel::Sfc2));   // PPS 3.3-11 V 4.05 A behind a 5 A cable
    CHECK_EQ(label(33, 110, 4050, 3000), int(rc::QcLabel::None));   // a 3 A cable cannot carry it
    CHECK_EQ(label(33, 110, 3000, 5000), int(rc::QcLabel::None));   // below 4.05 A
    CHECK_EQ(label(50, 110, 4050, 5000), int(rc::QcLabel::None));   // does not cover 3.3 V
    CHECK_EQ(label(50, 150, 3000, 3000), int(rc::QcLabel::Sfc2));   // PPS to 15 V at 3 A needs no 5 A cable
    CHECK_EQ(label(50, 150, 2950, 5000), int(rc::QcLabel::None));
    CHECK_EQ(label(33, 210, 3000, 3000), int(rc::QcLabel::Sfc3));   // PPS to 20 V at 3 A
    CHECK_EQ(label(33, 200, 3000, 0), int(rc::QcLabel::Sfc3));
    CHECK_EQ(label(33, 210, 2950, 5000), int(rc::QcLabel::None));
    CHECK_EQ(label(33, 210, 6250, 5000) != int(rc::QcLabel::Sfc2), 1);   // 3.0 outranks 2.0
    CHECK_STR(rc::qc_label_name(rc::QcLabel::Sfc2), "Samsung SFC 2.0");
    CHECK_STR(rc::qc_label_name(rc::QcLabel::Sfc3), "Samsung SFC 3.0");
    CHECK_EQ(rc::qc_label_text(pps_card(0, 33, 210, 3000, 3000), t, sizeof t) > 0, 1);
    CHECK_STR(t, "Samsung SFC 3.0 (via PPS 3.3-21V 3A)");
    // A Qualcomm vendor ID keeps its own label.
    CHECK_EQ(int(rc::qc_label(pps_card(0x05C6, 33, 210, 3000, 3000))), int(rc::QcLabel::Qc4Plus));
    // Another PD list with no PPS range: nothing.
    rc::Card none; none.id.vid = 0; CHECK_EQ(int(rc::qc_label(none)), int(rc::QcLabel::None));
}

// Advertised currents: a PPS APDO's max current and the highest fixed PDO current ride in spare bits of the record
// (no size change, format 3) and an older record decodes with both unknown.
static void test_currents_survive_the_record() {
    std::vector<ct::Step> steps;
    ct::Step f; f.group = ct::Group::Pd; f.algorithm = ct::Algorithm::Pd3Fixed; f.pos = 1; f.verdict = ct::Verdict::Pass; f.mv = 9000; f.measured_mv = 9000;
    f.pdo = (0u << 30) | (9000u / 50 << 10) | 300u;            // fixed 9 V, 3 A
    steps.push_back(f);
    f.pos = 2; f.mv = 20000; f.measured_mv = 20000; f.pdo = (20000u / 50 << 10) | 225u;   // 20 V 2.25 A: lower than 3 A
    steps.push_back(f);
    ct::Step r; r.group = ct::Group::Pd; r.algorithm = ct::Algorithm::Pps; r.pos = 3; r.continuous = true; r.verdict = ct::Verdict::Pass;
    r.mv = 5000; r.measured_mv = 5000;
    r.pdo = 0xC0000000u | (110u << 17) | (50u << 8) | 60u;     // PPS 5-11 V, 60 x 50 mA = 3 A
    steps.push_back(r);
    const rc::Card c = rc::build(steps, evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None);
    CHECK_EQ(c.pd_ma, 3000);
    CHECK_EQ(c.n_ranges, 1);
    CHECK_EQ(c.range[0].ma, 3000);
    CHECK_EQ(c.range[0].lo_dv, 50);
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, c);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d == c, 1);
    CHECK_EQ(d.range[0].ma, 3000); CHECK_EQ(d.pd_ma, 3000);
    CHECK_EQ(d.range[0].lo_dv, 50);
    rec[4 + 33] = 2;                                           // format 2: those bits mean nothing
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.range[0].ma, 0); CHECK_EQ(d.pd_ma, 0);
}

// The 6 bit detail and 9+9 bit voltages of the group record: PD errors (128+) and the largest values survive.
static void test_group_codec_edges() {
    rc::Card c;
    c.group[0] = {rc::GV::Fail, 48000, uint8_t(128 + int(k2::pd::Err::BadChoice)), uint8_t(ct::Reason::Request), 51100};
    c.group[1] = {rc::GV::Fail, 51100, uint8_t(k2::fc::Fail::Inline), uint8_t(ct::Reason::Request), 100};
    c.group[2] = {rc::GV::Pass, 5000, 0, 0, 0};
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, c);
    CHECK_EQ(rec[4 + 33], rc::kFormat);                            // record format 5: cable block
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d == c, 1);
    CHECK_EQ(rc::kPayload + rc::kRecordOverhead, 202u);   // v7: 196 + 6 = 202
}

// V5: the cable block round-trips, and a v4 payload (format byte 4, zero-padded to kPayload) decodes with an
// empty cable and migrate_to_current raises the format bit — the in-memory conversion of old cards.
static void test_v5_cable_roundtrip_and_migration() {
    rc::Card c;
    c.cable = {42, 0x2B01, 0x0001, 50000, 5000, 2, rc::kCableObserved | rc::kCableSpoofed};
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, c);
    CHECK_EQ(rec[4 + 33], unsigned(rc::kFormat));
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.cable == c.cable, 1);
    CHECK_EQ(d == c, 1);
    // The Ra guard's reading (bits 3..5, Topology + 1) and "started anyway" (bit 6) ride in the same flags byte.
    c.cable.flags |= uint8_t((3 << rc::kCableGuardShift) & rc::kCableGuardMask) | rc::kCableGuardBypassed;
    rc::encode_card(rec, c);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.cable.flags, c.cable.flags);
    CHECK_EQ(unsigned(d.cable.flags & rc::kCableGuardMask) >> rc::kCableGuardShift, 3u);
    CHECK_EQ((d.cable.flags & rc::kCableGuardBypassed) != 0, true);
    c.cable.flags = rc::kCableObserved | rc::kCableSpoofed; rc::encode_card(rec, c);   // back to the plain card
    // legacy v4 record: format byte 4, cable area zero
    std::memset(rec, 0, sizeof rec);
    rc::Card old;
    old.group[0] = {rc::GV::Pass, 20000, 0, 0, 0};
    // hand-encode a v4 payload: reuse encode, then downgrade the format byte and truncate the cable block
    rc::encode_card(rec, old);
    rec[4 + 33] = 4;
    std::memset(rec + 4 + 162, 0, rc::kPayload - 162);   // a real v4 file has no cable block; padding must read as empty
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.format, 4u);
    CHECK_EQ(d.cable == rc::Cable{}, 1);
    CHECK_EQ(d.group[0].max_mv, 20000u);
    rc::migrate_to_current(d);
    CHECK_EQ(d.format, rc::kFormat);
    CHECK_EQ(d.cable == rc::Cable{}, 1);   // converted: format current, cable stays "none recorded"
}

// "Resumed N times": a run that lost power and went on carries the count in the cable block's reserved byte (no format bump:
// older cards hold 0 there and decode as never resumed). Everything else on the card equals the uninterrupted run's.
static void test_resumed_count_roundtrip() {
    std::vector<ct::Step> steps(2);
    steps[0].verdict = ct::Verdict::Pass; steps[0].mv = 5000; steps[1].verdict = ct::Verdict::Pass; steps[1].mv = 9000;
    ct::Evidence ev; ct::HandoverInfo h{};
    const rc::Card plain = rc::build(steps, ev, h, {}, {}, ct::State::Done, ct::Reason::None, {}, 0);
    const rc::Card again = rc::build(steps, ev, h, {}, {}, ct::State::Done, ct::Reason::None, {}, 3);
    CHECK_EQ(plain.resumed, 0u); CHECK_EQ(again.resumed, 3u);
    rc::Card back = again; back.resumed = 0;
    CHECK_EQ(back == plain, 1);                       // only the note differs
    CHECK_EQ(again == plain, 0);
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, again);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.resumed, 3u); CHECK_EQ(d == again, 1);
    rc::encode_card(rec, plain);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.resumed, 0u);
}

// A record written before format 1 (byte 33 = 0): group = verdict/reason, detail, max in mV. It decodes to the same group,
// with the max rounded to 100 mV and no failing target (the old record never kept one).
static void test_legacy_group_record_migrates() {
    uint8_t rec[rc::kMaxRecord]; rc::Card c, d;
    rc::encode_card(rec, c);
    uint8_t* p = rec + 4;
    p[33] = 0;
    uint8_t* q = p + rc::off::kGroups + 4 * unsigned(ct::Group::Qc2);
    q[0] = uint8_t(unsigned(rc::GV::Fail) << 6 | unsigned(ct::Reason::Request)); q[1] = uint8_t(k2::fc::Fail::NoBand);
    q[2] = uint8_t(11950 & 0xFF); q[3] = uint8_t(11950 >> 8);
    CHECK_EQ(rc::decode_card({p, rc::kPayload}, d), 1);
    const auto& g = d.group[unsigned(ct::Group::Qc2)];
    CHECK_EQ(int(g.verdict), int(rc::GV::Fail)); CHECK_EQ(g.max_mv, 12000); CHECK_EQ(g.fail_mv, 0);
    CHECK_EQ(g.detail, uint8_t(k2::fc::Fail::NoBand)); CHECK_EQ(g.reason, int(ct::Reason::Request));
}

static void test_handover_fields() {
    rc::Identity id{.fp = 1};
    rc::Context cx;
    ct::HandoverInfo h{ct::HandoverOutcome::FellBack, 6200};
    const rc::Card c = rc::build(run_a(), evidence(), h, id, cx, ct::State::Done, ct::Reason::None);
    CHECK_EQ(int(c.handover), int(ct::HandoverOutcome::FellBack));
    CHECK_EQ(c.handover_ds, 62);               // deciseconds
    h.ms = 70'000'000;                         // clamps instead of wrapping
    CHECK_EQ(rc::build(run_a(), evidence(), h, id, cx, ct::State::Done, ct::Reason::None).handover_ds, 65535);
}

static void test_codec_roundtrip_and_size() {
    static_assert(rc::kPayload <= 250, "a card record stays within 256 bytes");
    rc::Identity id{.fp = 0xAABBCCDD, .vid = 0x2717, .pid = 0x1234};
    rc::Card c = rc::build(run_a(), evidence(), {ct::HandoverOutcome::StayedPd, 60000}, id, {.fw = 0x1234ABCD, .boots = 3, .uptime_s = 99},
                           ct::State::Done, ct::Reason::None);
    c.card_no = 5; c.runs = 9;
    rc::set_name(c, "Baseus CJ21");
    std::array<uint8_t, rc::kMaxRecord> buf{};
    const size_t n = rc::encode_card(buf, c);
    CHECK_EQ(n, rc::kRecordOverhead + rc::kPayload);
    rc::Card d;
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d == c, 1);                               // every field survives
    CHECK_STR(d.name, "Baseus CJ21");
    CHECK_EQ(d.runs, 9); CHECK_EQ(d.card_no, 5);
    // A payload of the wrong length or with an out of range enum is refused.
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload - 2}, d), 0);
    buf[4 + rc::off::kGroups + 0] = 0xFF;              // verdict 3 reason 63: reason out of range
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, d), 0);
}

static void test_name_rules() {
    rc::Card c;
    rc::set_name(c, "A name that is much longer than twenty");
    CHECK_EQ(std::strlen(c.name), rc::kNameMax);       // cut, never overflowing
    CHECK_EQ(rc::valid_name("Anker 735"), 1);
    CHECK_EQ(rc::valid_name(""), 0);
    CHECK_EQ(rc::valid_name("bad/name"), 0);           // same alphabet as cable names: A-Z a-z 0-9 space - _ .
    CHECK_EQ(rc::valid_name("123456789012345678901"), 0);
}

static std::string auto_name(uint16_t vid, const char* mfr, uint8_t pdp) {
    char out[rc::kNameMax + 1] = {};
    rc::auto_name(out, vid, mfr, pdp);
    return out;
}

static void test_brand_table() {
    CHECK_STR(rc::brand(0x05AC), "Apple");
    CHECK_STR(rc::brand(0x04E8), "Samsung");
    CHECK_STR(rc::brand(0x2717), "Xiaomi");
    CHECK_STR(rc::brand(0x291A), "Anker");
    CHECK_STR(rc::brand(0x19D2), "ZTE");
    // Honor 0x339B, vivo 0x2D95 and Zimi/CUKTECH 0x2B01 are not in the usb.ids list, so they stay out of the table.
    CHECK_EQ(rc::brand(0x339B) == nullptr, 1);
    CHECK_EQ(rc::brand(0x2D95) == nullptr, 1);
    CHECK_EQ(rc::brand(0x2B01) == nullptr, 1);
    CHECK_EQ(rc::brand(0x0000) == nullptr, 1);
    CHECK_EQ(rc::brand(0x1234) == nullptr, 1);
    // Every brand is a valid card name on its own (and leaves room for " 240W").
    for (unsigned v = 0; v < 0x10000; ++v) if (const char* b = rc::brand(uint16_t(v))) CHECK_EQ(rc::valid_name(b) && std::strlen(b) <= rc::kNameMax - 5, 1);
}

static void test_auto_name_rules() {
    CHECK_STR(auto_name(0xFFFF, "", 100).c_str(), "");   // all-ones VID is unknown: no name
    CHECK_STR(auto_name(0x291A, "Anker Innova", 65).c_str(), "Anker 65W");         // brand from the VID wins over the mfr text
    CHECK_STR(auto_name(0x2717, "", 67).c_str(), "Xiaomi 67W");
    CHECK_STR(auto_name(0x9999, "Acme Power", 30).c_str(), "Acme Power 30W");      // unknown VID: the mfr string
    CHECK_STR(auto_name(0x9999, "", 30).c_str(), "VID 9999 30W");                  // then the VID
    CHECK_STR(auto_name(0x0ABC, "", 0).c_str(), "VID 0ABC");                       // no wattage: no suffix
    CHECK_STR(auto_name(0x05AC, "", 0).c_str(), "Apple");
    CHECK_STR(auto_name(0, "", 65).c_str(), "");                                   // nothing known: stays unnamed
    CHECK_STR(auto_name(0, "", 0).c_str(), "");
    CHECK_STR(auto_name(0, "Acme/Power*", 18).c_str(), "AcmePower 18W");           // characters outside the name alphabet drop
    CHECK_STR(auto_name(0, "  Acme  ", 18).c_str(), "Acme 18W");                   // no leading or trailing space
    CHECK_STR(auto_name(0, "A very long manufacturer", 140).c_str(), "A very long man 140W");   // base cut so the suffix fits 20
    CHECK_EQ(rc::valid_name(auto_name(0, "A very long manufacturer", 140)), 1);
    CHECK_EQ(rc::valid_name(auto_name(0x9999, "", 255)), 1);
}

static void test_codec_identity_fields_and_old_formats() {
    rc::Card c = rc::build(run_a(), evidence(), {ct::HandoverOutcome::StayedPd, 0}, {.fp = 7, .vid = 0x291A, .pid = 0x0102, .pdp_w = 65}, {},
                           ct::State::Done, ct::Reason::None);
    rc::set_auto_name(c, "");
    CHECK_STR(c.name, "Anker 65W"); CHECK_EQ(c.name_auto, 1);
    std::array<uint8_t, rc::kMaxRecord> buf{};
    rc::encode_card(buf, c);
    CHECK_EQ(buf[4 + 33], rc::kFormat);                // format 7 (repeat counter)
    CHECK_EQ(buf[4 + 54], 65);                         // pdp_w sits in the spare bytes after the 20 name characters
    CHECK_EQ(buf[4 + 55], 1);                          // flags: name is automatic
    rc::Card d;
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d == c, 1);
    CHECK_EQ(d.id.pdp_w, 65); CHECK_EQ(d.name_auto, 1);
    // A card written before format 2 has zero spare bytes (or stale ones): its name is a user name and pdp is unknown.
    buf[4 + 33] = 1; buf[4 + 54] = 99; buf[4 + 55] = 1;
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.id.pdp_w, 0); CHECK_EQ(d.name_auto, 0);
    buf[4 + 33] = rc::kFormat + 1;                     // a newer format is refused
    CHECK_EQ(rc::decode_card({buf.data() + 4, rc::kPayload}, d), 0);
}

static void test_inherit_rules() {
    rc::Card prev; prev.id = {.fp = 5, .vid = 0x2717, .pid = 3, .pdp_w = 67};
    rc::set_name(prev, "Desk charger");
    rc::Card now; now.id.fp = 5;
    rc::set_auto_name(now, "");                        // nothing known: stays empty
    rc::inherit(now, prev);
    CHECK_STR(now.name, "Desk charger"); CHECK_EQ(now.name_auto, 0);
    CHECK_EQ(now.id.vid, 0x2717); CHECK_EQ(now.id.pdp_w, 67);   // identity kept when the run learned none
    // A new automatic name never replaces a user name.
    rc::Card fresh; fresh.id = {.fp = 5, .vid = 0x05AC, .pid = 1, .pdp_w = 30};
    rc::set_auto_name(fresh, "");
    rc::inherit(fresh, prev);
    CHECK_STR(fresh.name, "Desk charger"); CHECK_EQ(fresh.name_auto, 0);
    CHECK_EQ(fresh.id.vid, 0x05AC);                    // what the run learned wins
    // An automatic name is refreshed by a later automatic name, or kept when the run has none.
    rc::Card a; a.id = {.fp = 5, .vid = 0x2717, .pdp_w = 67}; rc::set_auto_name(a, "");
    rc::Card b; b.id = {.fp = 5, .vid = 0x2717, .pdp_w = 100}; rc::set_auto_name(b, "");
    rc::inherit(b, a);
    CHECK_STR(b.name, "Xiaomi 100W");
    rc::Card blank; blank.id.fp = 5;
    rc::inherit(blank, b);
    CHECK_STR(blank.name, "Xiaomi 100W"); CHECK_EQ(blank.name_auto, 1);
    // First run: no previous card, nothing to do.
    rc::Card lone; lone.id.fp = 9;
    rc::Card none;
    rc::inherit(lone, none);
    CHECK_STR(lone.name, "");
}

static void test_fw_hash() {
    CHECK_EQ(rc::fw_hash("ff2c610"), 0x0FF2C610u);
    CHECK_EQ(rc::fw_hash("ff2c610-dirty"), 0x0FF2C610u);
    CHECK_EQ(rc::fw_hash("v1.2-3-gabcdef12"), 0xABCDEF12u);   // describe output: the commit token
    CHECK_EQ(rc::fw_hash("1234567890abc"), 0x12345678u);      // at most 8 digits
    CHECK_EQ(rc::fw_hash(""), 0u);
    CHECK_EQ(rc::fw_hash("unknown"), 0u);
}

// A source with no PD caps still gets an identity: a hash of what its legacy protocols answered.
static void test_legacy_fingerprint() {
    using G = ct::Group;
    const std::vector<ct::Step> bank = {
        step(A::Qc2, G::Qc2, 0, 5000, V::Pass, 5160), step(A::Qc2, G::Qc2, 0, 9000, V::Fail),
        step(A::Vooc, G::Vooc, 0, 5000, V::Fail),
    };
    auto other = bank; other[1] = step(A::Qc2, G::Qc2, 0, 9000, V::Pass, 9200);
    const uint32_t a = rc::legacy_fingerprint(bank);
    CHECK_EQ(a != 0, 1);
    CHECK_EQ(a == rc::legacy_fingerprint(bank), 1);
    CHECK_EQ(a == rc::legacy_fingerprint(other), 0);   // a higher passing level is another charger
    auto noise = bank; noise[0].measured_mv = 5190;
    CHECK_EQ(a == rc::legacy_fingerprint(noise), 1);   // the reading itself is not part of it
    CHECK_EQ(rc::legacy_fingerprint({}) != 0, 1);
    const uint32_t pdos[] = {0x2701912C};
    CHECK_EQ(a == rc::fingerprint(pdos, 2), 0);
}
// The two newest reasons and the newest state still decode: the codec bound follows the enums (ShutOff cards were rejected).
static void test_late_reasons_and_states_roundtrip() {
    for (ct::Reason why : {ct::Reason::ShutOff, ct::Reason::NoReattach, ct::Reason::CutRecovered, ct::Reason::BlockedByPd, ct::Reason::NoQc3Plus, ct::Reason::RebootLimit, ct::Reason::ClassAOnly, ct::Reason::NotUnlocked}) {
        std::vector<ct::Step> steps = {
            step(A::Pps, ct::Group::Pd, 4, 3300, V::Fail, 0, why, kPps, true),
        };
        const rc::Card c = rc::build(steps, evidence(), {}, {}, {}, ct::State::Attach, why);
        uint8_t rec[rc::kMaxRecord]; rc::Card d;
        rc::encode_card(rec, c);
        CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
        CHECK_EQ(d == c, 1);
        CHECK_EQ(int(d.state), int(ct::State::Attach));
        CHECK_EQ(int(d.reason), int(why));
    }
}

// The PD 3.2 SPR AVS APDO has no voltage fields: 9 V up to 20 V when the 20 V current (9:0) is set, else 15 V.
// UGREEN X865: 0xE004B145 = 15 V 3 A, 20 V 3.25 A. The PPS bit layout read it as "17.7-0.2 V".
static void test_spr_avs_range_limits() {
    constexpr uint32_t kX865Avs = 0xE004B145u, kAvs15 = 0xE004B000u;   // the second one has no 20 V current
    const std::vector<ct::Step> s = {
        step(A::SprAvs, ct::Group::Pd, 5, 20000, V::Pass, 20010, ct::Reason::None, kX865Avs, true, true),
        step(A::SprAvs, ct::Group::Pd, 6, 15000, V::Pass, 15010, ct::Reason::None, kAvs15, true, true),
    };
    const rc::Card c = rc::build(s, evidence(), {}, rc::Identity{.fp = 1}, {}, ct::State::Done, ct::Reason::None);
    CHECK_EQ(c.n_ranges, 2);
    CHECK_EQ(c.range[0].lo_dv, 90); CHECK_EQ(c.range[0].hi_dv, 200); CHECK_EQ(c.range[0].ma, 3250);
    CHECK_EQ(c.range[1].lo_dv, 90); CHECK_EQ(c.range[1].hi_dv, 150); CHECK_EQ(c.range[1].ma, 3000);
}

// A cable that cannot carry EPR: the charger still lists its levels (EPR_Get_Source_Cap in SPR mode), each stored as a named skip.
static void test_epr_levels_offered_without_an_epr_cable() {
    const std::vector<ct::Step> steps = {
        step(A::Epr, ct::Group::Pd, 0, 28000, V::Skip, 0, ct::Reason::CableLimit, 0, false, false, ct::kDetailEprOffered),
        step(A::Epr, ct::Group::Pd, 0, 36000, V::Skip, 0, ct::Reason::CableLimit, 0, false, false, ct::kDetailEprOffered),
        step(A::Epr, ct::Group::Pd, 0, 48000, V::Skip, 0, ct::Reason::CableLimit, 0, false, false, ct::kDetailEprOffered),
    };
    const rc::Card c = rc::build(steps, evidence(), {}, rc::Identity{.fp = 0xE0000002u}, {}, ct::State::Done, ct::Reason::None);
    for (unsigned i = 0; i < 3; ++i) {
        CHECK_EQ(c.epr[i].dv, 280u + 80u * i + (i == 2 ? 40u : 0u));
        CHECK_EQ(int(c.epr[i].r.verdict), int(rc::GV::Skip)); CHECK_EQ(c.epr[i].r.reason, int(ct::Reason::CableLimit));
        CHECK_EQ(c.epr[i].r.detail, ct::kDetailEprOffered);
        CHECK_STR(rc::group_why(unsigned(ct::Group::Pd), c.epr[i].r), "EPR offered: needs EPR cable");
    }
}
// EPR per-level verdicts: build() collects Algorithm::Epr steps and stores them in epr[].
static void test_epr_per_level_verdicts() {
    // EPR steps at 28V pass, 36V skip (above monitor), 48V skip (above monitor)
    const std::vector<ct::Step> steps = {
        step(A::Epr, ct::Group::Pd, 0, 28000, V::Pass, 28280, ct::Reason::None, 0, false, false, 0),
        step(A::Epr, ct::Group::Pd, 0, 36000, V::Skip, 0, ct::Reason::OverCap, 0, false, false, ct::kDetailMonitor36),
        step(A::Epr, ct::Group::Pd, 0, 48000, V::Skip, 0, ct::Reason::OverCap, 0, false, false, ct::kDetailMonitor48),
    };
    const rc::Card c = rc::build(steps, evidence(), {}, rc::Identity{.fp = 0xE0000001u}, {}, ct::State::Done, ct::Reason::None);
    // Check EPR entries
    // Note: dv_round rounds measured_mv, so 28280 -> 28300
    CHECK_EQ(c.epr[0].dv, 280u); CHECK_EQ(int(c.epr[0].r.verdict), int(rc::GV::Pass)); CHECK_EQ(c.epr[0].r.max_mv, 28300u);
    CHECK_EQ(c.epr[1].dv, 360u); CHECK_EQ(int(c.epr[1].r.verdict), int(rc::GV::Skip)); CHECK_EQ(c.epr[1].r.reason, int(ct::Reason::OverCap)); CHECK_EQ(c.epr[1].r.detail, ct::kDetailMonitor36);
    CHECK_EQ(c.epr[2].dv, 480u); CHECK_EQ(int(c.epr[2].r.verdict), int(rc::GV::Skip)); CHECK_EQ(c.epr[2].r.reason, int(ct::Reason::OverCap)); CHECK_EQ(c.epr[2].r.detail, ct::kDetailMonitor48);
    // Pd group entry has overall verdict: pass (from 28V pass) > skip
    const auto& pd = c.group[unsigned(ct::Group::Pd)];
    CHECK_EQ(int(pd.verdict), int(rc::GV::Pass));
    // Encode/decode round-trip
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, c);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d == c, 1);
}

// EPR: fail at one level wins over pass at another level
static void test_epr_fail_wins_over_pass() {
    const std::vector<ct::Step> steps = {
        step(A::Epr, ct::Group::Pd, 0, 28000, V::Pass, 28280),
        step(A::Epr, ct::Group::Pd, 0, 36000, V::Fail, 35000, ct::Reason::Voltage, 0, false, false, 0),
    };
    const rc::Card c = rc::build(steps, evidence(), {}, rc::Identity{.fp = 0xE0000002u}, {}, ct::State::Done, ct::Reason::None);
    // The Pd group should have fail verdict
    CHECK_EQ(int(c.group[unsigned(ct::Group::Pd)].verdict), int(rc::GV::Fail));
    // EPR entries: 28V pass, 36V fail
    CHECK_EQ(c.epr[0].dv, 280u); CHECK_EQ(int(c.epr[0].r.verdict), int(rc::GV::Pass));
    CHECK_EQ(c.epr[1].dv, 360u); CHECK_EQ(int(c.epr[1].r.verdict), int(rc::GV::Fail));
}

// EPR: more than 3 distinct targets saturates at 3 entries
static void test_epr_saturates_at_three() {
    const std::vector<ct::Step> steps = {
        step(A::Epr, ct::Group::Pd, 0, 28000, V::Pass, 28280),
        step(A::Epr, ct::Group::Pd, 0, 30000, V::Pass, 30280),
        step(A::Epr, ct::Group::Pd, 0, 36000, V::Pass, 36280),
        step(A::Epr, ct::Group::Pd, 0, 40000, V::Pass, 40280),
        step(A::Epr, ct::Group::Pd, 0, 48000, V::Pass, 48280),
    };
    const rc::Card c = rc::build(steps, evidence(), {}, rc::Identity{.fp = 0xE0000003u}, {}, ct::State::Done, ct::Reason::None);
    // Only first 3 distinct voltages stored
    CHECK_EQ(c.epr[0].dv, 280u);
    CHECK_EQ(c.epr[1].dv, 300u);
    CHECK_EQ(c.epr[2].dv, 360u);
    // 40V and 48V are not stored (no space) - epr[3] doesn't exist, array is only size 3
    // Verify that only 3 entries exist and the rest are zero
    CHECK_EQ(c.epr[0].dv != 0, true);
    CHECK_EQ(c.epr[1].dv != 0, true);
    CHECK_EQ(c.epr[2].dv != 0, true);
}

// EPR: migration from v3 (144-byte payload, format byte 3) with zero-padding
static void test_v3_migration_with_zero_padding() {
    // Create a v3 card (144 byte payload, format byte = 3)
    rc::Card c = rc::build(run_a(), evidence(), {}, rc::Identity{.fp = 0x10000001u}, {}, ct::State::Done, ct::Reason::None);
    uint8_t rec[rc::kMaxRecord];
    rc::encode_card(rec, c);
    // Override the format byte (payload byte 33, i.e. record offset 4 + 33) to 3 (v3)
    rec[4 + 33] = 3;
    // Decode with zero-padded payload (simulating an old 158-byte card file read from flash)
    std::array<uint8_t, rc::kPayload> payload{};
    std::memcpy(payload.data(), rec + 4, 144);  // copy only the 144 v3 payload bytes
    std::memset(payload.data() + 144, 0, rc::kPayload - 144);   // zero-pad to kPayload
    rc::Card d;
    CHECK_EQ(rc::decode_card(payload, d), 1);
    CHECK_EQ(d.format, 3);   // the decoded card remembers its format
    // EPR entries should be GV::None
    for (unsigned i = 0; i < 3; ++i) {
        CHECK_EQ(d.epr[i].dv, 0u);
        CHECK_EQ(int(d.epr[i].r.verdict), int(rc::GV::None));
    }
}

// A format byte newer than the firmware's kFormat is refused (a current-format payload is not)
static void test_format_byte_too_new_is_refused() {
    std::array<uint8_t, rc::kPayload> payload{};
    payload[33] = rc::kFormat + 1;
    rc::Card d;
    CHECK_EQ(rc::decode_card(payload, d), 0);  // should fail
    payload[33] = rc::kFormat;
    CHECK_EQ(rc::decode_card(payload, d), 1);
}

// EPR: non-empty epr_levels round-trip with encode/decode
static void test_epr_encode_decode_roundtrip() {
    rc::Card c;
    c.id.fp = 0x20000001u;
    c.epr[0] = {280, {.verdict = rc::GV::Pass, .max_mv = 28280u, .detail = 0, .reason = 0, .fail_mv = 0}};
    c.epr[1] = {360, {.verdict = rc::GV::Skip, .max_mv = 0, .detail = ct::kDetailMonitor36, .reason = uint8_t(ct::Reason::OverCap), .fail_mv = 0}};
    c.epr[2] = {480, {.verdict = rc::GV::Fail, .max_mv = 47000u, .detail = 0, .reason = uint8_t(ct::Reason::Voltage), .fail_mv = 47500u}};
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, c);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.format, rc::kFormat);   // round-trip keeps the v4 format
    // Check EPR entries match (note: max_mv/fail_mv are rounded via dv_round during encode/decode)
    CHECK_EQ(d.epr[0].dv, 280u); CHECK_EQ(int(d.epr[0].r.verdict), int(rc::GV::Pass)); CHECK_EQ(d.epr[0].r.max_mv, 28300u);  // 28280 -> dv_round -> 283 -> *100
    CHECK_EQ(d.epr[1].dv, 360u); CHECK_EQ(int(d.epr[1].r.verdict), int(rc::GV::Skip)); CHECK_EQ(d.epr[1].r.detail, ct::kDetailMonitor36);
    CHECK_EQ(d.epr[2].dv, 480u); CHECK_EQ(int(d.epr[2].r.verdict), int(rc::GV::Fail)); CHECK_EQ(d.epr[2].r.max_mv, 47000u);  // 47000 is already rounded
}

// V6: the UFCS modes (up to four, 3 bytes each at payload 178): lo and hi voltage in 100 mV, the mode's current in 50 mA.
// The builder takes them from the UFCS steps (Step::pdo, ct::pack_ufcs_mode), first appearance first, repeats dropped.
static ct::Step ufcs_step(unsigned mv, int lo_mv, int hi_mv, int ma) {
    return step(A::Ufcs, ct::Group::Ufcs, 0, mv, V::Pass, mv, ct::Reason::None, ct::pack_ufcs_mode(lo_mv, hi_mv, 0, ma));
}
static void test_v6_ufcs_modes() {
    static_assert(rc::kMaxUfcs == 4);
    CHECK_EQ(rc::kFormat, 7u);
    CHECK_EQ(unsigned(rc::off::kUfcs), 178u);
    CHECK_EQ(unsigned(rc::off::kRepeat), 190u);
    CHECK_EQ(rc::kPayload, 196u);
    CHECK_EQ(rc::kMaxRecord, 202u);
    std::vector<ct::Step> steps = {
        ufcs_step(5000, 3400, 5500, 3000),
        ufcs_step(5000, 5000, 11000, 3000), ufcs_step(11000, 5000, 11000, 3000),      // the same mode again: one entry
        ufcs_step(20000, 5000, 20000, 5000),
    };
    rc::Card c = rc::build(steps, evidence(), {}, rc::Identity{.fp = 0x30000001u}, {}, ct::State::Done, ct::Reason::None);
    CHECK_EQ(c.format, 7u);
    CHECK_EQ(rc::ufcs_modes(c), 3u);
    CHECK_EQ(c.ufcs[0].lo_dv, 34u); CHECK_EQ(c.ufcs[0].hi_dv, 55u); CHECK_EQ(c.ufcs[0].ma, 3000u);
    CHECK_EQ(c.ufcs[1].lo_dv, 50u); CHECK_EQ(c.ufcs[1].hi_dv, 110u); CHECK_EQ(c.ufcs[1].ma, 3000u);
    CHECK_EQ(c.ufcs[2].lo_dv, 50u); CHECK_EQ(c.ufcs[2].hi_dv, 200u); CHECK_EQ(c.ufcs[2].ma, 5000u);
    CHECK_EQ(c.ufcs[3].hi_dv, 0u);
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    CHECK_EQ(rc::encode_card(rec, c), size_t(rc::kMaxRecord));
    CHECK_EQ(rec[4 + 33], 7u);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d == c, 1);
    CHECK_EQ(d.format, 7u);
    // u24 per mode, little endian: bits 0..7 lo, 8..16 hi, 17..23 current.
    const uint32_t m2 = rec[4 + 178 + 6] | uint32_t(rec[4 + 178 + 7]) << 8 | uint32_t(rec[4 + 178 + 8]) << 16;
    CHECK_EQ(m2, 50u | 200u << 8 | 100u << 17);
    // A fifth mode is dropped, not wrapped.
    steps.push_back(ufcs_step(9000, 3300, 9000, 2000));
    steps.push_back(ufcs_step(9000, 3300, 12000, 2000));
    c = rc::build(steps, evidence(), {}, rc::Identity{.fp = 0x30000001u}, {}, ct::State::Done, ct::Reason::None);
    CHECK_EQ(rc::ufcs_modes(c), 4u);
    CHECK_EQ(c.ufcs[3].lo_dv, 33u); CHECK_EQ(c.ufcs[3].hi_dv, 90u);
    // A card with no UFCS mode (a plain run, or UFCS rows planned before the list was read) has none.
    CHECK_EQ(rc::ufcs_modes(rc::build(run_a(), evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None)), 0u);
    ct::Step bare = step(A::Ufcs, ct::Group::Ufcs, 0, 5000, V::Pass, 5000);
    CHECK_EQ(rc::ufcs_modes(rc::build(std::span<const ct::Step>(&bare, 1), evidence(), {}, {}, {}, ct::State::Done, ct::Reason::None)), 0u);
    // Saturation: the current at 6.35 A, the low end at 25.5 V.
    rc::Card big;
    big.ufcs[0] = {400, 600, 9000};
    rc::encode_card(rec, big);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.ufcs[0].lo_dv, 255u); CHECK_EQ(d.ufcs[0].hi_dv, 511u); CHECK_EQ(d.ufcs[0].ma, 6350u);
}
// A v5 payload (178 bytes, zero-padded to kPayload as the store does) decodes with no UFCS modes whatever the padding holds,
// and migrate_to_current raises it to v6.
static void test_v5_payload_has_no_ufcs_modes() {
    rc::Card c;
    c.group[0] = {rc::GV::Pass, 20000, 0, 0, 0};
    c.ufcs[0] = {50, 200, 3000};
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, c);
    rec[4 + 33] = 5;                                  // as a v5 firmware wrote it: format 5 ...
    std::memset(rec + 4 + 178, 0xEE, 12);            // ... and whatever lies past byte 178 is not a mode
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.format, 5u);
    CHECK_EQ(rc::ufcs_modes(d), 0u);
    CHECK_EQ(d.group[0].max_mv, 20000u);
    rc::migrate_to_current(d);
    CHECK_EQ(d.format, 7u);
    CHECK_EQ(rc::ufcs_modes(d), 0u);
}

// V7: the repeat block (payload 190): times u8 (the runs that gave exactly these results, 1 = once), last_boots u16 and
// last_uptime_s u24 (the newest of them). A v6 payload (190 bytes, zero-padded; its CRC bytes sit where the block is) reads as
// "tested once" whatever those bytes hold, and a card that was never repeated carries times 1.
static void test_v7_repeat_block() {
    rc::Card c;
    c.group[0] = {rc::GV::Pass, 20000, 0, 0, 0};
    CHECK_EQ(c.times, 1u);                                   // a fresh card was run once
    c.times = 3; c.last_boots = 0x1234; c.last_uptime_s = 0xABCDEF;
    uint8_t rec[rc::kMaxRecord]; rc::Card d;
    rc::encode_card(rec, c);
    CHECK_EQ(rec[4 + 190], 3u);
    CHECK_EQ(rec[4 + 191] | rec[4 + 192] << 8, 0x1234);
    CHECK_EQ(rec[4 + 193] | rec[4 + 194] << 8 | rec[4 + 195] << 16, 0xABCDEF);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d == c, 1);
    CHECK_EQ(d.times, 3u); CHECK_EQ(d.last_boots, 0x1234u); CHECK_EQ(d.last_uptime_s, 0xABCDEFu);
    // Saturation: the count at 255, the boot at 65535, the uptime at 2^24 - 1.
    c.times = 255; c.last_boots = 65535; c.last_uptime_s = 0xFFFFFFFFu;
    rc::encode_card(rec, c);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.times, 255u); CHECK_EQ(d.last_boots, 65535u); CHECK_EQ(d.last_uptime_s, 0xFFFFFFu);
    // A zero count in a v7 payload (never written by this firmware) reads as once.
    rec[4 + 190] = 0;
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.times, 1u);
    // A v6 payload: format byte 6, and the bytes at 190..195 are the old CRC and padding, not a repeat block.
    c.times = 1; c.last_boots = 0; c.last_uptime_s = 0;
    rc::encode_card(rec, c);
    rec[4 + 33] = 6;
    std::memset(rec + 4 + 190, 0xEE, 6);
    CHECK_EQ(rc::decode_card({rec + 4, rc::kPayload}, d), 1);
    CHECK_EQ(d.format, 6u);
    CHECK_EQ(d.times, 1u); CHECK_EQ(d.last_boots, 0u); CHECK_EQ(d.last_uptime_s, 0u);
    CHECK_EQ(d.group[0].max_mv, 20000u);
    rc::migrate_to_current(d);
    CHECK_EQ(d.format, 7u);
    CHECK_EQ(d.times, 1u);
}

// The identity rule of a re-run (same_results): the semantic results, never the clock. Everything below is built the way a run
// builds its card, from steps.
static rc::Card run_card(const std::vector<ct::Step>& steps, uint32_t boots = 1, uint32_t uptime = 100, uint32_t fw = 0x1234ABCD) {
    return rc::build(steps, evidence(), {ct::HandoverOutcome::StayedPd, 60000}, rc::Identity{.fp = 0xAABB0001u, .vid = 0x2717, .pid = 7, .pdp_w = 65},
                     {.fw = fw, .boots = boots, .uptime_s = uptime}, ct::State::Done, ct::Reason::None);
}
static void test_same_results_ignores_the_clock_and_bookkeeping() {
    const rc::Card a = run_card(run_a(), 1, 100);
    rc::Card b = run_card(run_a(), 9, 5000);       // another boot, another uptime
    b.card_no = 4; b.runs = 12; b.times = 7; b.last_boots = 3; b.last_uptime_s = 9;
    rc::set_name(b, "Renamed");
    b.handover_ds = a.handover_ds + 50;            // the handover took longer: timing, not a result
    b.resumed = 2;                                 // the run lost power and went on
    b.cable.seq = 99; b.cable.flags = rc::kCableSpoofed;   // library entry number and the guard's reading are bookkeeping
    CHECK_EQ(rc::same_results(a, b), true);
    CHECK_EQ(rc::same_results(b, a), true);
    CHECK_EQ(rc::same_results(a, a), true);
}
static void test_same_results_notices_any_row() {
    const rc::Card a = run_card(run_a());
    auto changed = [&](auto edit) { std::vector<ct::Step> s = run_a(); edit(s); return rc::same_results(a, run_card(s)); };
    // One verdict, one reason, the failing target, a range's failure, a skip's cause: each makes a new run.
    CHECK_EQ(changed([](auto& s) { s[5].verdict = V::Fail; s[5].reason = ct::Reason::Voltage; }), false);      // QC3 pass -> fail
    CHECK_EQ(changed([](auto& s) { s[6].reason = ct::Reason::Timeout; }), false);                               // AFC skip, another reason
    CHECK_EQ(changed([](auto& s) { s[4].reason = ct::Reason::Request; }), false);                              // PD fail, another reason
    CHECK_EQ(changed([](auto& s) { s[4].mv = 15000; }), false);                                                  // the PPS target that failed
    CHECK_EQ(changed([](auto& s) { s[3].verdict = V::Fail; }), false);                                           // the PPS top step failed too
    CHECK_EQ(changed([](auto& s) { s.pop_back(); }), false);                                                     // AFC not part of the run
    CHECK_EQ(changed([](auto& s) { s[2].measured_mv = 15000; }), false);                                         // the PD max moved far
    CHECK_EQ(changed([](auto&) {}), true);
    // A different firmware build, or a different charger (PDO count, evidence flags), is a new run.
    CHECK_EQ(rc::same_results(a, run_card(run_a(), 1, 100, 0x77777777u)), false);
    rc::Card d = a; d.num_pdos = 5;     CHECK_EQ(rc::same_results(a, d), false);
    d = a; d.ev_flags ^= 1;             CHECK_EQ(rc::same_results(a, d), false);
    d = a; d.state = ct::State::Stopped; CHECK_EQ(rc::same_results(a, d), false);
    d = a; d.cable.max_ma = 3000;       CHECK_EQ(rc::same_results(a, d), false);
    d = a; d.handover = ct::HandoverOutcome::InContract; CHECK_EQ(rc::same_results(a, d), false);
    d = a; d.ufcs[0] = {50, 200, 3000}; CHECK_EQ(rc::same_results(a, d), false);
}
// What the card keeps is 100 mV: two runs whose measured levels round to the same step are the same card, and a level that sits on
// a rounding boundary and fell into the next step (noise below the card's resolution) is still the same run. A change of two
// steps is a real change, and so is a moved target (fail_mv, a range's fail_dv).
static void test_same_results_tolerates_one_stored_step_of_noise() {
    auto with = [](unsigned pd_meas, unsigned qc_meas, unsigned range_meas) {
        std::vector<ct::Step> s = run_a();
        s[2].measured_mv = uint16_t(pd_meas); s[5].measured_mv = uint16_t(qc_meas); s[3].measured_mv = uint16_t(range_meas);
        return run_card(s);
    };
    const rc::Card a = with(19950, 8900, 20990);                // stored: 20.0 V, 8.9 V, range 21.0 V
    CHECK_EQ(rc::same_results(a, with(19951, 8949, 20999)), true);   // below the resolution: the same stored values
    CHECK_EQ(rc::same_results(a, with(19949, 8951, 20940)), true);   // 19.9 / 9.0 / 20.9: one step across a boundary
    CHECK_EQ(rc::same_results(a, with(19849, 8900, 20990)), false);  // 19.8 V: two steps below the stored 20.0 V
    CHECK_EQ(rc::same_results(a, with(19950, 8700, 20990)), false);  // 8.7 V against 8.9 V
    CHECK_EQ(rc::same_results(a, with(19950, 8900, 20790)), false);  // the range reached 20.8 V against 21.0 V
    // A moved target is a different failure, however small the move.
    auto fail_at = [](unsigned fixed_mv, unsigned pps_mv) {
        std::vector<ct::Step> s = run_a();
        s[5].verdict = V::Fail; s[5].mv = uint16_t(fixed_mv); s[5].reason = ct::Reason::Voltage;
        s[4].mv = uint16_t(pps_mv);
        return run_card(s);
    };
    CHECK_EQ(rc::same_results(fail_at(9000, 13000), fail_at(9000, 13000)), true);
    CHECK_EQ(rc::same_results(fail_at(9000, 13000), fail_at(9100, 13000)), false);   // group fail_mv
    CHECK_EQ(rc::same_results(fail_at(9000, 13000), fail_at(9000, 13100)), false);   // range fail_dv
}


int main() {
    test_spr_avs_range_limits();
    test_late_reasons_and_states_roundtrip();
    test_legacy_fingerprint();
    test_fw_hash();
    test_fingerprint();
    test_build_groups_and_ranges();
    test_two_pps_ranges_and_fixed_max();
    test_group_keeps_max_pass_and_first_fail();
    test_capped_range_keeps_its_ceiling_and_reason();
    test_group_detail_for_detected_and_handshake_ok();
    test_group_why_names();
    test_qc_label();
    test_moto_turbopower_label();
    test_samsung_sfc_labels();
    test_oppo_svid_note();
    test_scp_capability_note();
    test_qc3plus_without_response_is_a_skipped_group();
    test_class_b_on_class_a_is_a_skip_not_a_fail();
    test_group_codec_edges();
    test_v5_cable_roundtrip_and_migration(); test_resumed_count_roundtrip();
    test_currents_survive_the_record();
    test_legacy_group_record_migrates();
    test_handover_fields();
    test_codec_roundtrip_and_size();
    test_name_rules();
    test_brand_table();
    test_auto_name_rules();
    test_codec_identity_fields_and_old_formats();
    test_inherit_rules();
    test_epr_per_level_verdicts();
    test_epr_levels_offered_without_an_epr_cable();
    test_epr_fail_wins_over_pass();
    test_epr_saturates_at_three();
    test_v3_migration_with_zero_padding();
    test_format_byte_too_new_is_refused();
    test_epr_encode_decode_roundtrip();
    test_qc_label_justification();
    test_v6_ufcs_modes();
    test_v5_payload_has_no_ufcs_modes();
    test_v7_repeat_block();
    test_same_results_ignores_the_clock_and_bookkeeping();
    test_same_results_notices_any_row();
    test_same_results_tolerates_one_stored_step_of_noise();
    return test_report("test_report_card");
}
