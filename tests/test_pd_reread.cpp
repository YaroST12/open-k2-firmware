// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the source-cap re-read sequencer (k2/pd/pd_reread.hpp): the after-contract conversation
// (Get_Source_Cap_Extended, Discover Identity, Discover SVIDs, Get_Source_Cap), one message at a time, with the wire
// words of the AVS-unlocking exchange.
#include <vector>

#include "k2/pd/pd_reread.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

struct Rig {
    Reread r;
    std::uint32_t now = 8000;
    std::vector<TxMsg> sent;
    RereadLink link{.contract = true, .pdos = 6, .rev = 0};
    std::uint16_t svids_[4] = {};
    void start(std::uint8_t mask = kRereadDefault, std::uint8_t rev = 2, std::uint8_t pdos = 6, bool attach = false, RereadOpts opts = {}) {
        r.start(rev, mask, pdos, now, attach, opts);
    }
    void set_svids(std::initializer_list<std::uint16_t> l) {
        unsigned n = 0;
        for (std::uint16_t s : l) svids_[n++] = s;
        link.svids = {svids_, n};
    }
    bool step(bool tx_ok = true, bool idle = true) {
        r.tick(now, idle, link);
        TxMsg m;
        bool out = false;
        if (r.take_tx(m)) { sent.push_back(m); r.on_tx_result(tx_ok, now); out = true; }
        now += 10;
        return out;
    }
    bool next(unsigned limit_ms = 1000) {
        for (unsigned t = 0; t < limit_ms; t += 10) if (step()) return true;
        return false;
    }
    void deliver(Sop sop, std::uint16_t header, const std::uint8_t* d, std::size_t n) { r.on_message(sop, header, {d, n}, now); }
    void control(std::uint8_t type) { deliver(Sop::Sop, make_header(type, 0, 2), nullptr, 0); }
    // VDM answer: ctype 1 ACK, 2 NAK, 3 BUSY, for command `cmd` (1 identity, 2 SVIDs).
    void vdm(std::uint8_t cmd, std::uint8_t ctype) {
        const std::uint8_t d[4] = {std::uint8_t(cmd | (ctype << 6)), 0xA0, 0x00, 0xFF};
        deliver(Sop::Sop, make_header(kMsgVendorDefined, 1, 2), d, 4);
    }
    void ext_caps() {
        const std::uint8_t d[28] = {24, 0x80};   // chunked, chunk 0, 24 bytes of data (content irrelevant here)
        deliver(Sop::Sop, std::uint16_t(0x8000 | (7u << 12) | (2u << 6) | kExtSourceCapsExt), d, 28);   // Source_Capabilities_Extended, 7 DOs
    }
    // Source_Capabilities with `n` PDOs: five fixed ones, and when avs the last is an SPR AVS APDO (bits 31:30 = 11, 29:28 = 10).
    void source_cap(unsigned n, bool avs) {
        std::uint8_t d[28] = {};
        for (unsigned i = 0; i < n; i++) {
            std::uint32_t w = 0x0001912C;   // fixed 5 V 3 A
            if (avs && i + 1 == n) w = (3u << 30) | (2u << 28) | (150u << 10) | 40u;   // AVS APDO (range fields irrelevant here)
            for (int b = 0; b < 4; b++) d[4 * i + b] = std::uint8_t(w >> (8 * b));
        }
        deliver(Sop::Sop, make_header(kDataSourceCap, std::uint8_t(n), 2), d, 4 * n);
    }
};

