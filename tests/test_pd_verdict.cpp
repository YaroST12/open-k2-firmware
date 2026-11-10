// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the one "PD present" verdict per attach (k2::pd::pd_verdict, ROADMAP 2026-11-10 Row C):
// the input matrix (caps answered / caps unanswered / no caps with Rp only / VDMs only / before and after
// the settle window) and the pd::Link integration (the per-attach caps counter, the attach timestamp and
// the verdict the Link carries for the Connection page and the remote state).
#include "k2/pd/pd_link.hpp"
#include "k2/pd/pd_verdict.hpp"
#include <cstring>
#include "test_util.h"

using namespace k2::pd;
#define CHECK(c) CHECK_EQ(int(!!(c)), 1)

static PdVerdictIn vin(std::uint32_t now, std::uint32_t attach, bool attached = true, bool any_msg = false,
                      std::uint32_t caps = 0, bool answered = false) {
    PdVerdictIn in;
    in.now_ms = now;
    in.attach_at_ms = attach;
    in.attached = attached;
    in.any_msg = any_msg;
    in.caps_rx = caps;
    in.sink_answered = answered;
    return in;
}

static void test_not_attached_is_pending() {
    const PdVerdictOut v = pd_verdict(vin(10000, 0, false, true, 44, true));
    CHECK_EQ(int(v.verdict), int(PdVerdict::Pending));
    CHECK_EQ(int(v.why), int(PdNoneWhy::None));
}

static void test_no_verdict_before_the_settle_window() {
    // Even 44 unanswered caps stay Pending before attach+1500 ms: a normal negotiation never flashes a message.
    CHECK_EQ(int(pd_verdict(vin(1499, 0, true, true, 44, false)).verdict), int(PdVerdict::Pending));
    const PdVerdictOut edge = pd_verdict(vin(1500, 0, true, false, 0, false));
    CHECK(edge.verdict != PdVerdict::Pending);   // at 1500 ms the settle window is over
}

static void test_caps_answered_is_present() {
    const PdVerdictOut v = pd_verdict(vin(5000, 0, true, true, 1, true));
    CHECK_EQ(int(v.verdict), int(PdVerdict::Present));
    CHECK_EQ(int(v.why), int(PdNoneWhy::None));
    // and before the settle the answer still counts once the window is over
    CHECK_EQ(int(pd_verdict(vin(1600, 0, true, true, 1, true)).verdict), int(PdVerdict::Present));
}

static void test_caps_unanswered_is_none_offered() {
    // 44 repeated Source_Capabilities with no GoodCRC and no Request (a replug shouting into the void).
    const PdVerdictOut v = pd_verdict(vin(60000, 0, true, true, 44, false));
    CHECK_EQ(int(v.verdict), int(PdVerdict::None));
    CHECK_EQ(int(v.why), int(PdNoneWhy::SinkSilentSourceOffered));
    // the threshold: kPdUnansweredCapsMin repeated caps already read as unanswered
    const PdVerdictOut thr = pd_verdict(vin(60000, 0, true, true, kPdUnansweredCapsMin, false));
    CHECK_EQ(int(thr.verdict), int(PdVerdict::None));
    CHECK_EQ(int(thr.why), int(PdNoneWhy::SinkSilentSourceOffered));
    // one or two caps are an announcement, not a shout: with any other message decoded it is Present
    CHECK_EQ(int(pd_verdict(vin(60000, 0, true, true, 1, false)).verdict), int(PdVerdict::Present));
    CHECK_EQ(int(pd_verdict(vin(60000, 0, true, true, 2, false)).verdict), int(PdVerdict::Present));
}

static void test_no_caps_is_none_never_offered() {
    // A plain Type-C source (Rp only): no Source_Capabilities at all, nothing decoded.
    const PdVerdictOut v = pd_verdict(vin(9000, 0, true, false, 0, false));
    CHECK_EQ(int(v.verdict), int(PdVerdict::None));
    CHECK_EQ(int(v.why), int(PdNoneWhy::SourceNeverOffered));
}

static void test_vdms_only_are_present() {
    // Vendor messages without any caps/answer: PD is talking.
    const PdVerdictOut v = pd_verdict(vin(9000, 0, true, true, 0, false));
    CHECK_EQ(int(v.verdict), int(PdVerdict::Present));
}

