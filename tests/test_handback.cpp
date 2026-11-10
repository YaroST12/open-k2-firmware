// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The phase-1 lab hand-back (ROADMAP "An inline latch leaves the far sink on Type-C current: hand the contract
// back with a Hard Reset", phase 1 lab only): the decision truth table (pd::handback_verdict) and the sequence's
// step ORDER (k2::handback::Seq) - the comeback record must be gone before the Hard Reset is queued (X1 in
// docs/CONNECTION_MATRIX.md).
#include "test_util.h"
#include "k2/core/handback.hpp"
#include "k2/pd/pd_passive.hpp"
#include <string>
#include <vector>

using namespace k2;

static void test_handback_verdict_truth_table() {
    using V = pd::HandbackVerdict;
    const pd::HandbackFacts ok{false, true, true, false, false};   // not busy, inline, contract owned, no far Request, not done
    CHECK_EQ(int(pd::handback_verdict(ok, true)), int(V::Allowed));
    // every missing precondition refuses with its own reason
    CHECK_EQ(int(pd::handback_verdict({true, true, true, false, false}, true)), int(V::RefusedBusy));        // a run owns the link, flags or not
    CHECK_EQ(int(pd::handback_verdict(ok, false)), int(V::NeedConfirm));                                       // --confirm required
    CHECK_EQ(int(pd::handback_verdict({false, false, true, false, false}, true)), int(V::RefusedNotInline));   // no sink behind the K2
    CHECK_EQ(int(pd::handback_verdict({false, true, false, false, false}, true)), int(V::RefusedNoContract));  // nothing to hand back
    CHECK_EQ(int(pd::handback_verdict({false, true, true, true, false}, true)), int(V::RefusedFarRequest));    // the far end has its own contract
    CHECK_EQ(int(pd::handback_verdict({false, true, true, false, true}, true)), int(V::RefusedAgain));         // one hand-back per attach
    // busy wins over everything, with or without the flag
    CHECK_EQ(int(pd::handback_verdict({true, false, false, true, true}, true)), int(V::RefusedBusy));
    CHECK_EQ(int(pd::handback_verdict({true, true, true, false, false}, false)), int(V::RefusedBusy));
    // names for the wire / the log
    CHECK_STR(pd::handback_verdict_name(V::Allowed), "allowed");
    CHECK_STR(pd::handback_verdict_name(V::RefusedNoContract), "no contract owned");
    CHECK_STR(pd::handback_verdict_name(V::RefusedFarRequest), "far sink Request seen this attach");
}

static std::vector<std::string> g_calls;
static bool g_send_ok = true;
static void drop_rd() { g_calls.push_back("drop_rd"); }
static bool send_reset() { g_calls.push_back("reset"); return g_send_ok; }

static void test_handback_sequence_order_record_before_reset() {
    g_calls.clear(); g_send_ok = true;
    handback::Seq s;
    s.arm(1000, 0);
    CHECK_EQ(int(s.step), int(handback::Step::WaitRecord));
    CHECK_EQ(s.done, true);   // armed = done for this attach
    // record still on the volume: nothing happens, and above all no reset
    CHECK_EQ(int(s.poll(1100, true, 0, 0, drop_rd, send_reset)), int(handback::Ev::None));
    CHECK_EQ(g_calls.empty(), true);
    CHECK_EQ(int(s.poll(5000, true, 0, 0, drop_rd, send_reset)), int(handback::Ev::None));
    CHECK_EQ(g_calls.empty(), true);
    // record gone: drop the Rd FIRST, then queue the one Hard Reset - in this order, after the record is gone
    CHECK_EQ(int(s.poll(6000, false, 0, 0, drop_rd, send_reset)), int(handback::Ev::RecordGone));
    CHECK_EQ(g_calls.size(), 2u);
    CHECK_EQ(g_calls[0] == "drop_rd" && g_calls[1] == "reset", true);
    CHECK_EQ(int(s.step), int(handback::Step::Watching));
    CHECK_EQ(s.reset_at, 6000u);
    // watching sends nothing more
    CHECK_EQ(int(s.poll(7000, false, 0, 0, drop_rd, send_reset)), int(handback::Ev::None));
    CHECK_EQ(g_calls.size(), 2u);
    // the far sink's Requests raise the highest-seen voltage; the result event fires 60 s after the reset
    CHECK_EQ(int(s.poll(6000 + 30000, false, 1, 9000, drop_rd, send_reset)), int(handback::Ev::None));
    CHECK_EQ(s.far_max_mv, 9000u);
    CHECK_EQ(int(s.poll(6000 + 59999, false, 2, 5000, drop_rd, send_reset)), int(handback::Ev::None));
    CHECK_EQ(s.far_max_mv, 9000u);   // the highest, not the latest (5000)
    CHECK_EQ(int(s.poll(6000 + 60000, false, 2, 5000, drop_rd, send_reset)), int(handback::Ev::Result));
    CHECK_EQ(int(s.step), int(handback::Step::Idle));
    CHECK_EQ(g_calls.size(), 2u);    // exactly one reset in the whole sequence
}

static void test_handback_sequence_record_timeout_sends_no_reset() {
    g_calls.clear(); g_send_ok = true;
    handback::Seq s;
    s.arm(0, 0);
    // the record never goes away: give up WITHOUT a reset (X1: a reset before the record is gone brings the K2
    // back with Rd and it fights the far sink)
    CHECK_EQ(int(s.poll(handback::kRecordWaitMs + 1, true, 0, 0, drop_rd, send_reset)), int(handback::Ev::RecordTimeout));
    CHECK_EQ(g_calls.empty(), true);
    CHECK_EQ(int(s.step), int(handback::Step::Idle));
}

static void test_handback_sequence_reset_refused_by_the_driver() {
    g_calls.clear(); g_send_ok = false;
    handback::Seq s;
    s.arm(0, 0);
    CHECK_EQ(int(s.poll(100, false, 0, 0, drop_rd, send_reset)), int(handback::Ev::ResetRefused));
    CHECK_EQ(g_calls.size(), 2u);    // the Rd was still dropped first
    CHECK_EQ(int(s.step), int(handback::Step::Idle));
    // no result event without a sent reset
    CHECK_EQ(int(s.poll(100 + handback::kResultMs, false, 0, 0, drop_rd, send_reset)), int(handback::Ev::None));
}

static void test_handback_result_without_any_far_request() {
    g_calls.clear(); g_send_ok = true;
    handback::Seq s;
    s.arm(0, 0);
    s.poll(10, false, 0, 0, drop_rd, send_reset);
    CHECK_EQ(int(s.poll(10 + handback::kResultMs, false, 0, 0, drop_rd, send_reset)), int(handback::Ev::Result));
    CHECK_EQ(s.far_max_mv, 0u);   // "no Request: Type-C current"
}

int main() {
    test_handback_verdict_truth_table();
    test_handback_sequence_order_record_before_reset();
    test_handback_sequence_record_timeout_sends_no_reset();
    test_handback_sequence_reset_refused_by_the_driver();
    test_handback_result_without_any_far_request();
    return test_report("test_handback");
}