void test_message_order_and_words() {
    Rig g; g.start();
    CHECK_EQ(g.next(), true);
    CHECK_EQ(g.sent[0].header, 0x0091);                  // Get_Source_Cap_Extended, rev 2, id 0
    CHECK_EQ(g.sent[0].len, 0);
    g.ext_caps();
    CHECK_EQ(g.next(), true);
    CHECK_EQ(g.sent[1].header, 0x108F);                  // Discover Identity VDM
    CHECK_EQ(g.sent[1].data[0], 0x01); CHECK_EQ(g.sent[1].data[1], 0xA0);
    CHECK_EQ(g.sent[1].data[2], 0x00); CHECK_EQ(g.sent[1].data[3], 0xFF);   // 0xFF00A001
    g.vdm(1, 1);
    CHECK_EQ(g.next(), true);
    CHECK_EQ(g.sent[2].header, 0x108F);
    CHECK_EQ(g.sent[2].data[0], 0x02); CHECK_EQ(g.sent[2].data[1], 0xA0);   // 0xFF00A002 Discover SVIDs
    g.vdm(2, 1);
    CHECK_EQ(g.next(), true);
    CHECK_EQ(g.sent[3].header, 0x0087);                  // Get_Source_Cap
    CHECK_EQ(g.sent[3].len, 0);
    g.source_cap(7, true);
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.r.status().ok_mask, 15);
    CHECK_EQ(g.r.status().skipped_mask, 0);
    CHECK_EQ(g.r.status().pdos_before, 6);
    CHECK_EQ(g.r.status().pdos_last, 7);
    CHECK_EQ(g.r.status().caps_rx, 1);
    CHECK_EQ(g.r.status().avs_seen, true);
    CHECK_EQ(g.sent.size(), 4u);
}

void test_get_source_cap_only() {
    Rig g; g.start(kRereadGetCap);
    CHECK_EQ(g.next(), true);
    CHECK_EQ(g.sent[0].header, 0x0087);
    g.source_cap(6, false);
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.r.status().pdos_last, 6);
    CHECK_EQ(g.r.status().avs_seen, false);
    CHECK_EQ(g.next(200), false);
}

void test_second_get_source_cap_and_header_id_free() {
    Rig g; g.start(kRereadGetCap | kRereadGetCap2);
    CHECK_EQ(g.next(), true);
    g.source_cap(6, false);
    CHECK_EQ(g.next(), true);
    CHECK_EQ(g.sent[1].header, 0x0087);
    g.source_cap(7, true);
    CHECK_EQ(g.r.status().caps_rx, 2);
    CHECK_EQ(g.r.status().pdos_last, 7);
    CHECK_EQ(g.r.status().avs_seen, true);
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
}

void test_refusals_skip_the_step_without_retry() {
    Rig g; g.start();
    g.next(); g.control(kCtlNotSupported);               // Get_Source_Cap_Extended not supported
    CHECK_EQ(g.r.status().skipped_mask, 1);
    g.next(); g.vdm(1, 2);                               // NAK to Discover Identity
    g.next(); g.vdm(2, 3);                               // BUSY to Discover SVIDs
    g.next(); g.source_cap(6, false);
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.r.status().skipped_mask, 7);
    CHECK_EQ(g.r.status().ok_mask, 8);
    CHECK_EQ(g.sent.size(), 4u);
}

void test_silent_source_ends_within_budget() {
    Rig g; g.start();
    const std::uint32_t t0 = g.now;
    while (g.r.running() && g.now - t0 < 10000) g.step();
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.now - t0 <= kRereadTotalMs, true);
    CHECK_EQ(g.sent.size(), 4u);
    CHECK_EQ(g.r.status().ok_mask, 0);
    CHECK_EQ(g.r.status().skipped_mask, 15);
}

void test_pd2_source_has_no_extended_messages() {
    Rig g; g.start(kRereadDefault, 1);
    CHECK_EQ(g.next(), true);
    CHECK_EQ(g.sent[0].header & 0x1F, kMsgVendorDefined);   // Get_Source_Cap_Extended does not exist in PD 2.0
    CHECK_EQ(g.sent[0].data[1], 0x80);                      // VDM 1.0
    CHECK_EQ(g.r.status().skipped_mask & 1, 1);
}

void test_waits_for_idle_line_and_a_failed_tx_skips() {
    Rig g; g.start(kRereadGetCap);
    for (int i = 0; i < 50; i++) CHECK_EQ(g.step(true, false), false);   // line busy: nothing goes out
    CHECK_EQ(g.step(false, true), true);                                  // no GoodCRC: the step is skipped
    CHECK_EQ(g.r.status().skipped_mask, kRereadGetCap);
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
}

void test_foreign_traffic_and_abort() {
    Rig g; g.start(kRereadGetCap);
    g.next();
    const std::uint8_t d[4] = {1, 0xA0, 0, 0xFF};
    g.deliver(Sop::SopP, make_header(kMsgVendorDefined, 1, 2), d, 4);   // the cable answering, not us
    g.source_cap(6, false);                                              // fine: SOP source cap counts
    CHECK_EQ(g.r.status().caps_rx, 1);
    Rig h; h.start();
    h.next();
    h.r.abort();
    CHECK_EQ(h.r.running(), false);
    CHECK_EQ(h.next(100), false);
}