static void test_tokens_fit_the_binlog() {
    // %s in the binlog is cut at 12 bytes: every token must fit.
    CHECK(std::strlen(pd_verdict_token(PdVerdict::Pending, PdNoneWhy::None)) <= 12);
    CHECK(std::strlen(pd_verdict_token(PdVerdict::Present, PdNoneWhy::None)) <= 12);
    CHECK(std::strlen(pd_verdict_token(PdVerdict::None, PdNoneWhy::SinkSilentSourceOffered)) <= 12);
    CHECK(std::strlen(pd_verdict_token(PdVerdict::None, PdNoneWhy::SourceNeverOffered)) <= 12);
    CHECK_EQ(std::strcmp(pd_verdict_token(PdVerdict::None, PdNoneWhy::SinkSilentSourceOffered), "none-offered"), 0);
    CHECK_EQ(std::strcmp(pd_verdict_token(PdVerdict::None, PdNoneWhy::SourceNeverOffered), "none-never"), 0);
}

// ---- pd::Link integration: the verdict rides the link state the page and the remote read -------------------

static LinkInput lin(std::uint32_t t, std::int32_t vbus, std::uint8_t cc = 1, std::uint32_t rx = 0) {
    LinkInput in;
    in.now_ms = t;
    in.vbus_known = true;
    in.vbus_mv = vbus;
    in.cc = cc;
    in.caps_rx = rx;
    return in;
}

static void test_link_verdict_matrix() {
    Link l;
    // Boot: attached at 0 with 5 caps already decoded.
    l.update(lin(0, 5000, 1, 5));
    CHECK_EQ(l.attached(), 1);
    CHECK_EQ(l.attach_at(), 0u);
    CHECK_EQ(l.caps_rx_this_attach(), 5u);
    // Before the settle window: Pending even with caps flowing.
    l.update(lin(1000, 5000, 1, 8));
    CHECK_EQ(l.caps_rx_this_attach(), 8u);
    CHECK_EQ(int(l.pd_verdict()), int(PdVerdict::Pending));
    // After the settle with the sink having answered: Present.
    LinkInput a = lin(2000, 5000, 1, 9);
    a.any_pd_msg = true;
    a.sink_answered = true;
    l.update(a);
    CHECK_EQ(int(l.pd_verdict()), int(PdVerdict::Present));
    CHECK_EQ(int(l.pd_none_why()), int(PdNoneWhy::None));
    // A decoded message with fewer than kPdUnansweredCapsMin caps and no answer is still PD talking.
    Link l2;
    l2.update(lin(0, 5000, 1, 0));
    LinkInput b = lin(2000, 5000, 1, 2);
    b.any_pd_msg = true;
    l2.update(b);
    CHECK_EQ(int(l2.pd_verdict()), int(PdVerdict::Present));
}

static void test_link_verdict_none_after_reattach() {
    Link l;
    l.update(lin(0, 5000, 1, 3));
    // Detach: VBUS low for more than 300 ms.
    l.update(lin(1000, 0, 0, 3));
    l.update(lin(1401, 0, 0, 3));
    CHECK_EQ(l.attached(), 0);
    // Re-attach at 2000: the attach timestamp and the caps counter restart there.
    l.update(lin(2000, 5000, 1, 3));
    CHECK_EQ(l.attached(), 1);
    CHECK_EQ(l.attach_at(), 2000u);
    CHECK_EQ(l.caps_rx_this_attach(), 0u);
    // The source re-sends its caps four times; the sink never answers.
    for (std::uint32_t t = 2100; t <= 2400; t += 100) l.update(lin(t, 5000, 1, 3 + (t - 2000) / 100 + 1));
    l.update(lin(3400, 5000, 1, 7));   // 4 caps this attach, still before... settle is attach+1500 = 3500
    CHECK_EQ(l.caps_rx_this_attach(), 4u);
    CHECK_EQ(int(l.pd_verdict()), int(PdVerdict::Pending));
    LinkInput u = lin(3501, 5000, 1, 7);
    u.any_pd_msg = true;   // the caps themselves are PD messages
    l.update(u);
    CHECK_EQ(int(l.pd_verdict()), int(PdVerdict::None));
    CHECK_EQ(int(l.pd_none_why()), int(PdNoneWhy::SinkSilentSourceOffered));
}

static void test_link_forced_attach_is_present() {
    Link l;
    l.force_attached(100);
    l.update(lin(200, 0, 0, 0));
    CHECK_EQ(int(l.pd_verdict()), int(PdVerdict::Present));
}

int main() {
    test_not_attached_is_pending();
    test_no_verdict_before_the_settle_window();
    test_caps_answered_is_present();
    test_caps_unanswered_is_none_offered();
    test_no_caps_is_none_never_offered();
    test_vdms_only_are_present();
    test_tokens_fit_the_binlog();
    test_link_verdict_matrix();
    test_link_verdict_none_after_reattach();
    test_link_forced_attach_is_present();
    return test_report("test_pd_verdict");
}
