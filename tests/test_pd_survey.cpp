// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the Connection page's survey (k2/pd/pd_survey.hpp): the ordered plan of one Refresh, the gating (inline sink,
// charger test, Xiaomi chain, PD 2.0, no EPR bit), the status mapping (answered, Not_Supported / Reject, silence), and the two
// promises the page makes: nothing active before Refresh, and nothing sent while the sink is not Ready.
#include "k2/pd/pd_survey.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

constexpr SvCtx kOpen{.contract = true, .inline_sink = false, .charger_test = false, .xiaomi_chain = false, .pd20 = false, .epr_capable = true};

SvState st(const Survey &s, SvField f) { return s.entry(f).state; }
SvWhy why(const Survey &s, SvField f) { return s.entry(f).why; }

// Runs the whole plan with every action answered: returns the actions in the order they were handed out.
struct Walk { SvAct acts[8]; int n = 0; };
Walk walk(Survey &s, std::uint32_t &now, SvOutcome o = SvOutcome::Answered) {
    Walk w;
    for (int guard = 0; guard < 400 && s.running(); guard++) {   // 10 ms per poll, a pause of kSurveyPaceMs between actions
        const SvAct a = s.poll(now, true, true);
        now += 10;
        if (a == SvAct::None) continue;
        w.acts[w.n++] = a;
        switch (a) {
        case SvAct::Caps: s.set(SvField::Caps, o); break;
        case SvAct::Wire:
            for (SvField f : {SvField::Identity, SvField::Svids, SvField::Modes, SvField::CapsExt, SvField::MfrInfo}) s.set(f, o);
            break;
        case SvAct::Epr: s.set(SvField::EprList, o); break;
        case SvAct::Cable: s.set(SvField::Cable, o); break;
        default: break;
        }
        s.finish(now);
    }
    return w;
}

void test_nothing_active_before_refresh() {
    Survey s;
    for (unsigned f = 0; f < kSvFields; f++) CHECK_EQ(int(s.entry(SvField(f)).state), int(SvState::NotAsked));
    for (int i = 0; i < 50; i++) CHECK_EQ(int(s.poll(1000 + 300u * i, true, true)), int(SvAct::None));   // never started: never sends
    CHECK_EQ(s.running() ? 1 : 0, 0);
    s.clear();
    CHECK_EQ(int(s.poll(99999, true, true)), int(SvAct::None));
    CHECK_EQ(int(s.wire_mask()), 0);
}

void test_plan_order() {
    Survey s;
    std::uint32_t now = 1000;
    s.start(kOpen, now);
    for (unsigned f = 0; f < kSvFields; f++) CHECK_EQ(int(st(s, SvField(f))), int(SvState::Pending));
    // caps, then the source's VDM / extended conversation as one wire run, then the EPR list, then the cable
    const Walk w = walk(s, now);
    CHECK_EQ(w.n, 4);
    CHECK_EQ(int(w.acts[0]), int(SvAct::Caps));
    CHECK_EQ(int(w.acts[1]), int(SvAct::Wire));
    CHECK_EQ(int(w.acts[2]), int(SvAct::Epr));
    CHECK_EQ(int(w.acts[3]), int(SvAct::Cable));
    for (unsigned f = 0; f < kSvFields; f++) CHECK_EQ(int(st(s, SvField(f))), int(SvState::Got));
    CHECK_EQ(s.running() ? 1 : 0, 0);
    CHECK_EQ(int(s.poll(now + 5000, true, true)), int(SvAct::None));   // finished: it never sends again by itself
}

void test_wire_mask_is_the_plan_order_of_the_source_steps() {
    Survey s;
    s.start(kOpen, 0);
    CHECK_EQ(int(s.wire_mask()), int(kRereadIdentity | kRereadSvids | kRereadModes | kRereadCapsExt | kRereadMfrInfo));
    Survey p;
    SvCtx c = kOpen; c.pd20 = true;
    p.start(c, 0);
    CHECK_EQ(int(p.wire_mask()), int(kRereadIdentity | kRereadSvids | kRereadModes));   // PD 2.0 has no extended messages
}

void test_blocked_inline_sends_nothing() {
    Survey s;
    SvCtx c = kOpen; c.inline_sink = true;
    s.start(c, 0);
    for (unsigned f = 0; f < kSvFields; f++) {
        CHECK_EQ(int(st(s, SvField(f))), int(SvState::Blocked));
        CHECK_EQ(int(why(s, SvField(f))), int(SvWhy::Inline));
    }
    CHECK_EQ(s.running() ? 1 : 0, 0);   // nothing left to ask: the run is over at once
    CHECK_EQ(int(s.poll(500, true, true)), int(SvAct::None));
}

