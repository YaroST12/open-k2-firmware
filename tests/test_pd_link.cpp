// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the attach/detach state machine (k2::pd::Link): debounce, both detach
// triggers (VBUS below vSafe0V, CC lost), attach, the "waiting for source" window and the
// cable (e-marker) staleness that follows CC edges only.
#include "k2/pd/pd_link.hpp"
#include <cstring>
#include "test_util.h"

using namespace k2::pd;
using k2::raguard::WaitKind;

static LinkInput in(std::uint32_t t, std::int32_t vbus, std::uint8_t cc = 1, std::uint32_t rx = 1) {
    return {t, true, vbus, cc, rx};
}

static void test_boot_attached_and_stable() {
    Link l;
    l.update(in(0, 5000));
    l.update(in(1000, 5100));
    CHECK_EQ(l.attached(), 1);
    CHECK_EQ(l.charger_stale(), 0);
}

static void test_vbus_detach_debounce() {
    Link l;
    l.update(in(0, 5000));
    l.update(in(100, 0));
    l.update(in(400, 0));           // 300 ms low: not yet "more than 300"
    CHECK_EQ(l.attached(), 1);
    l.update(in(401, 0));
    CHECK_EQ(l.attached(), 0);
    CHECK_EQ(l.charger_stale(), 1);
    CHECK_EQ(l.detached_for_s(100 + 61000), 61);   // measured from the first low sample
}

static void test_vbus_glitch_ignored() {
    Link l;
    l.update(in(0, 5000));
    l.update(in(100, 300));
    l.update(in(250, 300));
    l.update(in(300, 5000));        // back above vSafe0V: timer resets
    l.update(in(500, 300));
    l.update(in(700, 300));         // only 200 ms low again
    CHECK_EQ(l.attached(), 1);
}

static void test_cc_detach_and_attach() {
    Link l;
    l.update(in(0, 5000));
    l.update(in(100, 5000, 0));
    l.update(in(401, 5000, 0));
    CHECK_EQ(l.attached(), 0);      // CC gone with VBUS still up (hard reset excluded: 300 ms)
    l.update(in(500, 5000, 2));     // CC back: attach at once
    CHECK_EQ(l.attached(), 1);
}

static void test_attach_threshold_and_hysteresis() {
    Link l;
    l.update(in(0, 0));
    l.update(in(400, 0));
    CHECK_EQ(l.attached(), 0);
    l.update(in(500, 2000, 0));     // between vSafe0V and vSafe5V min: still detached
    CHECK_EQ(l.attached(), 0);
    l.update(in(600, 4750, 0));
    CHECK_EQ(l.attached(), 1);
}

static void test_unknown_vbus_uses_cc_only() {
    Link l;
    l.update({0, false, 0, 1, 1});
    l.update({1000, false, 0, 1, 1});
    CHECK_EQ(l.attached(), 1);
    l.update({1100, false, 0, 0, 1});
    l.update({1500, false, 0, 0, 1});
    CHECK_EQ(l.attached(), 0);
}

static void test_waiting_until_new_caps() {
    Link l;
    l.update(in(0, 5000, 1, 7));
    l.update(in(100, 0, 1, 7));
    l.update(in(500, 0, 1, 7));
    CHECK_EQ(l.attached(), 0);
    CHECK_EQ(l.waiting(), 0);
    l.update(in(2000, 5000, 1, 7));
    CHECK_EQ(l.attached(), 1);
    CHECK_EQ(l.waiting(), 1);       // attached again, caps count unchanged
    CHECK_EQ(l.charger_stale(), 1); // old data stays marked
    CHECK_EQ(l.take_new_source(), 1);
    CHECK_EQ(l.take_new_source(), 0);
    l.update(in(2100, 5000, 1, 8)); // new Source_Capabilities
    CHECK_EQ(l.waiting(), 0);
    CHECK_EQ(l.charger_stale(), 0);
}