void test_avs_detection_is_the_spr_pattern_only() {
    const std::uint32_t spr_avs = (3u << 30) | (2u << 28) | 0x0640;   // bits 31:30 = 11, 29:28 = 10
    const std::uint32_t epr_avs = (3u << 30) | (1u << 28);            // bits 29:28 = 01 is EPR AVS: not the SPR APDO
    const std::uint32_t pps = (3u << 30) | (0u << 28);
    CHECK_EQ(is_spr_avs_pdo(spr_avs), true);
    CHECK_EQ(is_spr_avs_pdo(epr_avs), false);
    CHECK_EQ(is_spr_avs_pdo(pps), false);
    CHECK_EQ(is_spr_avs_pdo(0x0001912C), false);
}

// The experiment path makes its own contract: the run waits for it (the driver Requests PDO 1), lets the line settle so a
// charger that asks for our Sink_Capabilities after PS_RDY is heard first, and only then starts the steps and the budget.
void test_attach_waits_for_the_contract_then_settles() {
    Rig g; g.link.contract = false; g.link.pdos = 0;
    g.start(kRereadGetCap, 2, 0, true);
    CHECK_EQ(g.r.running(), true);
    CHECK_EQ(g.r.status().attaching, true);
    for (int i = 0; i < 300; i++) CHECK_EQ(g.step(), false);            // 3 s without a contract: nothing is sent, still waiting
    CHECK_EQ(g.r.running(), true);
    CHECK_EQ(g.r.status().attaching, true);
    g.link.contract = true; g.link.pdos = 6;
    const std::uint32_t t_contract = g.now;
    bool sent = false;
    while (!sent && g.now - t_contract < 3000) sent = g.step();
    CHECK_EQ(sent, true);
    CHECK_EQ(g.now - t_contract >= kRereadSettleMs, true);              // the settle time passed first
    CHECK_EQ(g.now - t_contract <= kRereadSettleMs + 50, true);
    CHECK_EQ(g.sent[0].header, 0x0087);
    CHECK_EQ(g.r.status().attaching, false);
    CHECK_EQ(g.r.status().pdos_before, 6);                              // PDOs held once the contract and the settle are over
    g.source_cap(7, true);
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.r.status().avs_seen, true);
    CHECK_EQ(g.r.status().attach_failed, false);
}

void test_attach_budget_starts_after_the_settle() {
    Rig g; g.link.contract = false;
    g.start(kRereadDefault, 2, 0, true);
    for (int i = 0; i < 600; i++) g.step();                             // 6 s waiting for the contract
    g.link.contract = true;
    const std::uint32_t t0 = g.now;
    while (g.r.running() && g.now - t0 < 10000) g.step();               // silent source: all steps time out
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.r.status().attach_failed, false);
    CHECK_EQ(g.r.status().skipped_mask, 15);
    CHECK_EQ(g.now - t0 <= kRereadSettleMs + kRereadTotalMs + 50, true);
    CHECK_EQ(g.sent.size(), 4u);
}

void test_attach_that_never_gets_a_contract_fails_without_sending() {
    Rig g; g.link.contract = false;
    g.start(kRereadDefault, 2, 0, true);
    const std::uint32_t t0 = g.now;
    while (g.r.running() && g.now - t0 < 20000) g.step();
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.r.status().attach_failed, true);
    CHECK_EQ(g.r.status().attaching, false);
    CHECK_EQ(g.r.status().ok_mask, 0);
    CHECK_EQ(g.r.status().skipped_mask, kRereadDefault);
    CHECK_EQ(g.now - t0 >= kRereadAttachMs, true);
    CHECK_EQ(g.now - t0 <= kRereadAttachMs + 50, true);
    CHECK_EQ(g.sent.size(), 0u);
}

