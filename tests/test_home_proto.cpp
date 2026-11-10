// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home v7 protocol row source: what the firmware knows about the charger (PD contract and caps, the
// passive D+/D- verdict, the Type-C Rp current) turned into one ProtoInfo.
#include "test_util.h"
#include "k2/ui/home_proto.hpp"

using namespace k2;
using namespace k2::home;
using fastcharge::Protocol;

static pd::Pdo fixed(uint16_t mv, uint16_t ma) {
    pd::Pdo p;
    p.type = pd::PdoType::Fixed; p.raw = 1; p.min_mv = mv; p.max_mv = mv; p.max_ma = ma; p.max_mw = uint32_t(mv) * ma / 1000;
    return p;
}

// A 140 W EPR charger: 5/9/15/20 V fixed, a 3.3-21 V PPS, EPR 28 V 5 A at position 8, an AVS 15-28 V.
static pd::SourceCaps caps_epr() {
    pd::SourceCaps c;
    c.valid = true; c.epr = true; c.num_pdos = 8;
    c.pdos[0] = fixed(5000, 3000);
    c.pdos[1] = fixed(9000, 3000);
    c.pdos[2] = fixed(15000, 3000);
    c.pdos[3] = fixed(20000, 5000);
    c.pdos[4].type = pd::PdoType::Pps; c.pdos[4].raw = 1; c.pdos[4].min_mv = 3300; c.pdos[4].max_mv = 21000; c.pdos[4].max_ma = 5000;
    c.pdos[5] = fixed(28000, 5000);
    c.pdos[6].type = pd::PdoType::Avs; c.pdos[6].raw = 1; c.pdos[6].min_mv = 15000; c.pdos[6].max_mv = 28000; c.pdos[6].max_mw = 140000;
    c.pdos[7] = fixed(28000, 5000);
    return c;
}

static ProtoSource pd_source(const pd::SourceCaps &c, uint8_t pos, uint16_t mv) {
    ProtoSource s;
    s.pd_link = true;
    s.caps = &c;
    s.ctr.valid = true; s.ctr.ready = true; s.ctr.pos = pos; s.ctr.mv = mv; s.ctr.op_ma = 2000; s.ctr.max_ma = 3000;
    s.fc = Protocol::Pd;
    s.vbus_mv = mv;
    return s;
}

static void test_pd_fixed() {
    const pd::SourceCaps c = caps_epr();
    const ProtoInfo p = derive_proto(pd_source(c, 4, 20000));
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::PdFixed));
    CHECK_EQ(p.mv, 20000);
    CHECK_EQ(p.ma, 5000);                 // the PDO's rating, not the Request's operating current
    CHECK_EQ(p.max_mv, 28000);            // the highest voltage the source offers
}

static void test_pd_epr() {
    const pd::SourceCaps c = caps_epr();
    const ProtoInfo p = derive_proto(pd_source(c, 8, 28000));
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::PdEpr));
    CHECK_EQ(p.mv, 28000);
    CHECK_EQ(p.ma, 5000);
}

static void test_pd_pps() {
    const pd::SourceCaps c = caps_epr();
    const ProtoInfo p = derive_proto(pd_source(c, 5, 9000));
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::Pps));
    CHECK_EQ(p.lo_mv, 3300);
    CHECK_EQ(p.hi_mv, 21000);
    CHECK_EQ(p.ma, 5000);
}

static void test_pd_avs() {
    const pd::SourceCaps c = caps_epr();
    const ProtoInfo p = derive_proto(pd_source(c, 7, 20000));
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::Avs));
    CHECK_EQ(p.lo_mv, 15000);
    CHECK_EQ(p.hi_mv, 28000);
    CHECK_EQ(p.mw, 140000);
    CHECK_EQ(p.ma, 5000);                 // 140 W at 20 V is 7 A: capped at the 5 A of a standard cable
}

// ROADMAP "PD 0V5.82A" row: a contract decoded without its Source_Capabilities has an unknown PDO
// type. Never decode the RDO with a guessed layout (a PPS voltage reads as a max current): show the
// measured level and no current - "PD 9V", tag only - and no "max" either without caps.
static void test_pd_contract_without_caps_shows_the_measured_level_and_no_current() {
    ProtoSource s;
    s.pd_link = true;
    s.ctr.valid = true; s.ctr.ready = true; s.ctr.pos = 2; s.ctr.mv = 9000; s.ctr.max_ma = 3000;
    s.vbus_mv = 9090;
    ProtoInfo p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::PdFixed));
    CHECK_EQ(p.mv, 9000);                 // the measured level, not ctr.mv (0 on the wire)
    CHECK_EQ(p.ma, 0);                    // no current: the RDO layout is a guess without caps
    CHECK_EQ(p.max_mv, 0);                // the source's maximum is unknown too
    // The caps pointer set but invalid (the K2 booted mid-contract, pd_link is up): same rule.
    const pd::SourceCaps empty{};
    s.caps = &empty;
    p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::PdFixed));
    CHECK_EQ(p.mv, 9000);
    CHECK_EQ(p.ma, 0);
    CHECK_EQ(p.max_mv, 0);
    // Caps valid but the Request's position is beyond them: the PDO type is still unknown, the
    // source's top voltage is known and stays.
    const pd::SourceCaps c = caps_epr();
    s.caps = &c;
    s.ctr.pos = 9;
    p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::PdFixed));
    CHECK_EQ(p.mv, 9000);
    CHECK_EQ(p.ma, 0);
    CHECK_EQ(p.max_mv, 28000);
}