// A source that never sends Source_Capabilities (a plain Type-C 5 V port): "waiting" must end after
// kWaitingCapsMs, so the pages can fall through to the Type-C advertisement instead of waiting forever.
static void test_waiting_clears_on_timeout() {
    Link l;
    l.update(in(0, 5000, 1, 7));
    l.update(in(100, 0, 1, 7));
    l.update(in(500, 0, 1, 7));
    CHECK_EQ(l.attached(), 0);
    l.update(in(2000, 5000, 1, 7));                 // re-attach, caps count unchanged: waiting
    CHECK_EQ(l.attached(), 1);
    CHECK_EQ(l.waiting(), 1);
    l.update(in(2000 + kWaitingCapsMs, 5000, 1, 7));      // not yet "more than" the timeout
    CHECK_EQ(l.waiting(), 1);
    l.update(in(2000 + kWaitingCapsMs + 1, 5000, 1, 7));
    CHECK_EQ(l.waiting(), 0);                       // timeout: the source is not PD, show what is known
    CHECK_EQ(l.charger_stale(), 0);
    // Caps arriving still clears it early.
    Link m;
    m.update(in(0, 5000, 1, 7));
    m.update(in(100, 0, 1, 7));
    m.update(in(500, 0, 1, 7));
    m.update(in(2000, 5000, 1, 7));
    CHECK_EQ(m.waiting(), 1);
    m.update(in(3000, 5000, 1, 8));                 // new Source_Capabilities before the timeout
    CHECK_EQ(m.waiting(), 0);
    // A detach while waiting restarts the window on the next attach.
    Link n;
    n.update(in(0, 5000, 1, 7));
    n.update(in(100, 0, 1, 7));
    n.update(in(500, 0, 1, 7));
    n.update(in(2000, 5000, 1, 7));
    n.update(in(2000 + kWaitingCapsMs + 1, 5000, 1, 7));
    CHECK_EQ(n.waiting(), 0);
    n.update(in(9000, 0, 1, 7));
    n.update(in(9400, 0, 1, 7));
    CHECK_EQ(n.attached(), 0);
    n.update(in(10000, 5000, 1, 7));
    CHECK_EQ(n.waiting(), 1);                       // fresh attach: waiting again, full window
    n.update(in(10000 + kWaitingCapsMs, 5000, 1, 7));
    CHECK_EQ(n.waiting(), 1);
    n.update(in(10000 + kWaitingCapsMs + 1, 5000, 1, 7));
    CHECK_EQ(n.waiting(), 0);
}

static void test_cable_stale_on_cc_edge_only() {
    Link l;
    l.update(in(0, 5000));
    CHECK_EQ(l.cable_scanned(), 0);
    l.emarker_scanned(1000);
    CHECK_EQ(l.cable_scanned(), 1);
    CHECK_EQ(l.cable_age_s(66000), 65);
    // VBUS loss alone: cable data stays live.
    l.update(in(2000, 0));
    l.update(in(2400, 0));
    CHECK_EQ(l.attached(), 0);
    CHECK_EQ(l.cable_stale(), 0);
    l.update(in(3000, 5000));
    CHECK_EQ(l.cable_stale(), 0);
    // CC detach: the cable may have been swapped.
    l.update(in(4000, 5000, 0));
    l.update(in(4400, 5000, 0));
    CHECK_EQ(l.cable_stale(), 1);
    l.update(in(5000, 5000, 1));
    CHECK_EQ(l.cable_stale(), 1);   // attach alone does not clear it
    l.emarker_scanned(6000);
    CHECK_EQ(l.cable_stale(), 0);
}

static LinkInput inl(std::uint32_t t, std::int32_t vbus, std::int32_t ma, std::uint32_t req = 0, bool own = false) {
    LinkInput i = in(t, vbus);
    i.current_known = true; i.current_ma = ma; i.requests = req; i.own_contract = own;
    return i;
}

static void test_inline_by_current() {
    Link l;
    l.update(inl(0, 5000, 10));
    l.update(inl(100, 5000, 50));
    CHECK_EQ(l.inline_mode(), 0);       // 50 mA is not "more than 50"
    l.update(inl(200, 5000, -51));
    CHECK_EQ(l.inline_mode(), 1);       // either direction
    l.update(inl(300, 5000, 13));
    CHECK_EQ(l.inline_mode(), 1);       // sticky: an idle laptop draws ~13 mA
}

// No USB host: the K2 runs on the measured VBUS and its own ~60-100 mA flows through the shunt (USB-A port, hardware
// 2026-11-06). Up to the allowance that is not a sink; a real sink's draw on top of it still latches.
static void test_self_supply_is_not_a_load() {
    Link l;
    LinkInput i = inl(0, 5100, 95);
    i.self_supply_ma = kSelfSupplyAllowanceMa;
    l.update(i);
    i.now_ms = 100; i.current_ma = 120; l.update(i);
    CHECK_EQ(l.inline_mode(), 0);
    i.now_ms = 200; i.current_ma = kInlineCurrentMa + kSelfSupplyAllowanceMa + 1; l.update(i);
    CHECK_EQ(l.inline_mode(), 1);
    Link h;                               // with a USB host (HID power) the 50 mA rule is unchanged
    h.update(inl(0, 5100, 10));
    h.update(inl(100, 5100, 95));
    CHECK_EQ(h.inline_mode(), 1);
}