void test_blocked_charger_test_and_xiaomi_chain() {
    Survey a;
    SvCtx c = kOpen; c.charger_test = true;
    a.start(c, 0);
    for (unsigned f = 0; f < kSvFields; f++) CHECK_EQ(int(why(a, SvField(f))), int(SvWhy::ChargerTest));
    CHECK_EQ(int(a.poll(10, true, true)), int(SvAct::None));
    Survey b;
    c = kOpen; c.xiaomi_chain = true;
    b.start(c, 0);
    for (unsigned f = 0; f < kSvFields; f++) CHECK_EQ(int(why(b, SvField(f))), int(SvWhy::XiaomiChain));
    CHECK_EQ(int(b.poll(10, true, true)), int(SvAct::None));
    // inline wins over a test that happens to run, a test over the Xiaomi chain
    Survey d;
    c = kOpen; c.inline_sink = true; c.charger_test = true; c.xiaomi_chain = true;
    d.start(c, 0);
    CHECK_EQ(int(why(d, SvField(SvField::Identity))), int(SvWhy::Inline));
    Survey e;
    c = kOpen; c.charger_test = true; c.xiaomi_chain = true;
    e.start(c, 0);
    CHECK_EQ(int(why(e, SvField(SvField::Identity))), int(SvWhy::ChargerTest));
}

void test_pd20_skips_the_pd3_steps() {
    Survey s;
    SvCtx c = kOpen; c.pd20 = true;
    s.start(c, 0);
    CHECK_EQ(int(st(s, SvField::CapsExt)), int(SvState::Blocked));
    CHECK_EQ(int(why(s, SvField::CapsExt)), int(SvWhy::Pd20));
    CHECK_EQ(int(why(s, SvField::MfrInfo)), int(SvWhy::Pd20));
    CHECK_EQ(int(why(s, SvField::EprList)), int(SvWhy::Pd20));
    CHECK_EQ(int(st(s, SvField::Identity)), int(SvState::Pending));
    CHECK_EQ(int(st(s, SvField::Modes)), int(SvState::Pending));
    std::uint32_t now = 0;
    const Walk w = walk(s, now);
    CHECK_EQ(w.n, 3);   // Caps, Wire, Cable: no EPR action on PD 2.0
    CHECK_EQ(int(w.acts[2]), int(SvAct::Cable));
    CHECK_EQ(int(st(s, SvField::CapsExt)), int(SvState::Blocked));   // answered fields do not touch a blocked one
}

void test_epr_list_needs_the_epr_bit() {
    Survey s;
    SvCtx c = kOpen; c.epr_capable = false;
    s.start(c, 0);
    CHECK_EQ(int(st(s, SvField::EprList)), int(SvState::Blocked));
    CHECK_EQ(int(why(s, SvField::EprList)), int(SvWhy::NoEpr));
    std::uint32_t now = 0;
    const Walk w = walk(s, now);
    CHECK_EQ(w.n, 3);
}

void test_status_mapping() {
    Survey s;
    s.start(kOpen, 0);
    s.set(SvField::Identity, SvOutcome::Answered);
    s.set(SvField::Svids, SvOutcome::Refused);   // Not_Supported, Reject or a NAK
    s.set(SvField::Modes, SvOutcome::Silent);    // timeout
    CHECK_EQ(int(st(s, SvField::Identity)), int(SvState::Got));
    CHECK_EQ(int(st(s, SvField::Svids)), int(SvState::NotSupported));
    CHECK_EQ(int(st(s, SvField::Modes)), int(SvState::NoAnswer));
    CHECK_EQ(int(st(s, SvField::MfrInfo)), int(SvState::Pending));
    // a second answer does not rewrite a resolved field
    s.set(SvField::Identity, SvOutcome::Silent);
    CHECK_EQ(int(st(s, SvField::Identity)), int(SvState::Got));
}

void test_finish_turns_unanswered_fields_of_the_action_into_no_answer() {
    Survey s;
    s.start(kOpen, 0);
    CHECK_EQ(int(s.poll(0, true, true)), int(SvAct::Caps));
    s.finish(5);   // the caps never came
    CHECK_EQ(int(st(s, SvField::Caps)), int(SvState::NoAnswer));
    CHECK_EQ(int(s.poll(600, true, true)), int(SvAct::Wire));
    s.set(SvField::Identity, SvOutcome::Answered);
    s.finish(700);
    CHECK_EQ(int(st(s, SvField::Identity)), int(SvState::Got));
    CHECK_EQ(int(st(s, SvField::Svids)), int(SvState::NoAnswer));
    CHECK_EQ(int(st(s, SvField::MfrInfo)), int(SvState::NoAnswer));
    CHECK_EQ(int(st(s, SvField::EprList)), int(SvState::Pending));   // not asked yet
}