void test_attach_takes_the_wire_revision_from_the_link() {
    Rig g; g.link.contract = false;
    g.start(kRereadCapsExt | kRereadGetCap, 2, 0, true);
    g.link.contract = true; g.link.rev = 1;                             // the source turned out to be PD 2.0
    CHECK_EQ(g.next(3000), true);
    CHECK_EQ(g.sent[0].header, 0x0047);                                 // Get_Source_Cap, rev 1 (no Get_Source_Cap_Extended)
    CHECK_EQ(g.r.status().skipped_mask & kRereadCapsExt, kRereadCapsExt);
}

void test_link_reset_only_ends_a_run_that_is_past_the_attach() {
    Rig g; g.link.contract = false;
    g.start(kRereadGetCap, 2, 0, true);
    g.r.on_link_reset();                                                // a hard reset while the contract is still being made
    CHECK_EQ(g.r.running(), true);
    CHECK_EQ(g.r.status().attaching, true);
    Rig h; h.start(kRereadGetCap);
    h.next();
    h.r.on_link_reset();                                                // a hard reset in the middle of the conversation
    CHECK_EQ(h.r.running(), false);
    CHECK_EQ(h.r.status().state == RereadState::Aborted, true);
}

void test_an_existing_contract_still_settles_before_the_first_message() {
    Rig g;
    g.start(kRereadGetCap, 2, 6, true);                                 // attach asked, contract already there
    const std::uint32_t t0 = g.now;
    bool sent = false;
    while (!sent && g.now - t0 < 3000) sent = g.step();
    CHECK_EQ(sent, true);
    CHECK_EQ(g.now - t0 >= kRereadSettleMs, true);
}

// ---- the survey's extension: Manufacturer_Info, Discover Modes per SVID, the survey order, pacing, per-step outcome ----

constexpr std::uint8_t kSurveyMask = kRereadIdentity | kRereadSvids | kRereadModes | kRereadCapsExt | kRereadMfrInfo;

// Discover Modes answer for `svid` (cmd 3); ctype 1 = ACK, 2 = NAK.
void modes_ack(Rig &g, std::uint16_t svid, std::uint8_t ctype = 1) {
    const std::uint8_t d[8] = {std::uint8_t(3 | (ctype << 6)), 0xA0, std::uint8_t(svid), std::uint8_t(svid >> 8), 0x01, 0x00, 0x01, 0x00};
    g.deliver(Sop::Sop, make_header(kMsgVendorDefined, 2, 2), d, 8);
}

void mfr_info_answer(Rig &g) {
    const std::uint8_t d[8] = {26, 0x80};
    g.deliver(Sop::Sop, std::uint16_t(0x8000 | (7u << 12) | (2u << 6) | kExtManufacturerInfo), d, 8);
}

void test_lab_order_is_unchanged_and_mfr_info_comes_last() {
    Rig g; g.start(kRereadDefault | kRereadMfrInfo);
    CHECK_EQ(g.next(), true); CHECK_EQ(g.sent[0].header, 0x0091); g.ext_caps();
    CHECK_EQ(g.next(), true); g.vdm(1, 1);
    CHECK_EQ(g.next(), true); g.vdm(2, 1);
    CHECK_EQ(g.next(), true); CHECK_EQ(g.sent[3].header, 0x0087); g.source_cap(6, false);
    CHECK_EQ(g.next(), true);
    CHECK_EQ(g.sent[4].header, std::uint16_t(0x8000 | (1u << 12) | (2u << 6) | kExtGetManufacturerInfo));   // unchunked extended, one DO
    CHECK_EQ(g.sent[4].len, 4);
    CHECK_EQ(g.sent[4].data[0], 2);
    CHECK_EQ(g.sent[4].data[1], 0);
    mfr_info_answer(g);
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.r.status().ok_mask, kRereadDefault | kRereadMfrInfo);
}

