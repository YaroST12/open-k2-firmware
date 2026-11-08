// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_ccmode.hpp"
#include <cstdio>
using namespace k2::pd;
static int g_n = 0, g_bad = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    // plain boot: never any Rd unless an explicit active operation runs, and then only the active line
    for (std::uint8_t cc = 0; cc <= 2; cc++) CHECK(rd_lines(0, false, false, cc) == 0);
    CHECK(rd_lines(0, true, false, 0) == 0);          // no line yet: nothing to pull down
    CHECK(rd_lines(0, true, false, 1) == 1 && rd_lines(0, true, false, 2) == 2);
    CHECK(rd_lines(0, true, true, 1) == 0);           // a sink downstream: never
    // boot modes: one Rd pinned, off while a sink is downstream. Never both lines: Rd/Rd is the Debug Accessory signature and
    // latches a source's Rp off (OnePlus 120W brick, 2026-11-06), so a pinned 3 (nothing sets it any more) collapses to one line.
    CHECK(rd_lines(2, false, false, 0) == 2 && rd_lines(1, false, false, 1) == 1 && rd_lines(3, false, false, 2) == 2);
    CHECK(rd_lines(3, false, false, 0) == 2 && rd_lines(3, false, false, 1) == 1);
    CHECK(rd_lines(3, true, false, 1) == 1);
    CHECK(rd_lines(3, false, true, 1) == 0);
    for (std::uint8_t pin = 1; pin <= 3; pin++)
        for (std::uint8_t cc = 0; cc <= 2; cc++)
            for (int op = 0; op <= 1; op++) CHECK(rd_lines(pin, op != 0, false, cc) != 3);
    // a run start on a key mode's pin adds no second Rd (the start presents both lines while no CC is known, the pin is already
    // one): the pair collapses onto the line the pin follows
    CHECK(key_rd_lines(3, 2, 0) == 2 && key_rd_lines(3, 1, 0) == 1 && key_rd_lines(3, 3, 0) == 2);
    CHECK(key_rd_lines(3, 2, 1) == 1 && key_rd_lines(3, 1, 2) == 2);
    CHECK(key_rd_lines(3, 0, 0) == 3 && key_rd_lines(3, 0, 1) == 3);   // plain boot: the run start's own rule, untouched
    CHECK(key_rd_lines(1, 2, 0) == 1 && key_rd_lines(2, 1, 0) == 2 && key_rd_lines(0, 2, 0) == 0);
    // a single pinned Rd follows the active line: PLUS pinned CC2, the charger turned out to sit on CC1 -> Rd on CC1 (the
    // PLUS key's own Rd on CC2 is hardware, not ours); a charger on the pinned line changes nothing
    CHECK(rd_lines(2, false, false, 1) == 1 && rd_lines(1, false, false, 2) == 2);
    CHECK(rd_lines(2, false, false, 2) == 2 && rd_lines(1, false, false, 1) == 1);
    CHECK(rd_lines(2, false, true, 1) == 0);          // still never with a sink downstream
    // run start: Rd on the Rp line, both lines when none yet, never inline
    CHECK(start_rd_lines(false, 1) == 1 && start_rd_lines(false, 2) == 2 && start_rd_lines(false, 0) == 3);
    CHECK(start_rd_lines(true, 0) == 0 && start_rd_lines(true, 2) == 0);
    CHECK(start_needs_rd(false, -1) && start_needs_rd(false, 0) && start_needs_rd(false, 4499));
    CHECK(!start_needs_rd(false, 4500) && !start_needs_rd(true, 0));
    // the START e-marker scan (VCONN + SOP' from us) only on a dead line: VBUS below vSafe0V and no Rp on the wire
    CHECK(start_scan_wanted(0, false) && start_scan_wanted(799, false) && start_scan_wanted(-1, false));
    CHECK(!start_scan_wanted(800, false) && !start_scan_wanted(5000, false));
    CHECK(!start_scan_wanted(0, true) && !start_scan_wanted(100, true));   // an awake source hears our SOP' on its CC
    {   // the START wait services the PD engine at the main loop's pace (Baseus 140W, 2026-11-04: a 56 ms cadence missed
        // its ~30 ms SenderResponseTimer and the source sent a Hard Reset); the VBUS read is paced separately
        CHECK(kStartWaitStepMs <= 2);
        CHECK(!start_wait_slow_due(1000, 1000) && !start_wait_slow_due(1000 + kStartWaitSlowMs - 1, 1000) && start_wait_slow_due(1000 + kStartWaitSlowMs, 1000));
        CHECK(!start_wait_vbus_due(1000, 1000, 0) && !start_wait_vbus_due(1000 + kStartWaitSlowMs - 1, 1000, 0));
        CHECK(start_wait_vbus_due(1000 + kStartWaitSlowMs, 1000, 0) && start_wait_vbus_due(1000 + 500, 1000, 4499));
        CHECK(!start_wait_vbus_due(1000 + 500, 1000, 4500) && !start_wait_vbus_due(1000 + 500, 1000, 20000));   // up: no more reads in the way
        CHECK(start_wait_vbus_due(10, 0xFFFFFFF0u, -1));   // the millisecond clock wraps
    }
    {   // an attached charger that stays silent gets the longer wait
        VbusWait w; w.begin(1000);
        CHECK(w.update(1000 + kStartVbusWaitMs, 0, true) == VbusWait::State::Waiting);
        CHECK(w.update(1000 + kStartVbusAttachedWaitMs - 1, 0, true) == VbusWait::State::Waiting);
        CHECK(w.update(1000 + kStartVbusAttachedWaitMs, 0, true) == VbusWait::State::TimedOut);
    }
    {   // VBUS wait: ready on arrival, timeout after kStartVbusWaitMs, sticky
        VbusWait w;
        CHECK(w.state() == VbusWait::State::Idle && w.update(0, 0) == VbusWait::State::Idle);
        w.begin(5000);
        CHECK(w.update(5100, 0) == VbusWait::State::Waiting);
        CHECK(w.update(5400, 5000, false, true) == VbusWait::State::Ready);   // VBUS up and the caps are in
        CHECK(w.update(7000, 0) == VbusWait::State::Ready);
        w.begin(0xFFFFFF00u);   // wraps
        CHECK(w.update(0xFFFFFF00u + kStartVbusWaitMs - 1, 0) == VbusWait::State::Waiting);
        CHECK(w.update(0xFFFFFF00u + kStartVbusWaitMs, 0) == VbusWait::State::TimedOut);
        CHECK(w.update(0xFFFFFF00u + kStartVbusWaitMs + 1, 5000) == VbusWait::State::TimedOut);
    }
    {   // VBUS up is not enough: the charger's Source_Capabilities come after it (C135: ~670 ms after Rd)
        VbusWait w; w.begin(1000);
        CHECK(w.update(1200, 5000) == VbusWait::State::Waiting);                 // up, no caps yet
        CHECK(w.update(1200 + kStartCapsWaitMs - 1, 5000) == VbusWait::State::Waiting);
        CHECK(w.update(1700, 5000, true, true) == VbusWait::State::Ready);       // caps arrived: go
        VbusWait q; q.begin(1000);
        CHECK(q.update(1200, 5000) == VbusWait::State::Waiting);
        CHECK(q.update(1200 + kStartCapsWaitMs, 5000) == VbusWait::State::Ready);  // a non-PD source: 2 s of silence after VBUS up
        VbusWait r; r.begin(1000);
        CHECK(r.update(1200, 0) == VbusWait::State::Waiting);
        CHECK(r.update(3500, 5000) == VbusWait::State::Waiting);                 // the 2 s count from VBUS up, not from begin
        CHECK(r.update(3500 + kStartCapsWaitMs - 1, 5000) == VbusWait::State::Waiting);
        CHECK(r.update(3500 + kStartCapsWaitMs, 5000) == VbusWait::State::Ready);
        VbusWait c; c.begin(1000);
        CHECK(c.update(1100, 0, true, true) == VbusWait::State::Waiting);        // caps with VBUS still down: keep waiting
        CHECK(c.update(1200, 5000, true, true) == VbusWait::State::Ready);
        VbusWait t; t.begin(1000);
        CHECK(t.update(5900, 0) == VbusWait::State::Waiting);
        CHECK(t.update(5950, 5000) == VbusWait::State::Waiting);                 // up just before the old timeout: not TimedOut
        CHECK(t.update(6100, 5000) == VbusWait::State::Waiting);
        CHECK(t.update(5950 + kStartCapsWaitMs, 5000) == VbusWait::State::Ready);
    }
    {   // the start's own Request (PDO 1) is still in flight when the caps are in: wait for it, so nobody else touches the sink
        // meanwhile (hardware 2026-11-04, UGREEN X865: the wait ended 5 ms after TXSENT, the preview refused, rd_for_run(false)
        // released the sink and a second Request raced the pending Accept)
        VbusWait w; w.begin(1000);
        CHECK(w.update(1300, 5000, true, true, true) == VbusWait::State::Waiting);   // caps in, Request pending
        CHECK(w.update(1300 + kStartContractWaitMs - 1, 5000, true, true, true) == VbusWait::State::Waiting);
        CHECK(w.update(1300 + 150, 5000, true, true, false) == VbusWait::State::Ready);   // contract settled: go
        VbusWait b; b.begin(1000);
        CHECK(b.update(1300, 5000, true, true, true) == VbusWait::State::Waiting);
        CHECK(b.update(1300 + kStartContractWaitMs, 5000, true, true, true) == VbusWait::State::Ready);   // bounded: never hangs the START
        VbusWait q; q.begin(1000);
        CHECK(q.update(1300, 5000, true, true, false) == VbusWait::State::Ready);   // no Request pending: as before
        VbusWait n; n.begin(1000);
        CHECK(n.update(1200, 5000, false, false, true) == VbusWait::State::Waiting);   // no caps yet: the 2 s rule alone decides
        CHECK(n.update(1200 + kStartCapsWaitMs, 5000, false, false, true) == VbusWait::State::Ready);
    }
    {   // the start acts as a sink: ask for PDO 1 once, on the attached line, when the caps arrive
        CHECK(start_request_wanted(true, false, 2, true, false));
        CHECK(!start_request_wanted(false, false, 2, true, false));   // no run start: passive
        CHECK(!start_request_wanted(true, true, 2, true, false));     // a sink inline: never
        CHECK(!start_request_wanted(true, false, 0, true, false));    // no attached line yet
        CHECK(!start_request_wanted(true, false, 2, false, false));   // the sink engine is busy already
        CHECK(!start_request_wanted(true, false, 2, true, true));     // asked once
        // why not, for the black box: inline 1, no CC 2, sink busy 4, already requested 8
        CHECK(start_request_blockers(false, 2, true, false) == 0);
        CHECK(start_request_blockers(true, 2, true, false) == 1);
        CHECK(start_request_blockers(false, 0, true, false) == 2);
        CHECK(start_request_blockers(false, 2, false, false) == 4);
        CHECK(start_request_blockers(false, 2, true, false, true) == 16);   // legacy-only: PD-silent, no Request
        CHECK(!start_request_wanted(true, false, 2, true, false, true));
        CHECK(start_request_blockers(false, 2, true, true) == 8);
        CHECK(start_request_blockers(true, 0, false, true) == 15);
    }
    {   // Source_Capabilities that went by with Auto-CRC on were GoodCRC'd by the chip, which starts the source's
        // SenderResponseTimer (24-30 ms). Only a Request stops it; silence ends in a Hard Reset (OnePlus 120W, 2026-11-06: the
        // pd-reread's first caps were acked, no Request, Hard Reset 27 ms later). Whatever turned Auto-CRC on, the K2
        // Requests PDO 1 itself, unless the answer is already somebody's.
        // blockers: not acked 1, inline 2, no CC 4, legacy-only 8, sink busy 16, prepare running 32, requested in this frame 64
        CHECK(acked_caps_blockers(true, false, 2, false, true, false, false) == 0);
        CHECK(acked_caps_need_request(true, false, 2, false, true, false, false));
        CHECK(acked_caps_blockers(false, false, 2, false, true, false, false) == 1);   // not acked: the source heard nothing, no timer
        CHECK(acked_caps_blockers(true, true, 2, false, true, false, false) == 2);     // a real sink downstream owns the answer
        CHECK(acked_caps_blockers(true, false, 0, false, true, false, false) == 4);
        CHECK(acked_caps_blockers(true, false, 2, true, true, false, false) == 8);     // legacy-only: PD-silent, never a Request
        CHECK(acked_caps_blockers(true, false, 2, false, false, false, false) == 16);  // a negotiation is running already
        CHECK(acked_caps_blockers(true, false, 2, false, true, true, false) == 32);    // the prepare re-announces, then Requests
        CHECK(acked_caps_blockers(true, false, 2, false, true, false, true) == 64);    // the start / resume branch just asked
        CHECK(!acked_caps_need_request(true, false, 2, false, true, false, true));
        CHECK(acked_caps_blockers(true, true, 0, true, false, true, true) == 126);
    }
    {   // self-hold: a pinned-Rd K2 (key mode or inherited pin) takes a plain 5 V contract as soon as caps arrive, so the
        // source's caps retries are GoodCRC'd and it never gives up the attach (OnePlus 120W, 2026-11-06). Never plain boot
        // (HID-powered), never inline, never in the PD-silent legacy session.
        CHECK(self_hold_wanted(1, false, false) && self_hold_wanted(2, false, false));
        CHECK(!self_hold_wanted(0, false, false));    // plain boot: unchanged
        CHECK(!self_hold_wanted(2, true, false));     // a sink downstream owns the answer
        CHECK(!self_hold_wanted(2, false, true));     // legacy-only: PD-silent
        CHECK(!self_hold_wanted(3, false, false));    // a stale pair is no pin (never both lines)
        CHECK(kSelfHoldMa >= 500 && kSelfHoldMa <= 1000);   // modest operating current, the RDO max stays the PDO's
    }
    std::printf("test_pd_ccmode: %d checks, %d failed\n", g_n, g_bad);
    return g_bad ? 1 : 0;
}
