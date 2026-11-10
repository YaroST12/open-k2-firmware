// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_passive.hpp"
#include "test_util.h"

using namespace k2::pd;

int main() {
    // VBUS below 3.0 V: detach only after a full second.
    {
        DetachTracker t;
        CHECK_EQ(int(t.update(0, 5000, true)), 0);
        CHECK_EQ(int(t.update(100, 2900, true)), 0);
        CHECK_EQ(int(t.update(1099, 2900, true)), 0);
        CHECK_EQ(int(t.update(1100, 2900, true)), 1);
    }
    // A bounce above 3.0 V restarts the timer (the hard-reset loop bounces VBUS).
    {
        DetachTracker t;
        CHECK_EQ(int(t.update(0, 5000, true)), 0);
        CHECK_EQ(int(t.update(0, 1500, true)), 0);
        CHECK_EQ(int(t.update(900, 1500, true)), 0);
        CHECK_EQ(int(t.update(950, 5000, true)), 0);
        CHECK_EQ(int(t.update(1500, 1500, true)), 0);
        CHECK_EQ(int(t.update(2400, 1500, true)), 0);
        CHECK_EQ(int(t.update(2500, 1500, true)), 1);
    }
    // VBUS that never reached 3 V in this attach is no evidence of a detach (the power monitor reads
    // the other port; QA: a charger on the top port with VBUS 0 mV flapped attach/detach every 2 s).
    {
        DetachTracker t;
        for (std::uint32_t ms = 0; ms < 20000; ms += 100) CHECK_EQ(int(t.update(ms, 0, true)), 0);
        CHECK_EQ(int(t.vbus_seen()), 0);
        CHECK_EQ(int(t.update(20000, 0, false)), 0);   // CC loss still counts, after its 500 ms
        CHECK_EQ(int(t.update(20500, 0, false)), 1);
    }
    // Once VBUS was there, losing it is a detach; reset() forgets it for the next attach.
    {
        DetachTracker t;
        CHECK_EQ(int(t.update(0, 5000, true)), 0);
        CHECK_EQ(int(t.update(10, 0, true)), 0);
        CHECK_EQ(int(t.update(1010, 0, true)), 1);
        t.reset();
        CHECK_EQ(int(t.update(0, 0, true)), 0);
        CHECK_EQ(int(t.update(5000, 0, true)), 0);
    }
    // CC gone: 500 ms, regardless of VBUS.
    {
        DetachTracker t;
        CHECK_EQ(int(t.update(0, 5000, true)), 0);
        CHECK_EQ(int(t.update(10, 5000, false)), 0);
        CHECK_EQ(int(t.update(509, 5000, false)), 0);
        CHECK_EQ(int(t.update(510, 5000, false)), 1);
    }
    // A glitch on CC does not count; unknown VBUS is ignored.
    {
        DetachTracker t;
        CHECK_EQ(int(t.update(0, -1, false)), 0);
        CHECK_EQ(int(t.update(400, -1, true)), 0);
        CHECK_EQ(int(t.update(800, -1, false)), 0);
        CHECK_EQ(int(t.update(1200, -1, true)), 0);
        CHECK_EQ(int(t.update(100000, -1, true)), 0);
        t.reset();
        CHECK_EQ(int(t.update(100001, 0, true)), 0);
        CHECK_EQ(int(t.update(101001, 0, true)), 0);   // never saw VBUS: not a detach
    }
    // Inline: the sink-present band on the active CC line (Rd of a real sink against the charger's Rp,
    // no Rd of ours on the line): closest to 1.5 V, within 0.7 V; open Rp (~3.3 V) or 0 V is no sink.
    CHECK_EQ(int(sink_present(1500)), 1);
    CHECK_EQ(int(sink_present(800)), 1);
    CHECK_EQ(int(sink_present(2200)), 1);
    CHECK_EQ(int(sink_present(799)), 0);
    CHECK_EQ(int(sink_present(2201)), 0);
    CHECK_EQ(int(sink_present(3300)), 0);
    CHECK_EQ(int(sink_present(-1)), 1);         // unknown: never a reason to detach
    {   // the sink unplugs while the charger stays (Pixel + charger): detach after 500 ms of no sink
        DetachTracker t;
        t.sink_seen();
        CHECK_EQ(int(t.update(0, 5000, true, true)), 0);
        CHECK_EQ(int(t.update(100, 5000, true, false)), 0);
        CHECK_EQ(int(t.update(599, 5000, true, false)), 0);
        CHECK_EQ(int(t.update(600, 5000, true, false)), 1);
        CHECK_EQ(int(t.reason()), int(DetachReason::Sink));
    }
    {   // a flicker of the sink line is ridden out
        DetachTracker t;
        t.sink_seen();
        CHECK_EQ(int(t.update(0, 5000, true, false)), 0);
        CHECK_EQ(int(t.update(300, 5000, true, true)), 0);
        CHECK_EQ(int(t.update(700, 5000, true, true)), 0);
        CHECK_EQ(int(t.update(900, 5000, true, false)), 0);
        CHECK_EQ(int(t.update(1300, 5000, true, false)), 0);
        CHECK_EQ(int(t.update(1400, 5000, true, false)), 1);
    }
    {   // loop.txt: laptop Rp -> K2 -> Pixel; the line never reads in the sink band once Rd is off
        // (BC_LVL=3, open line), the Pixel charges fine. Never having seen a sink is not a detach.
        DetachTracker t;
        for (std::uint32_t ms = 0; ms < 5000; ms += 50) CHECK_EQ(int(t.update(ms, 4850, true, false)), 0);
    }
    {   // seen in band, then the line goes open for 500 ms: still a detach
        DetachTracker t;
        t.sink_seen();
        CHECK_EQ(int(t.update(0, 4850, true, true)), 0);
        CHECK_EQ(int(t.update(100, 4850, true, false)), 0);
        CHECK_EQ(int(t.update(599, 4850, true, false)), 0);
        CHECK_EQ(int(t.update(600, 4850, true, false)), 1);
        CHECK_EQ(int(t.reason()), int(DetachReason::Sink));
        t.reset();   // a new attach forgets the sink
        for (std::uint32_t ms = 1000; ms < 3000; ms += 50) CHECK_EQ(int(t.update(ms, 4850, true, false)), 0);
    }
    // Type-C-only source: Rp level from BC_LVL, shown once no PD caps came within kTypeCOnlyMs of attach.
    CHECK_EQ(int(typec_ma(0)), 0);
    CHECK_EQ(int(typec_ma(1)), 500);
    CHECK_EQ(int(typec_ma(2)), 1500);
    CHECK_EQ(int(typec_ma(3)), 3000);
    CHECK_EQ(int(typec_only_ma(kTypeCOnlyMs - 1, false, 3)), 0);   // still waiting for caps
    CHECK_EQ(int(typec_only_ma(kTypeCOnlyMs, false, 3)), 3000);
    CHECK_EQ(int(typec_only_ma(5000, false, 2)), 1500);
    CHECK_EQ(int(typec_only_ma(5000, true, 3)), 0);                 // PD caps arrived: a PD source
    CHECK_EQ(int(typec_only_ma(5000, false, 0)), 0);                // no Rp seen: not a source
    CHECK_EQ(int(typec_only_ma(kTypeCOnlyMs - 1, false, 3)), 0);
    CHECK_EQ(int(typec_only_ma(kTypeCOnlyMs, false, 3, true, false)), 0);    // inline: a sink holds the contract, caps were missed
    CHECK_EQ(int(typec_only_ma(5000, false, 3, false, true)), 0);            // any PD traffic this session: a PD source
    CHECK_EQ(int(typec_only_ma(2999, false, 3, false, false)), 0);           // 3 s of silence first
    CHECK_EQ(int(typec_only_ma(3000, false, 3, false, false)), 3000);
    // One trigger gate for the wizard and the remote: a live sink on the far side means the source
    // already has a contract with it, and a Request from the K2 would reset it.
    CHECK_EQ(int(can_trigger({false, false, false})), int(TriggerRefusal::None));
    CHECK_EQ(int(can_trigger({true, false, false})), int(TriggerRefusal::Inline));
    CHECK_EQ(int(can_trigger({false, true, false})), int(TriggerRefusal::SinkRd));
    CHECK_EQ(int(can_trigger({false, false, true})), int(TriggerRefusal::ForeignRequest));
    CHECK_EQ(int(can_trigger({true, true, true})), int(TriggerRefusal::Inline));
    CHECK_STR(trigger_refusal_name(TriggerRefusal::ForeignRequest), "request from another sink");
    // The lab-only inline Hard Reset (`k2_remote.py pd reset --inline --confirm`, ROADMAP 2026-11-10
    // connection-matrix P4): today a Hard Reset is refused while a live sink is attached, because it cuts
    // that sink's supply; the lab override sends exactly one in that state, to diagnose the far sink's
    // renegotiation. Same lab gating as --over-load: refused while a charger test runs, and it needs BOTH
    // flags (--inline alone is a NeedConfirm, not an override; --confirm alone changes nothing inline).
    CHECK_EQ(int(lab_reset_verdict({false, false, false}, false, false)), int(LabResetVerdict::Allowed));   // plain reset as today
    CHECK_EQ(int(lab_reset_verdict({false, true, false}, false, false)), int(LabResetVerdict::RefusedInline)); // refusal, no flags: as today
    CHECK_EQ(int(lab_reset_verdict({true, false, false}, false, false)), int(LabResetVerdict::RefusedInline)); // inline, no flags: as today
    CHECK_EQ(int(lab_reset_verdict({true, false, false}, false, true)), int(LabResetVerdict::RefusedInline));  // confirm alone is no override
    CHECK_EQ(int(lab_reset_verdict({true, false, false}, true, false)), int(LabResetVerdict::NeedConfirm));    // --inline without --confirm
    CHECK_EQ(int(lab_reset_verdict({true, false, false}, true, true)), int(LabResetVerdict::Allowed));         // --inline --confirm while inline: one Hard Reset
    CHECK_EQ(int(lab_reset_verdict({false, false, false}, true, true)), int(LabResetVerdict::Allowed));        // off-inline the flags are harmless
    CHECK_EQ(int(lab_reset_verdict({false, false, true}, false, false)), int(LabResetVerdict::RefusedBusy));    // a charger test runs: refused
    CHECK_EQ(int(lab_reset_verdict({true, false, true}, true, true)), int(LabResetVerdict::RefusedBusy));        // busy wins over the lab flags
    {   // reasons
        DetachTracker t;
        t.update(0, 5000, true);
        t.update(10, 5000, false);
        CHECK_EQ(int(t.update(510, 5000, false)), 1);
        CHECK_EQ(int(t.reason()), int(DetachReason::Cc));
        DetachTracker v;
        v.update(0, 5000, true);
        v.update(10, 0, true);
        CHECK_EQ(int(v.update(1010, 0, true)), 1);
        CHECK_EQ(int(v.reason()), int(DetachReason::Vbus));
    }
    // After a detach Rd stays off for a while so the charger sees the line open and restarts; the
    // scan (which closes Rd again) waits for it.
    {
        RdHold h;
        CHECK_EQ(int(h.held(0)), 0);
        h.start(1000);
        CHECK_EQ(int(h.held(1000)), 1);
        CHECK_EQ(int(h.held(1000 + kRdHoldMs - 1)), 1);
        CHECK_EQ(int(h.held(1000 + kRdHoldMs)), 0);
        h.start(5000);
        h.cancel();
        CHECK_EQ(int(h.held(5001)), 0);
    }
    // The end of a hold is reported once, so Rd goes back on exactly then; a cancelled or never started
    // hold reports nothing.
    {
        RdHold h;
        CHECK_EQ(int(h.take_expiry(0)), 0);
        h.start(1000);
        CHECK_EQ(int(h.take_expiry(1000 + kRdHoldMs - 1)), 0);
        CHECK_EQ(int(h.take_expiry(1000 + kRdHoldMs)), 1);
        CHECK_EQ(int(h.take_expiry(1000 + kRdHoldMs + 20)), 0);
        h.start(5000);
        h.cancel();
        CHECK_EQ(int(h.take_expiry(9000)), 0);
    }
    // Auto-CRC: off by default, on while busy, off a linger after the last operation.
    {
        AutoCrcGate g;
        CHECK_EQ(int(g.update(0, false)), 0);
        CHECK_EQ(int(g.update(10, true)), 1);
        CHECK_EQ(int(g.update(500, true)), 1);
        CHECK_EQ(int(g.update(500 + kCrcLingerMs - 1, false)), 1);
        CHECK_EQ(int(g.update(500 + kCrcLingerMs, false)), 0);
        CHECK_EQ(int(g.update(2000, true)), 1);
    }
    // Caps sessions: a Request needs caps that arrived in the current attach.
    {
        CapsSession c;
        CHECK_EQ(int(c.fresh()), 0);
        c.attach();
        CHECK_EQ(int(c.fresh()), 0);          // attached, nothing offered yet
        const std::uint32_t s1 = c.id();
        c.caps_rx();
        CHECK_EQ(int(c.fresh()), 1);
        c.detach();                           // the list of charger A must not serve the next attach
        CHECK_EQ(int(c.fresh()), 0);
        CHECK_EQ(int(c.id() != s1), 1);
        c.attach();
        CHECK_EQ(int(c.fresh()), 0);          // re-scan after the detach, no fresh caps yet
        c.caps_rx();
        CHECK_EQ(int(c.fresh()), 1);
        const std::uint32_t s2 = c.id();
        c.attach();                           // attach without a seen detach is a new session too
        CHECK_EQ(int(c.fresh()), 0);
        CHECK_EQ(int(c.id() != s2), 1);
    }
    // Lab-only over-load gate (the remote's `pd request ... --over-load`): the K2 may request while a
    // far sink is attached only while that sink stays a passive load - it showed its Rd and sent no
    // PD message this attach. Refused with no far sink, refused after any far PD message.
    CHECK_EQ(int(over_load_allowed({true, false})), 1);    // passive far sink: allowed
    CHECK_EQ(int(over_load_allowed({false, false})), 0);   // no far sink: refused
    CHECK_EQ(int(over_load_allowed({false, true})), 0);    // no far sink at all: refused
    CHECK_EQ(int(over_load_allowed({true, true})), 0);     // the far sink spoke PD this attach: refused
    // The same refused row, spelled per evidence kind behind far_pd_message - the row's passive
    // definition is "Rd only, no Request, no Get_Source_Cap", and every kind of far PD message maps
    // to {far_sink=true, far_pd_message=true}:
    CHECK_EQ(int(over_load_allowed({true, true})), 0);     // .. a foreign Request (far_sink_seen)
    CHECK_EQ(int(over_load_allowed({true, true})), 0);     // .. the sink's Sink_Capabilities (engine.sink)
    CHECK_EQ(int(over_load_allowed({true, true})), 0);     // .. the sink's VDMs (engine.sink_vdm)
    CHECK_EQ(int(over_load_allowed({true, true})), 0);     // .. the sink's control message: Get_Source_Cap / GoodCRC (engine.sink_ctl_seen)
    return test_report("test_pd_passive");
}