void test_survey_order_identity_svids_modes_capsext_mfr() {
    Rig g;
    RereadOpts o; o.survey_order = true;
    g.start(kSurveyMask, 2, 6, false, o);
    g.set_svids({0xFF01, 0x2717});
    CHECK_EQ(g.next(), true); CHECK_EQ(g.sent[0].header, 0x108F); CHECK_EQ(g.sent[0].data[0], 0x01); g.vdm(1, 1);
    CHECK_EQ(g.next(), true); CHECK_EQ(g.sent[1].data[0], 0x02); g.vdm(2, 1);
    CHECK_EQ(g.next(), true);                                                    // Discover Modes for the first SVID
    CHECK_EQ(g.sent[2].header, 0x108F);
    CHECK_EQ(g.sent[2].data[0], 0x03); CHECK_EQ(g.sent[2].data[1], 0xA0);
    CHECK_EQ(g.sent[2].data[2], 0x01); CHECK_EQ(g.sent[2].data[3], 0xFF);        // 0xFF01A003
    modes_ack(g, 0xFF01);
    CHECK_EQ(g.next(), true);                                                    // then the second
    CHECK_EQ(g.sent[3].data[2], 0x17); CHECK_EQ(g.sent[3].data[3], 0x27);
    modes_ack(g, 0x2717);
    CHECK_EQ(g.next(), true); CHECK_EQ(g.sent[4].header, 0x0091); g.ext_caps();  // Get_Source_Cap_Extended
    CHECK_EQ(g.next(), true); CHECK_EQ(g.sent[5].header & 0x8000, 0x8000);       // Get_Manufacturer_Info
    mfr_info_answer(g);
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.r.status().ok_mask, kSurveyMask);
    CHECK_EQ(g.r.status().skipped_mask, 0);
    CHECK_EQ(g.sent.size(), 6u);
}

void test_modes_outcome_follows_the_svid_answers() {
    {   // one ACK among refusals is a Got
        Rig g; RereadOpts o; o.survey_order = true;
        g.start(kRereadModes, 2, 6, false, o);
        g.set_svids({0xFF01, 0x2717});
        g.next(); modes_ack(g, 0xFF01, 2);       // NAK
        g.next(); modes_ack(g, 0x2717);
        CHECK_EQ(g.r.status().ok_mask, kRereadModes);
        CHECK_EQ(g.r.status().notsup_mask, 0);
    }
    {   // every SVID refused: NotSupported
        Rig g; RereadOpts o; o.survey_order = true;
        g.start(kRereadModes, 2, 6, false, o);
        g.set_svids({0xFF01});
        g.next(); modes_ack(g, 0xFF01, 2);
        CHECK_EQ(g.r.status().state == RereadState::Done, true);
        CHECK_EQ(g.r.status().ok_mask, 0);
        CHECK_EQ(g.r.status().notsup_mask, kRereadModes);
        CHECK_EQ(g.r.status().skipped_mask, kRereadModes);
    }
    {   // silence on an SVID: no answer, and no retry
        Rig g; RereadOpts o; o.survey_order = true;
        g.start(kRereadModes, 2, 6, false, o);
        g.set_svids({0xFF01});
        g.next();
        for (unsigned t = 0; t < 2 * kRereadRespMs && g.r.running(); t += 10) g.step();
        CHECK_EQ(g.r.status().state == RereadState::Done, true);
        CHECK_EQ(g.r.status().skipped_mask, kRereadModes);
        CHECK_EQ(g.r.status().notsup_mask, 0);
        CHECK_EQ(g.sent.size(), 1u);
    }
    {   // no SVID to ask about: nothing is sent and nothing is claimed
        Rig g; RereadOpts o; o.survey_order = true;
        g.start(kRereadModes, 2, 6, false, o);
        g.set_svids({});
        g.next(100);
        CHECK_EQ(g.sent.size(), 0u);
        CHECK_EQ(g.r.status().state == RereadState::Done, true);
        CHECK_EQ(g.r.status().notsup_mask, 0);
        CHECK_EQ(g.r.status().skipped_mask, kRereadModes);
    }
}

void test_modes_asks_at_most_the_first_three_svids() {
    Rig g; RereadOpts o; o.survey_order = true;
    g.start(kRereadModes, 2, 6, false, o);
    g.set_svids({0xFF01, 0x2717, 0x1234, 0x5678});
    for (int i = 0; i < 6; i++) {
        if (!g.next()) break;
        modes_ack(g, std::uint16_t(g.sent.back().data[2] | (g.sent.back().data[3] << 8)));
    }
    CHECK_EQ(g.sent.size(), unsigned(kRereadModeSvids));
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
}