void test_one_action_in_flight_and_paced() {
    Survey s;
    s.start(kOpen, 0);
    CHECK_EQ(int(s.poll(0, true, true)), int(SvAct::Caps));
    for (std::uint32_t t = 1; t < 400; t += 7) CHECK_EQ(int(s.poll(t, true, true)), int(SvAct::None));   // the first is still out
    s.set(SvField::Caps, SvOutcome::Answered);
    s.finish(400);
    CHECK_EQ(int(s.poll(401, true, true)), int(SvAct::None));                          // paced
    CHECK_EQ(int(s.poll(400 + kSurveyPaceMs - 1, true, true)), int(SvAct::None));
    CHECK_EQ(int(s.poll(400 + kSurveyPaceMs, true, true)), int(SvAct::Wire));
}

void test_never_sends_while_the_sink_is_not_ready_or_the_line_busy() {
    Survey s;
    s.start(kOpen, 0);
    CHECK_EQ(int(s.poll(10, false, true)), int(SvAct::None));    // sink mid-negotiation
    CHECK_EQ(int(s.poll(20, true, false)), int(SvAct::None));    // line not idle
    CHECK_EQ(int(s.poll(30, false, false)), int(SvAct::None));
    CHECK_EQ(int(st(s, SvField::Caps)), int(SvState::Pending));  // still waiting, not given up
    CHECK_EQ(int(s.poll(40, true, true)), int(SvAct::Caps));
    s.set(SvField::Caps, SvOutcome::Answered);
    s.finish(50);
    for (std::uint32_t t = 51; t < 4000; t += 50) CHECK_EQ(int(s.poll(t, false, true)), int(SvAct::None));
    CHECK_EQ(int(s.poll(4001, true, true)), int(SvAct::Wire));   // Ready again: it goes on
}

void test_a_start_without_contract_asks_nothing() {
    Survey s;
    SvCtx c = kOpen; c.contract = false;
    s.start(c, 0);
    CHECK_EQ(s.running() ? 1 : 0, 0);
    CHECK_EQ(int(s.poll(10, true, true)), int(SvAct::None));
    for (unsigned f = 0; f < kSvFields; f++) CHECK_EQ(int(st(s, SvField(f))), int(SvState::NotAsked));   // the hint stays
}

void test_the_whole_survey_is_bounded() {
    Survey s;
    s.start(kOpen, 0);
    CHECK_EQ(int(s.poll(0, true, true)), int(SvAct::Caps));      // in flight, never answered
    CHECK_EQ(int(s.poll(kSurveyTotalMs - 1, true, true)), int(SvAct::None));
    CHECK_EQ(int(s.poll(kSurveyTotalMs, true, true)), int(SvAct::Abort));   // the glue stops the sub-run
    CHECK_EQ(s.running() ? 1 : 0, 0);
    for (unsigned f = 0; f < kSvFields; f++) CHECK_EQ(int(st(s, SvField(f))), int(SvState::NoAnswer));
    CHECK_EQ(int(s.poll(kSurveyTotalMs + 10, true, true)), int(SvAct::None));
    // a sink that never gets Ready ends the run too, with the fields named, never as a stuck "Reading"
    Survey t;
    t.start(kOpen, 100);
    for (std::uint32_t x = 100; x < 100 + kSurveyTotalMs + 50; x += 25) t.poll(x, false, true);
    CHECK_EQ(t.running() ? 1 : 0, 0);
    CHECK_EQ(int(st(t, SvField::Caps)), int(SvState::NoAnswer));
}

void test_counts_and_progress() {
    Survey s;
    s.start(kOpen, 0);
    CHECK_EQ(int(s.planned()), 8);
    CHECK_EQ(int(s.resolved()), 0);
    s.set(SvField::Caps, SvOutcome::Answered);
    s.set(SvField::Identity, SvOutcome::Answered);
    s.set(SvField::Svids, SvOutcome::Refused);
    s.set(SvField::Modes, SvOutcome::Silent);
    CHECK_EQ(int(s.resolved()), 4);
    CHECK_EQ(int(s.got()), 2);
    CHECK_EQ(int(s.refused()), 1);
    CHECK_EQ(int(s.silent()), 1);
    CHECK_EQ(int(s.blocked()), 0);
    Survey p;
    SvCtx c = kOpen; c.pd20 = true;
    p.start(c, 0);
    CHECK_EQ(int(p.planned()), 5);   // CapsExt, MfrInfo and EprList are blocked, not planned
    CHECK_EQ(int(p.blocked()), 3);
}