// A Request repeated every <= 10 s with no caps is the PPS keep-alive pattern: label it PPS with the
// measured VBUS ("PPS 9V"). A longer gap, or a single Request (no period known), stays "PD 9V".
static void test_periodic_request_without_caps_is_a_pps_keepalive() {
    ProtoSource s;
    s.pd_link = true;
    s.ctr.valid = true; s.ctr.ready = true; s.ctr.pos = 5;
    s.vbus_mv = 9090;
    s.req_period_ms = 500;
    ProtoInfo p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::Pps));
    CHECK_EQ(p.mv, 9000);
    CHECK_EQ(p.lo_mv, 9000);
    CHECK_EQ(p.hi_mv, 9000);
    CHECK_EQ(p.ma, 0);
    CHECK_EQ(p.max_mv, 0);
    s.req_period_ms = 10000;              // exactly 10 s: still the keep-alive
    CHECK_EQ(static_cast<int>(derive_proto(s).kind), static_cast<int>(ProtoKind::Pps));
    s.req_period_ms = 10001;              // slower: not the keep-alive pattern
    p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::PdFixed));
    CHECK_EQ(p.ma, 0);
    s.req_period_ms = 0;                  // one Request only: no period known
    CHECK_EQ(static_cast<int>(derive_proto(s).kind), static_cast<int>(ProtoKind::PdFixed));
}

static void test_non_pd_protocols() {
    ProtoSource s;
    s.vbus_mv = 8940;
    s.fc = Protocol::Qc2_9V;
    ProtoInfo p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::Qc2));
    CHECK_EQ(p.mv, 9000);                 // the level, not the sagging measurement
    CHECK_EQ(p.max_mv, 12000);
    CHECK_EQ(p.ma, 0);                    // QC announces no current rating
    s.fc = Protocol::Qc3;
    p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::Qc3));
    CHECK_EQ(p.lo_mv, 3600);
    CHECK_EQ(p.hi_mv, 12000);
    s.fc = Protocol::Qc2_20V;
    CHECK_EQ(derive_proto(s).max_mv, 20000);
    s.fc = Protocol::AfcFcp;
    p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::Fcp));
    CHECK_EQ(p.mv, 9000);
    s.fc = Protocol::Scp;
    CHECK_EQ(static_cast<int>(derive_proto(s).kind), static_cast<int>(ProtoKind::Scp));
    s.fc = Protocol::VoocWarp;
    CHECK_EQ(static_cast<int>(derive_proto(s).kind), static_cast<int>(ProtoKind::Svooc));
    s.fc = Protocol::Svooc;
    CHECK_EQ(static_cast<int>(derive_proto(s).kind), static_cast<int>(ProtoKind::Svooc));
}

static void test_type_c_and_dcp() {
    ProtoSource s;
    s.vbus_mv = 5100;
    s.fc = Protocol::NoQuick;
    s.typec_ma = 3000;
    ProtoInfo p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::TypeC));
    CHECK_EQ(p.mv, 5000);
    CHECK_EQ(p.ma, 3000);                 // the Rp current is the limit
    CHECK_EQ(p.max_mv, 5000);
    s.fc = Protocol::Dcp5V;
    s.typec_ma = 0;
    p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::Dcp));
    CHECK_EQ(p.mv, 5000);
    CHECK_EQ(p.ma, 0);
    s.fc = Protocol::Apple24;
    p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::Dcp));
    CHECK_EQ(p.ma, 2400);                 // the divider levels are a current rating
    s.fc = Protocol::Apple10;
    CHECK_EQ(derive_proto(s).ma, 1000);
}

static void test_pending_unknown_and_none() {
    ProtoSource s;
    s.fc = Protocol::Pending;
    CHECK_EQ(static_cast<int>(derive_proto(s).kind), static_cast<int>(ProtoKind::Detecting));
    s.fc = Protocol::NoInput;
    CHECK_EQ(static_cast<int>(derive_proto(s).kind), static_cast<int>(ProtoKind::None));
    s.fc = Protocol::PdLikely;            // no contract and no caps: no PD was decoded
    s.vbus_mv = 20100;
    ProtoInfo p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::TypeC));
    CHECK_EQ(p.mv, 20000);
    CHECK_EQ(p.ma, 0);
}

static void test_pd_labels_need_decoded_pd() {
    // A PD-ish D+/D- verdict without a decoded contract and without Source_Capabilities must not
    // produce a "PD" label: show what is known (the level, the Rp current), like the plain 5 V setup.
    const Protocol pdish[] = {Protocol::Pd, Protocol::PdMtk, Protocol::PdPe, Protocol::PdLikely};
    for (const Protocol fc : pdish) {
        ProtoSource s;
        s.fc = fc;
        s.vbus_mv = 8940;
        s.typec_ma = 1500;
        const ProtoInfo p = derive_proto(s);
        CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::TypeC));
        CHECK_EQ(p.mv, 9000);
        CHECK_EQ(p.ma, 1500);
    }
    // Source_Capabilities were seen (no contract yet): the caps justify the PD label.
    const pd::SourceCaps c = caps_epr();
    ProtoSource s;
    s.fc = Protocol::Pd;
    s.caps = &c;
    s.vbus_mv = 20100;
    const ProtoInfo p = derive_proto(s);
    CHECK_EQ(static_cast<int>(p.kind), static_cast<int>(ProtoKind::PdFixed));
    CHECK_EQ(p.mv, 20000);
    CHECK_EQ(p.ma, 0);
}

int main() {
    test_pd_fixed();
    test_pd_epr();
    test_pd_pps();
    test_pd_avs();
    test_pd_contract_without_caps_shows_the_measured_level_and_no_current();
    test_periodic_request_without_caps_is_a_pps_keepalive();
    test_non_pd_protocols();
    test_type_c_and_dcp();
    test_pending_unknown_and_none();
    test_pd_labels_need_decoded_pd();
    return test_report("test_home_proto");
}
