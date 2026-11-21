// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the Charger tab's automatic offers read (k2/pd/pd_offers.hpp): the order of the actions, what is never done (no
// Attach with a contract, no Release of what the K2 did not take, no step beyond Attach / Epr / Release), the refusals, the
// once-per-presence latch and the bound.
#include "k2/pd/pd_offers.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

constexpr OrCtx kCharger{.present = true, .inline_sink = false, .busy = false, .contract = false, .epr_capable = false};

// Drives one read to its end with every step answered as given; returns the actions in order.
struct Run { OrAct acts[6]; int n = 0; };
Run run(OffersRead &o, std::uint32_t &now, bool caps, bool epr_cap, bool owns, SvOutcome epr = SvOutcome::Answered) {
    Run r;
    for (int guard = 0; guard < 50 && o.running(); guard++, now += 10) {
        const OrAct a = o.poll(now);
        if (a == OrAct::None) continue;
        r.acts[r.n++] = a;
        switch (a) {
        case OrAct::Attach: o.attached(caps, epr_cap, owns); break;
        case OrAct::Epr: o.epr_done(epr); break;
        case OrAct::Release: o.released(); break;
        default: break;
        }
    }
    return r;
}

void test_no_contract_attaches_reads_epr_and_gives_the_port_back() {
    OffersRead o; std::uint32_t now = 1000;
    CHECK_EQ(o.due(now), true);
    CHECK_EQ(o.start(kCharger, now), true);
    CHECK_EQ(int(o.step()), int(OrStep::Attach));
    const Run r = run(o, now, true, true, true);
    CHECK_EQ(r.n, 3);
    CHECK_EQ(int(r.acts[0]), int(OrAct::Attach));
    CHECK_EQ(int(r.acts[1]), int(OrAct::Epr));
    CHECK_EQ(int(r.acts[2]), int(OrAct::Release));
    CHECK_EQ(int(o.step()), int(OrStep::Done));
    CHECK_EQ(int(o.epr()), int(SvOutcome::Answered));
    CHECK_EQ(o.epr_asked(), true);
}

void test_a_source_without_the_epr_bit_is_not_asked_for_epr() {
    OffersRead o; std::uint32_t now = 0;
    o.start(kCharger, now);
    const Run r = run(o, now, true, false, true);
    CHECK_EQ(r.n, 2);
    CHECK_EQ(int(r.acts[0]), int(OrAct::Attach));
    CHECK_EQ(int(r.acts[1]), int(OrAct::Release));
    CHECK_EQ(o.epr_asked(), false);
}

void test_a_failed_attach_still_gives_back_what_it_took() {
    OffersRead o; std::uint32_t now = 0;
    o.start(kCharger, now);
    const Run r = run(o, now, false, false, true);   // no caps came: Rd was presented, nothing answered
    CHECK_EQ(r.n, 2);
    CHECK_EQ(int(r.acts[1]), int(OrAct::Release));
    OffersRead p; now = 0;
    p.start(kCharger, now);
    const Run q = run(p, now, false, false, false);  // refused attach: nothing was taken
    CHECK_EQ(q.n, 1);
    CHECK_EQ(int(p.step()), int(OrStep::Done));
}

void test_a_held_contract_is_never_attached_or_released() {
    // The K2 lives on this port (or is already a sink there): read EPR on the contract, leave Rd and the contract alone.
    OrCtx c = kCharger; c.contract = true; c.epr_capable = true;
    OffersRead o; std::uint32_t now = 0;
    CHECK_EQ(o.start(c, now), true);
    const Run r = run(o, now, true, true, false);
    CHECK_EQ(r.n, 1);
    CHECK_EQ(int(r.acts[0]), int(OrAct::Epr));
    CHECK_EQ(int(o.step()), int(OrStep::Done));
    OffersRead p; c.epr_capable = false;
    CHECK_EQ(p.start(c, now), true);
    CHECK_EQ(int(p.step()), int(OrStep::Done));   // nothing to ask: done at once, nothing sent
    CHECK_EQ(int(p.poll(now)), int(OrAct::None));
}