void test_clear_forgets_the_source_view() {
    Survey s;
    std::uint32_t now = 0;
    s.start(kOpen, now);
    walk(s, now);
    s.clear();
    for (unsigned f = 0; f < kSvFields; f++) CHECK_EQ(int(st(s, SvField(f))), int(SvState::NotAsked));
    CHECK_EQ(int(s.got()), 0);
}

// A run that is cut short (a charger test took the line, the source stopped answering) ends: nothing stays Pending.
void test_abandon_ends_the_run_and_keeps_the_answers() {
    Survey s;
    std::uint32_t now = 0;
    s.start(kOpen, now);
    CHECK_EQ(int(s.poll(now, true, true)), int(SvAct::Caps));
    s.set(SvField::Caps, SvOutcome::Answered);
    s.finish(now);
    s.abandon();
    CHECK_EQ(int(s.running()), 0);
    CHECK_EQ(int(s.in_flight()), 0);
    CHECK_EQ(int(st(s, SvField::Caps)), int(SvState::Got));
    CHECK_EQ(int(st(s, SvField::Identity)), int(SvState::NoAnswer));
    CHECK_EQ(int(st(s, SvField::Cable)), int(SvState::NoAnswer));
    CHECK_EQ(int(s.resolved()), int(s.planned()));
    CHECK_EQ(int(s.ran()), 1);
    CHECK_EQ(int(s.poll(now + 5000, true, true)), int(SvAct::None));
    // Blocked fields keep their reason.
    Survey b;
    b.start({.contract = true, .inline_sink = false, .charger_test = false, .xiaomi_chain = false, .pd20 = true, .epr_capable = true}, now);
    b.abandon();
    CHECK_EQ(int(st(b, SvField::CapsExt)), int(SvState::Blocked));
    CHECK_EQ(int(why(b, SvField::CapsExt)), int(SvWhy::Pd20));
}

// What the finished Reread run says about one Wire field: answered, refused (Not_Supported / Reject / NAK) or silent.
void test_wire_outcome_maps_the_reread_masks() {
    RereadStatus rs;
    rs.ok_mask = kRereadIdentity | kRereadSvids;
    rs.skipped_mask = kRereadModes | kRereadCapsExt | kRereadMfrInfo;
    rs.notsup_mask = kRereadCapsExt;
    CHECK_EQ(int(wire_outcome(rs, SvField::Identity)), int(SvOutcome::Answered));
    CHECK_EQ(int(wire_outcome(rs, SvField::Svids)), int(SvOutcome::Answered));
    CHECK_EQ(int(wire_outcome(rs, SvField::Modes)), int(SvOutcome::Silent));
    CHECK_EQ(int(wire_outcome(rs, SvField::CapsExt)), int(SvOutcome::Refused));
    CHECK_EQ(int(wire_outcome(rs, SvField::MfrInfo)), int(SvOutcome::Silent));
    // A step that never ran (aborted run) is silence, not a refusal.
    CHECK_EQ(int(wire_outcome(RereadStatus{}, SvField::Identity)), int(SvOutcome::Silent));
    // Only the five Wire fields have a step.
    CHECK_EQ(int(wire_outcome(rs, SvField::Cable)), int(SvOutcome::Silent));
}

}  // namespace

int main() {
    test_nothing_active_before_refresh();
    test_plan_order();
    test_wire_mask_is_the_plan_order_of_the_source_steps();
    test_blocked_inline_sends_nothing();
    test_blocked_charger_test_and_xiaomi_chain();
    test_pd20_skips_the_pd3_steps();
    test_epr_list_needs_the_epr_bit();
    test_status_mapping();
    test_finish_turns_unanswered_fields_of_the_action_into_no_answer();
    test_one_action_in_flight_and_paced();
    test_never_sends_while_the_sink_is_not_ready_or_the_line_busy();
    test_a_start_without_contract_asks_nothing();
    test_the_whole_survey_is_bounded();
    test_counts_and_progress();
    test_clear_forgets_the_source_view();
    test_abandon_ends_the_run_and_keeps_the_answers();
    test_wire_outcome_maps_the_reread_masks();
    return test_report("test_pd_survey");
}
