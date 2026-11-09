// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the Connection page's Refresh decisions (k2/core/rescan_bridge.hpp): what a rescan does for each kind of K2 and
// source (inline stays passive, a live contract asks for the caps instead of cycling CC, a silent source gets the cycle, bridged
// by the marker when the K2 lives on the charger's VBUS), the Get_Source_Cap flow with its two deadlines, and the comeback
// routing (a vouched Rescan lands on the page, a released one does not, neither is a charger-test resume).
#include <cstdint>

#include "k2/core/rd_intent.hpp"
#include "k2/core/rescan_bridge.hpp"
#include "test_util.h"

using namespace k2::rescanbridge;
using k2::rdintent::IntentKind;
using k2::rdintent::Verdict;

namespace {

Facts live_pd(std::uint8_t pin = 0, bool host = true) {
    Facts f;
    f.contract_live = true;
    f.pin = pin;
    f.host = host;
    return f;
}

void test_the_decision_table() {
    // inline: a real sink is attached, the rescan stays passive whatever else is true
    Facts f = live_pd(2, false);
    f.inline_mode = true;
    CHECK_EQ(int(decide(f)), int(Way::Passive));
    f = {};
    f.legacy_only = true;
    f.pin = 2;
    CHECK_EQ(int(decide(f)), int(Way::Passive));

    // a live explicit contract: ask for the caps, HID-powered or self-powered alike (no CC cycle, no dark)
    CHECK_EQ(int(decide(live_pd(0, true))), int(Way::GetSrcCap));
    CHECK_EQ(int(decide(live_pd(2, false))), int(Way::GetSrcCap));
    CHECK_EQ(int(decide(live_pd(1, false))), int(Way::GetSrcCap));

    // EPR mode: an unsolicited Source_Capabilities there makes the sink hard reset, so no Get_Source_Cap: straight to the cycle
    f = live_pd(2, false);
    f.epr_mode = true;
    CHECK_EQ(int(decide(f)), int(Way::CycleBridged));
    f = live_pd(0, true);
    f.epr_mode = true;
    CHECK_EQ(int(decide(f)), int(Way::Cycle));

    // a negotiation in flight (Request outstanding, caps just arrived): one at a time, ask again
    f = live_pd(2, false);
    f.contract_live = false;
    f.negotiating = true;
    CHECK_EQ(int(decide(f)), int(Way::Busy));

    // no contract at all (Idle, Failed, a non-PD source): the cycle, bridged only when the K2 lives on the charger's VBUS
    f = {};
    f.pin = 2;
    CHECK_EQ(int(decide(f)), int(Way::CycleBridged));
    f.pin = 1;
    CHECK_EQ(int(decide(f)), int(Way::CycleBridged));
    f.host = true;                                   // a USB host feeds this K2: it outlives the dark
    CHECK_EQ(int(decide(f)), int(Way::Cycle));
    f = {};                                          // no pin: a HID or bench supply
    CHECK_EQ(int(decide(f)), int(Way::Cycle));
}

void test_the_fallback_way_is_decided_on_fresh_facts() {
    Facts f;
    f.pin = 2;
    CHECK_EQ(int(cycle_way(f)), int(Way::CycleBridged));
    f.host = true;
    CHECK_EQ(int(cycle_way(f)), int(Way::Cycle));
    f.host = false;
    f.pin = 0;
    CHECK_EQ(int(cycle_way(f)), int(Way::Cycle));
    CHECK_EQ(self_powered({.pin = 1}), true);
    CHECK_EQ(self_powered({.pin = 3}), false);
}

void test_a_caps_message_ends_the_flow() {
    Flow fl;
    CHECK_EQ(int(fl.start(live_pd(), 1000, 5)), int(Way::GetSrcCap));
    CHECK_EQ(int(fl.tick(1000, 5)), int(Flow::Act::Send));      // the gate is tried until it takes the message
    CHECK_EQ(int(fl.tick(1001, 5)), int(Flow::Act::Send));
    fl.sent(1002);
    CHECK_EQ(int(fl.tick(1003, 5)), int(Flow::Act::Wait));
    CHECK_EQ(int(fl.tick(1040, 6)), int(Flow::Act::Refreshed)); // a Source_Capabilities arrived
    CHECK_EQ(fl.active(), false);
    CHECK_EQ(int(fl.tick(1041, 6)), int(Flow::Act::Wait));      // done: nothing more
}

void test_a_silent_source_falls_back_after_the_answer_window() {
    Flow fl;
    fl.start(live_pd(2, false), 2000, 9);
    fl.sent(2000);
    CHECK_EQ(int(fl.tick(2000 + kAnswerMs - 1, 9)), int(Flow::Act::Wait));
    CHECK_EQ(int(fl.tick(2000 + kAnswerMs, 9)), int(Flow::Act::Fallback));
    CHECK_EQ(fl.active(), false);
    CHECK_EQ(int(fl.tick(2000 + kAnswerMs + 1, 9)), int(Flow::Act::Wait));
    // an answer on the last tick still counts
    fl.start(live_pd(2, false), 3000, 9);
    fl.sent(3000);
    CHECK_EQ(int(fl.tick(3000 + kAnswerMs, 10)), int(Flow::Act::Refreshed));
}

void test_a_busy_gate_gives_up_without_cycling() {
    // The sink never took the message (a Request outstanding for too long): nothing was sent and nothing is cycled; the
    // page says "Busy". A cycle on top of a negotiation would be the dark the flow exists to avoid.
    Flow fl;
    fl.start(live_pd(2, false), 5000, 1);
    CHECK_EQ(int(fl.tick(5000 + kQueueMs - 1, 1)), int(Flow::Act::Send));
    CHECK_EQ(int(fl.tick(5000 + kQueueMs, 1)), int(Flow::Act::GiveUp));
    CHECK_EQ(fl.active(), false);
}

void test_only_a_get_src_cap_decision_starts_a_flow() {
    Flow fl;
    Facts f = live_pd(2, false);
    f.inline_mode = true;
    CHECK_EQ(int(fl.start(f, 0, 0)), int(Way::Passive));
    CHECK_EQ(fl.active(), false);
    CHECK_EQ(int(fl.start({.pin = 2}, 0, 0)), int(Way::CycleBridged));
    CHECK_EQ(fl.active(), false);
    // a second start while one runs is refused: one negotiation in flight
    CHECK_EQ(int(fl.start(live_pd(), 0, 0)), int(Way::GetSrcCap));
    CHECK_EQ(int(fl.start(live_pd(), 1, 0)), int(Way::Busy));
    CHECK_EQ(fl.active(), true);
}

void test_the_comeback_routing() {
    // a vouched Rescan comeback lands back on Connection -> Source; a released one is an ordinary plain boot; the older kinds
    // never land there
    CHECK_EQ(lands_on_page(IntentKind::Rescan, Verdict::Vouched), true);
    CHECK_EQ(lands_on_page(IntentKind::Rescan, Verdict::Release), false);
    CHECK_EQ(lands_on_page(IntentKind::Rescan, Verdict::Wait), false);
    CHECK_EQ(lands_on_page(IntentKind::Session, Verdict::Vouched), false);
    CHECK_EQ(lands_on_page(IntentKind::Reattach, Verdict::Vouched), false);
    CHECK_EQ(lands_on_page(IntentKind::None, Verdict::Vouched), false);
    // the charger test resumes from its journal and only a marker kind that says a RUN was bridged may support it: a Rescan never
    CHECK_EQ(supports_run_resume(IntentKind::Reattach), true);
    CHECK_EQ(supports_run_resume(IntentKind::Session), true);
    CHECK_EQ(supports_run_resume(IntentKind::Rescan), false);
    CHECK_EQ(supports_run_resume(IntentKind::None), false);
}
}  // namespace

int main() {
    test_the_decision_table();
    test_the_fallback_way_is_decided_on_fresh_facts();
    test_a_caps_message_ends_the_flow();
    test_a_silent_source_falls_back_after_the_answer_window();
    test_a_busy_gate_gives_up_without_cycling();
    test_only_a_get_src_cap_decision_starts_a_flow();
    test_the_comeback_routing();
    return test_report("test_rescan_bridge");
}