void test_refusals_name_their_reason_and_start_nothing() {
    OffersRead o; std::uint32_t now = 0;
    OrCtx c = kCharger; c.inline_sink = true;
    CHECK_EQ(o.start(c, now), false);
    CHECK_EQ(int(o.why()), int(OrWhy::Inline));
    CHECK_EQ(int(o.step()), int(OrStep::Refused));
    CHECK_EQ(int(o.poll(now)), int(OrAct::None));
    c = kCharger; c.busy = true;
    CHECK_EQ(o.start(c, now), false);
    CHECK_EQ(int(o.why()), int(OrWhy::Busy));
    c = kCharger; c.present = false;
    CHECK_EQ(o.start(c, now), false);
    CHECK_EQ(int(o.why()), int(OrWhy::NoCharger));
    c = kCharger; c.inline_sink = c.busy = true;
    CHECK_EQ(int(o.gate(c)), int(OrWhy::Inline));   // the strongest first
    CHECK_EQ(o.due(now), true);                     // a refusal latches nothing: the read starts when the cause clears
    CHECK_EQ(o.start(kCharger, now), true);
    CHECK_EQ(int(o.why()), int(OrWhy::None));
}

void test_one_read_per_presence_even_when_it_fails_or_the_port_cycles() {
    OffersRead o; std::uint32_t now = 0;
    o.presence(true, now);
    o.start(kCharger, now);
    run(o, now, false, false, true);
    for (int i = 0; i < 6000; i++, now += 10) { o.presence(true, now); CHECK_EQ(o.due(now), false); }   // a minute: not again
    // The K2's own Rd cycle drops the attach for a moment: shorter than kOffersGoneMs keeps the presence.
    o.presence(false, now); now += kOffersGoneMs - 1; o.presence(false, now); o.presence(true, now + 1);
    now += 100000;
    CHECK_EQ(o.due(now), false);
}

void test_a_new_presence_re_arms_after_the_gap() {
    OffersRead o; std::uint32_t now = 0;
    o.presence(true, now);
    o.start(kCharger, now);
    run(o, now, true, false, true);
    const std::uint32_t t0 = 0;
    o.presence(false, now);
    now += kOffersGoneMs;
    o.presence(false, now);              // gone long enough
    o.presence(true, now + 10);
    CHECK_EQ(o.due(now + 20), false);    // two reads are never closer than the gap
    CHECK_EQ(o.due(t0 + kOffersGapMs - 1), false);
    CHECK_EQ(o.due(t0 + kOffersGapMs), true);
}

void test_refresh_re_arms_at_once() {
    OffersRead o; std::uint32_t now = 0;
    o.presence(true, now);
    o.start(kCharger, now);
    run(o, now, true, false, true);
    CHECK_EQ(o.due(now), false);
    o.rearm();
    CHECK_EQ(o.due(now), true);
}

void test_a_stuck_attach_is_aborted_at_the_bound() {
    OffersRead o; std::uint32_t now = 0;
    o.start(kCharger, now);
    CHECK_EQ(int(o.poll(now)), int(OrAct::Attach));
    CHECK_EQ(int(o.poll(now + kOffersTotalMs - 1)), int(OrAct::None));
    CHECK_EQ(int(o.poll(now + kOffersTotalMs)), int(OrAct::Abort));
    CHECK_EQ(o.running(), false);
}

void test_a_silent_epr_ask_ends_at_the_bound_and_releases() {
    OffersRead o; std::uint32_t now = 0;
    o.start(kCharger, now);
    CHECK_EQ(int(o.poll(now)), int(OrAct::Attach));
    o.attached(true, true, true);
    CHECK_EQ(int(o.poll(now + 100)), int(OrAct::Epr));
    CHECK_EQ(int(o.poll(now + kOffersTotalMs)), int(OrAct::Release));   // no answer: silence, and the port goes back
    CHECK_EQ(int(o.epr()), int(SvOutcome::Silent));
    o.released();
    CHECK_EQ(int(o.step()), int(OrStep::Done));
}

void test_refused_epr_is_kept_apart_from_silence() {
    OffersRead o; std::uint32_t now = 0;
    o.start(kCharger, now);
    run(o, now, true, true, true, SvOutcome::Refused);
    CHECK_EQ(int(o.epr()), int(SvOutcome::Refused));
}

}  // namespace

int main() {
    test_no_contract_attaches_reads_epr_and_gives_the_port_back();
    test_a_source_without_the_epr_bit_is_not_asked_for_epr();
    test_a_failed_attach_still_gives_back_what_it_took();
    test_a_held_contract_is_never_attached_or_released();
    test_refusals_name_their_reason_and_start_nothing();
    test_one_read_per_presence_even_when_it_fails_or_the_port_cycles();
    test_a_new_presence_re_arms_after_the_gap();
    test_refresh_re_arms_at_once();
    test_a_stuck_attach_is_aborted_at_the_bound();
    test_a_silent_epr_ask_ends_at_the_bound_and_releases();
    test_refused_epr_is_kept_apart_from_silence();
    return test_report("test_pd_offers");
}