static void test_inline_by_foreign_request() {
    Link l;
    l.update(inl(0, 5000, 0, 0));
    CHECK_EQ(l.inline_mode(), 0);
    l.update(inl(100, 5000, 0, 1));
    CHECK_EQ(l.inline_mode(), 1);
}

static void test_inline_by_vbus_without_own_contract() {
    // VBUS at a PD level that this K2 did not ask for is somebody else's contract: possibly inline until proven otherwise.
    // (Hardware 2026-11-09: a dock at 20 V feeding a laptop through the K2 is silent after the K2 boots into it: no load
    // above 50 mA, no Request, no rise. The old rule called the K2 standalone and its Refresh transmitted into the session.)
    Link l;
    l.update(inl(0, 9000, 0, 0, false));
    l.update(inl(100, 9000, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 0);            // not for a sample or two
    l.update(inl(kInlineForeignMs - 1, 9000, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 0);
    l.update(inl(kInlineForeignMs + 1, 9000, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 1);            // a debounced foreign PD-level VBUS
    // Our own trigger asked for it: a rise from 5 V is ours.
    Link o;
    o.update(inl(0, 5000, 0, 0, true));
    o.update(inl(100, 9000, 0, 0, true));
    CHECK_EQ(o.inline_mode(), 0);
    // A rise nobody on this side caused: somebody behind the K2 negotiated it.
    Link m;
    m.update(inl(0, 5000, 0, 0, false));
    m.update(inl(100, 20000, 0, 0, false));
    CHECK_EQ(m.inline_mode(), 1);
}

// Hardware 2026-11-07 (Xiaomi 20K, SCP row after FCP 5/9/12 V): the SCP engine ran its handshake at 5 V, gave up
// (abort -> reset_bus) and the charger answered the K2's own D+/D- release with a VBUS rise to 6.78 V a few ms
// later. By then fctrig was no longer Negotiating/Active (own_contract false), the rise counted as a foreign sink and
// latched inline: the whole run stopped "load attached" with IBUS 0 mA everywhere.
static void test_own_trigger_rise_after_the_trigger_ended_is_not_a_sink() {
    Link l;
    for (std::uint32_t t = 66000; t < 66600; t += 50) l.update(inl(t, 4965, 0, 0, false));    // row start, passive
    for (std::uint32_t t = 66600; t < 69600; t += 50)                                         // SCP engine negotiating
        l.update(inl(t, 4965, 0, 0, true));
    l.update(inl(69600, 4965, 0, 0, false));   // the engine aborted: no longer Negotiating or Active
    l.update(inl(69650, 6780, 0, 0, false));   // the charger's answer to the released lines
    CHECK_EQ(l.inline_mode(), 0);
    l.update(inl(70000, 6780, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 0);
}

static void test_own_trigger_hold_ends_and_a_foreign_rise_still_latches() {
    Link l;
    l.update(inl(0, 5000, 0, 0, true));
    l.update(inl(100, 5000, 0, 0, true));
    l.update(inl(100 + kOwnHoldMs - 1, 5000, 0, 0, false));   // inside the hold: still ours
    l.update(inl(100 + kOwnHoldMs + 1, 5000, 0, 0, false));   // hold over
    l.update(inl(100 + kOwnHoldMs + 100, 20000, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 1);   // somebody behind the K2 negotiated 20 V
    // A rise long after the trigger, never own in this attach: unchanged rule.
    Link m;
    m.update(inl(0, 5000, 0, 0, false));
    m.update(inl(10, 9000, 0, 0, false));
    CHECK_EQ(m.inline_mode(), 1);
}

static void test_inline_by_request_then_vbus() {
    Link l;
    l.update(inl(0, 5000, 0, 0));
    l.update(inl(100, 5000, 0, 1));     // a Request nobody here sent
    l.update(inl(200, 9000, 0, 1));
    CHECK_EQ(l.inline_mode(), 1);
}

static void test_inline_clears_when_idle() {
    Link l;
    l.update(inl(0, 5000, 2000, 1));
    CHECK_EQ(l.inline_mode(), 1);
    l.update(inl(100, 5000, 0, 1));     // load gone: 0 mA, no new Request
    l.update(inl(kInlineQuietMs - 500, 5000, 0, 1));
    CHECK_EQ(l.inline_mode(), 1);       // not yet quiet for the whole window
    l.update(inl(kInlineQuietMs + 1100, 5000, 0, 1));
    CHECK_EQ(l.inline_mode(), 0);       // idle for the whole window: restored without a detach
    CHECK_EQ(l.attached(), 1);
    l.update(inl(kInlineQuietMs + 1200, 5000, 0, 1));
    CHECK_EQ(l.inline_mode(), 0);       // the old Request count must not re-latch
}

// A flip back to "standalone" needs proof, not a missed sample: a phone that sips less than 50 mA for a few seconds (full battery,
// screen off) is still a phone. Hardware 2026-11-09: 2 s of quiet was enough to put the K2's Rd back on a hub - K2 - phone chain.
static void test_a_short_lull_is_not_proof_of_a_standalone_k2() {
    Link l;
    l.update(inl(0, 5000, 900, 1));
    CHECK_EQ(l.inline_mode(), 1);
    l.update(inl(100, 5000, 20, 1));
    l.update(inl(9000, 5000, 20, 1));   // 9 s below 50 mA
    CHECK_EQ(l.inline_mode(), 1);
    l.update(inl(9100, 5000, 900, 1));  // the phone draws again: the window restarts
    l.update(inl(9200, 5000, 20, 1));
    l.update(inl(9200 + kInlineQuietMs - 100, 5000, 20, 1));
    CHECK_EQ(l.inline_mode(), 1);
}

// The inline state outlives a VBUS collapse: the hub or the charger dropping VBUS for a moment (Rd off, a swap, a re-negotiation)
// is not the sink leaving. Before this the latch cleared on every detach and re-applied the K2's Rd, which made the next
// attach a K2 sink again: the flap loop of the 2026-11-09 capture (inline yes / no every 0.5 s, 181502..183788 ms).
static void test_inline_survives_a_vbus_collapse_and_the_reattach() {
    Link l;
    l.update(inl(0, 5000, 900, 3));
    CHECK_EQ(l.inline_mode(), 1);
    l.update(inl(100, 0, 0, 3));
    l.update(inl(500, 0, 0, 3));
    CHECK_EQ(l.attached(), 0);
    CHECK_EQ(l.inline_mode(), 1);       // detached for a moment: still possibly inline
    l.update(inl(1500, 0, 0, 3));
    CHECK_EQ(l.inline_mode(), 1);
    l.update(inl(2000, 5000, 0, 3));    // back, no load yet, the old Request count must not matter
    CHECK_EQ(l.attached(), 1);
    CHECK_EQ(l.inline_mode(), 1);
    l.update(inl(2000 + kInlineQuietMs - 500, 5000, 0, 3));   // the quiet window starts at the re-attach, not at the old load
    CHECK_EQ(l.inline_mode(), 1);
}

// Gone for real: nothing came back for kInlineHoldMs, the next attach starts from "standalone".
static void test_inline_clears_after_a_long_detach() {
    Link l;
    l.update(inl(0, 5000, 900, 3));
    l.update(inl(100, 0, 0, 3));
    l.update(inl(500, 0, 0, 3));
    CHECK_EQ(l.attached(), 0);
    l.update(inl(100 + kInlineHoldMs - 100, 0, 0, 3));
    CHECK_EQ(l.inline_mode(), 1);
    l.update(inl(100 + kInlineHoldMs + 100, 0, 0, 3));
    CHECK_EQ(l.inline_mode(), 0);
    l.update(inl(100 + kInlineHoldMs + 2000, 5000, 0, 3));
    CHECK_EQ(l.inline_mode(), 0);       // the old Request count must not re-trigger
}

static void test_inline_stays_while_active() {
    Link l;
    l.update(inl(0, 5000, 2000, 1));
    for (std::uint32_t t = 100; t < 6000; t += 100) l.update(inl(t, 5000, (t / 100) % 2 ? 0 : 80, 1));
    CHECK_EQ(l.inline_mode(), 1);       // current above 50 mA every 200 ms keeps the window open
    l.update(inl(6100, 5000, 0, 2));    // a fresh Request restarts it too
    l.update(inl(6100 + kInlineQuietMs - 1000, 5000, 0, 2));
    CHECK_EQ(l.inline_mode(), 1);
    l.update(inl(6100 + kInlineQuietMs + 100, 5000, 0, 2));
    CHECK_EQ(l.inline_mode(), 0);
}

// Dock (20 V) - K2 - laptop, captured 2026-11-09: the K2 boots into a live contract. Nothing flows that it could measure as a load
// (the laptop is full), nobody sends a Request, VBUS never rises. VBUS at 20 V that this K2 did not ask for is the evidence, and it
// holds as long as it lasts: no quiet window can clear a state the data still shows.
static void test_a_boot_into_a_live_contract_is_possibly_inline_and_stays() {
    Link l;
    for (std::uint32_t t = 0; t <= 1000; t += 100) l.update(inl(t, 20190, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 0);                         // still inside the debounce
    for (std::uint32_t t = 1100; t <= 3000; t += 100) l.update(inl(t, 20190, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 1);
    for (std::uint32_t t = 3100; t <= 60000; t += 100) l.update(inl(t, 20190, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 1);                         // 57 s of silence is not proof of a standalone K2
    for (std::uint32_t t = 60100; t <= 62000; t += 100) l.update(inl(t, 0, 0, 0, false));   // the dock is unplugged
    CHECK_EQ(l.attached(), 0);
    CHECK_EQ(l.inline_mode(), 1);                         // held across the gap
    l.update(inl(62000 + kInlineHoldMs + 100, 0, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 0);
}

// A leftover 9 V that the K2 itself set earlier in this attach (a trigger that has ended) is its own doing, however long it lasts.
static void test_a_voltage_the_k2_set_itself_is_never_foreign() {
    Link l;
    for (std::uint32_t t = 0; t < 3000; t += 100) l.update(inl(t, 5000, 0, 0, true));
    for (std::uint32_t t = 3000; t < 9000; t += 100) l.update(inl(t, 9000, 0, 0, true));      // the K2's own contract
    for (std::uint32_t t = 9000; t < 40000; t += 100) l.update(inl(t, 9000, 0, 0, false));    // the trigger ended, the charger stays
    CHECK_EQ(l.inline_mode(), 0);
    Link o;                                                // an own contract right now masks everything
    for (std::uint32_t t = 0; t < 20000; t += 100) {
        LinkInput i = inl(t, 20000, 0, 0, true);
        i.far_sink_cc = true;
        o.update(i);
    }
    CHECK_EQ(o.inline_mode(), 0);
}

// A sink's Rd on a CC line while the K2 itself presents none and has no contract: a phone or laptop on the far side.
static void test_a_sink_on_the_far_cc_is_possibly_inline() {
    Link l;
    for (std::uint32_t t = 0; t < 600; t += 100) {
        LinkInput i = inl(t, 5000, 0, 0, false);
        i.far_sink_cc = true;
        l.update(i);
    }
    CHECK_EQ(l.inline_mode(), 0);                          // a blip
    l.update(inl(700, 5000, 0, 0, false));
    for (std::uint32_t t = 800; t < 800 + kInlineForeignMs + 200; t += 100) {
        LinkInput i = inl(t, 5000, 0, 0, false);
        i.far_sink_cc = true;
        l.update(i);
    }
    CHECK_EQ(l.inline_mode(), 1);                          // 1.5 s of continuous evidence
    const std::uint32_t t1 = 800 + kInlineForeignMs + 200;
    for (std::uint32_t t = t1; t < t1 + 30000; t += 100) {
        LinkInput i = inl(t, 5000, 0, 0, false);
        i.far_sink_cc = true;
        l.update(i);
    }
    CHECK_EQ(l.inline_mode(), 1);                          // evidence present: no unlatch
    const std::uint32_t t2 = t1 + 30000;
    for (std::uint32_t t = t2; t < t2 + kInlineQuietMs - 500; t += 100) l.update(inl(t, 5000, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 1);                          // evidence gone, the quiet window runs
    l.update(inl(t2 + kInlineQuietMs + 600, 5000, 0, 0, false));
    CHECK_EQ(l.inline_mode(), 0);                          // proof: 10 s without it
}

// 5 V alone (a hub or a plain charger, nobody behind the K2) never latches.
static void test_five_volts_alone_is_a_standalone_k2() {
    Link l;
    for (std::uint32_t t = 0; t < 30000; t += 100) l.update(inl(t, 5120, 10, 0, false));
    CHECK_EQ(l.inline_mode(), 0);
}

static void test_force() {
    Link l;
    l.force_detached(1000);
    CHECK_EQ(l.attached(), 0);
    CHECK_EQ(l.detached_for_s(4000), 3);
    l.force_attached(5000);
    CHECK_EQ(l.attached(), 1);
    // A forced state holds against the real inputs until they change meaning: fake PDOs count
    // as attached, so a low VBUS sample must not detach a forced attach.
    l.update(in(5100, 0));
    l.update(in(6000, 0));
    CHECK_EQ(l.attached(), 1);
}

static void test_format_ago() {
    char b[16];
    format_ago(5, b, sizeof b);      CHECK_STR(b, "5s");
    format_ago(83, b, sizeof b);     CHECK_STR(b, "1m 23s");
    format_ago(3599, b, sizeof b);   CHECK_STR(b, "59m 59s");
    format_ago(3720, b, sizeof b);   CHECK_STR(b, "1h 02m");
    format_ago(400000, b, sizeof b); CHECK_STR(b, "99h+");
}

// The waiting classifier through the Link input: the Rp trace is fed only while passive, and VBUS up voids it.
static LinkInput win(std::uint32_t t, std::int32_t cc1, std::int32_t cc2, bool passive, std::int32_t vbus = 0) {
    LinkInput in = LinkInput{};
    in.now_ms = t; in.vbus_known = true; in.vbus_mv = vbus; in.cc = 0; in.caps_rx = 0;
    in.cc1_mv = cc1; in.cc2_mv = cc2; in.passive = passive;
    return in;
}

static void test_wait_kind_steady_rp_is_charger_waiting() {
    Link l;
    for (std::uint32_t t = 0; t <= 1000; t += 20) l.update(win(t, 0, 2990, true));   // Rp on CC2 only, steady
    CHECK_EQ(int(l.wait_kind()), int(WaitKind::ChargerWaiting));
    CHECK_EQ(l.charger_waiting(), 1);
    l.update(win(1020, 0, 2990, true, 5000));   // VBUS up: a source is already attached, no verdict
    CHECK_EQ(int(l.wait_kind()), int(WaitKind::Unknown));
    CHECK_EQ(l.charger_waiting(), 0);
}

static void test_wait_kind_toggling_rp_is_dual_role() {
    Link l;
    for (std::uint32_t t = 0; t <= 1000; t += 20) l.update(win(t, 0, (t / 100) % 2 == 0 ? 3300 : 0, true));   // toggles every 100 ms
    CHECK_EQ(int(l.wait_kind()), int(WaitKind::DualRolePort));
    CHECK_EQ(l.charger_waiting(), 0);
}

static void test_wait_kind_no_rp_and_not_passive() {
    Link l;
    for (std::uint32_t t = 0; t <= 1000; t += 20) l.update(win(t, 0, 1707, true));   // idle CC2 only
    CHECK_EQ(int(l.wait_kind()), int(WaitKind::NoSource));
    for (std::uint32_t t = 1020; t <= 2000; t += 20) l.update(win(t, 3300, 3300, false));   // the K2 holds the lines (pinned boot)
    CHECK_EQ(int(l.wait_kind()), int(WaitKind::Unknown));   // not passive: the trace was dropped
    CHECK_EQ(l.charger_waiting(), 0);
}

int main() {
    test_boot_attached_and_stable();
    test_vbus_detach_debounce();
    test_vbus_glitch_ignored();
    test_cc_detach_and_attach();
    test_attach_threshold_and_hysteresis();
    test_unknown_vbus_uses_cc_only();
    test_waiting_until_new_caps();
    test_waiting_clears_on_timeout();
    test_cable_stale_on_cc_edge_only();
    test_inline_by_current();
    test_self_supply_is_not_a_load();
    test_inline_by_foreign_request();
    test_inline_by_vbus_without_own_contract();
    test_own_trigger_rise_after_the_trigger_ended_is_not_a_sink();
    test_own_trigger_hold_ends_and_a_foreign_rise_still_latches();
    test_inline_by_request_then_vbus();
    test_inline_clears_when_idle();
    test_inline_stays_while_active();
    test_a_short_lull_is_not_proof_of_a_standalone_k2();
    test_inline_survives_a_vbus_collapse_and_the_reattach();
    test_inline_clears_after_a_long_detach();
    test_a_boot_into_a_live_contract_is_possibly_inline_and_stays();
    test_a_voltage_the_k2_set_itself_is_never_foreign();
    test_a_sink_on_the_far_cc_is_possibly_inline();
    test_five_volts_alone_is_a_standalone_k2();
    test_force();
    test_format_ago();
    test_wait_kind_steady_rp_is_charger_waiting();
    test_wait_kind_toggling_rp_is_dual_role();
    test_wait_kind_no_rp_and_not_passive();
    return test_report("test_pd_link");
}