void test_notsup_mask_tells_refusal_from_silence() {
    Rig g; RereadOpts o; o.survey_order = true;
    g.start(kRereadIdentity | kRereadSvids | kRereadCapsExt | kRereadMfrInfo, 2, 6, false, o);
    g.next(); g.vdm(1, 2);                      // NAK: refusal
    g.next(); g.vdm(2, 3);                      // BUSY: the source is occupied, not a refusal
    g.next(); g.control(kCtlNotSupported);      // Get_Source_Cap_Extended
    g.next(); g.control(kCtlReject);            // Get_Manufacturer_Info
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.r.status().skipped_mask, kRereadIdentity | kRereadSvids | kRereadCapsExt | kRereadMfrInfo);
    CHECK_EQ(g.r.status().notsup_mask, kRereadIdentity | kRereadCapsExt | kRereadMfrInfo);
}

void test_a_failed_transmit_is_silence_not_refusal() {
    Rig g; RereadOpts o; o.survey_order = true;
    g.start(kRereadIdentity, 2, 6, false, o);
    g.step(false, true);                        // sent, no GoodCRC
    CHECK_EQ(g.r.status().skipped_mask, kRereadIdentity);
    CHECK_EQ(g.r.status().notsup_mask, 0);
}

void test_pd2_source_skips_both_extended_steps() {
    Rig g; RereadOpts o; o.survey_order = true;
    g.start(kSurveyMask, 1, 6, false, o);
    g.set_svids({});
    CHECK_EQ(g.r.status().skipped_mask, kRereadCapsExt | kRereadMfrInfo);
    g.next(); g.vdm(1, 1);
    g.next(); g.vdm(2, 1);
    g.next(100);
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.sent.size(), 2u);
}

void test_pacing_spaces_the_messages() {
    Rig g; RereadOpts o; o.survey_order = true; o.pace_ms = 220;
    g.start(kRereadIdentity | kRereadSvids, 2, 6, false, o);
    CHECK_EQ(g.next(), true);
    const std::uint32_t t0 = g.now;
    g.vdm(1, 1);
    CHECK_EQ(g.next(1000), true);
    CHECK_EQ(g.now - t0 >= 220, true);
    CHECK_EQ(g.now - t0 < 300, true);
}

void test_survey_budget_is_longer_than_the_lab_one() {
    Rig g; RereadOpts o; o.survey_order = true; o.pace_ms = 1000; o.total_ms = 6000;
    g.start(kSurveyMask, 2, 6, false, o);
    g.set_svids({0xFF01});
    for (unsigned t = 0; t < 2600; t += 10) g.step();   // silent source: the lab budget of kRereadTotalMs would have cut it
    CHECK_EQ(g.r.running(), true);
    for (unsigned t = 0; t < 6500 && g.r.running(); t += 10) g.step();
    CHECK_EQ(g.r.running(), false);
    CHECK_EQ(g.r.status().state == RereadState::Done, true);
    CHECK_EQ(g.r.status().skipped_mask, kSurveyMask);
}

}  // namespace

int main() {
    test_lab_order_is_unchanged_and_mfr_info_comes_last();
    test_survey_order_identity_svids_modes_capsext_mfr();
    test_modes_outcome_follows_the_svid_answers();
    test_modes_asks_at_most_the_first_three_svids();
    test_notsup_mask_tells_refusal_from_silence();
    test_a_failed_transmit_is_silence_not_refusal();
    test_pd2_source_skips_both_extended_steps();
    test_pacing_spaces_the_messages();
    test_survey_budget_is_longer_than_the_lab_one();
    test_attach_waits_for_the_contract_then_settles();
    test_attach_budget_starts_after_the_settle();
    test_attach_that_never_gets_a_contract_fails_without_sending();
    test_attach_takes_the_wire_revision_from_the_link();
    test_link_reset_only_ends_a_run_that_is_past_the_attach();
    test_an_existing_contract_still_settles_before_the_first_message();
    test_message_order_and_words();
    test_get_source_cap_only();
    test_second_get_source_cap_and_header_id_free();
    test_refusals_skip_the_step_without_retry();
    test_silent_source_ends_within_budget();
    test_pd2_source_has_no_extended_messages();
    test_waits_for_idle_line_and_a_failed_tx_skips();
    test_foreign_traffic_and_abort();
    test_avs_detection_is_the_spr_pattern_only();
    return test_report("test_pd_reread");
}
