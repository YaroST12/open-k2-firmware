// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_passive.hpp"
#include "test_util.h"

using namespace k2::pd;

int main() {
    // Auto-CRC is wanted for the whole contract: while the sink engine runs, while a trigger is held
    // (also in Ready, between keep-alives), during Identify, a pending Get_Source_Cap or the prepare.
    CHECK_EQ(int(crc_demand({})), 0);
    CHECK_EQ(int(crc_demand({.sink_active = true})), 1);
    CHECK_EQ(int(crc_demand({.sink_held = true})), 1);
    CHECK_EQ(int(crc_demand({.ident = true})), 1);
    CHECK_EQ(int(crc_demand({.getcap = true})), 1);
    CHECK_EQ(int(crc_demand({.prep = true})), 1);
    CHECK_EQ(int(crc_demand({.run_start = true})), 1);   // START's Rd: the source's caps must be ACKed
    // The armed e-marker spoof needs Auto-CRC, never Rd.
    CHECK_EQ(int(crc_demand({.spoof = true})), 1);
    CHECK_EQ(int(rd_demand({.spoof = true})), 0);
    CHECK_EQ(int(rd_demand({})), 0);
    CHECK_EQ(int(rd_demand({.sink_active = true})), 1);
    CHECK_EQ(int(rd_demand({.sink_held = true})), 1);
    CHECK_EQ(int(rd_demand({.ident = true})), 1);
    CHECK_EQ(int(rd_demand({.getcap = true})), 1);
    CHECK_EQ(int(rd_demand({.prep = true})), 1);
    CHECK_EQ(int(rd_demand({.run_start = true})), 1);
    // A self-powered K2 (a key-mode or inherited Rd pin) is a sink for as long as the source is attached: Auto-CRC on, the
    // contract engaged (never lingered out), Rd is the pinned one anyway.
    CHECK_EQ(int(crc_demand({.self_hold = true})), 1);
    CHECK_EQ(int(rd_demand({.self_hold = true})), 1);
    {   // the self-hold contract never ages out: the demand keeps the linger "engaged"
        ContractLinger l;
        const CrcDemand d{.self_hold = true};
        CHECK_EQ(int(l.update(1000, rd_demand(d))), 1);
        CHECK_EQ(int(l.update(1000 + 10 * kSinkIdleLingerMs, rd_demand(d))), 1);
    }
    {   // a Ready, unheld contract keeps Rd for kSinkIdleLingerMs after the last engaged pass, then lets go
        ContractLinger l;
        CHECK_EQ(int(l.update(500, false)), 0);
        CHECK_EQ(int(l.update(1000, true)), 1);
        CHECK_EQ(int(l.update(1000 + kSinkIdleLingerMs - 1, false)), 1);
        CHECK_EQ(int(l.update(1000 + kSinkIdleLingerMs, false)), 0);
        CHECK_EQ(int(l.update(2000 + kSinkIdleLingerMs, true)), 1);
        CHECK_EQ(int(l.update(2000 + kSinkIdleLingerMs + 10, false)), 1);
        l.reset();
        CHECK_EQ(int(l.update(2000 + kSinkIdleLingerMs + 20, false)), 0);
    }
    {   // a held contract keeps the gate open far beyond the 300 ms linger
        AutoCrcGate g;
        CHECK_EQ(int(g.update(0, crc_demand({.sink_held = true}))), 1);
        CHECK_EQ(int(g.update(60000, crc_demand({.sink_held = true}))), 1);
        CHECK_EQ(int(g.update(60100, crc_demand({}))), 1);     // released: the linger still ACKs the last reply
        CHECK_EQ(int(g.update(60300, crc_demand({}))), 0);
    }

    // Caps were never ACKed (passive): open the CC lines, wait for fresh caps, request 5 ms after them.
    {
        TriggerPrep p;
        p.start(1000, /*caps_count*/ 4, /*caps_acked*/ false);
        CHECK_EQ(int(p.active()), 1);
        CHECK_EQ(int(p.tick(1000, 4)), int(TriggerPrep::Act::OpenCc));
        CHECK_EQ(int(p.tick(1100, 4)), int(TriggerPrep::Act::None));
        CHECK_EQ(int(p.tick(1000 + kPrepOpenMs, 4)), int(TriggerPrep::Act::CloseCc));
        CHECK_EQ(int(p.tick(1400, 4)), int(TriggerPrep::Act::None));            // no caps yet
        CHECK_EQ(int(p.tick(1500, 5)), int(TriggerPrep::Act::None));            // caps arrive: wait 5 ms
        CHECK_EQ(int(p.tick(1504, 5)), int(TriggerPrep::Act::None));
        CHECK_EQ(int(p.tick(1505, 5)), int(TriggerPrep::Act::Send));
        CHECK_EQ(int(p.active()), 0);
        CHECK_EQ(int(p.tick(1600, 6)), int(TriggerPrep::Act::None));
    }
    // The Anker A2343 re-announces ~1.5-2 s after the detach (cable discovery first): the Request must not
    // go out on the stale caps before that, or the source GoodCRCs it and never Accepts.
    {
        TriggerPrep p;
        p.start(0, 1, false);
        p.tick(0, 1);
        p.tick(kPrepOpenMs, 1);
        CHECK_EQ(int(p.tick(kPrepOpenMs + 1500, 1)), int(TriggerPrep::Act::None));
        CHECK_EQ(int(p.tick(kPrepOpenMs + 2000, 2)), int(TriggerPrep::Act::None));   // caps arrive late
        CHECK_EQ(int(p.tick(kPrepOpenMs + 2005, 2)), int(TriggerPrep::Act::Send));
    }
    // Auto-CRC stays off while the lines are open and until they are closed (the Anker A2343 never re-announced when
    // the chip was switched to Auto-CRC before the CC cycle); it is wanted from the close on, to ACK the first caps.
    {
        TriggerPrep p;
        p.start(0, 1, false);
        CHECK_EQ(int(p.crc_wanted()), 0);
        p.tick(0, 1);
        CHECK_EQ(int(p.crc_wanted()), 0);
        p.tick(kPrepOpenMs, 1);
        CHECK_EQ(int(p.crc_wanted()), 1);   // lines closed: ACK the very first caps, or the source retries them into our Request
    }
    // The charger never re-announces: send anyway after the wait.
    {
        TriggerPrep p;
        p.start(0, 1, false);
        CHECK_EQ(int(p.tick(0, 1)), int(TriggerPrep::Act::OpenCc));
        CHECK_EQ(int(p.tick(kPrepOpenMs, 1)), int(TriggerPrep::Act::CloseCc));
        CHECK_EQ(int(p.tick(kPrepOpenMs + kPrepWaitCapsMs - 1, 1)), int(TriggerPrep::Act::None));
        // first wait over, still silent: one longer second open, then wait again
        const std::uint32_t t = kPrepOpenMs + kPrepWaitCapsMs;
        CHECK_EQ(int(p.tick(t, 1)), int(TriggerPrep::Act::None));
        CHECK_EQ(int(p.tick(t + 1, 1)), int(TriggerPrep::Act::OpenCc));
        CHECK_EQ(int(p.crc_wanted()), 0);
        CHECK_EQ(int(p.tick(t + kPrepRetryOpenMs - 1, 1)), int(TriggerPrep::Act::None));
        CHECK_EQ(int(p.tick(t + kPrepRetryOpenMs, 1)), int(TriggerPrep::Act::CloseCc));
        const std::uint32_t t2 = t + kPrepRetryOpenMs;
        CHECK_EQ(int(p.tick(t2 + kPrepWaitCapsMs - 1, 1)), int(TriggerPrep::Act::None));
        CHECK_EQ(int(p.tick(t2 + kPrepWaitCapsMs, 1)), int(TriggerPrep::Act::Send));
    }
    // The last caps were ACKed already (the source is in a live PD state): no CC cycle.
    {
        TriggerPrep p;
        p.start(0, 3, true);
        CHECK_EQ(int(p.tick(0, 3)), int(TriggerPrep::Act::Send));
        CHECK_EQ(int(p.active()), 0);
    }
    // abort() drops a running prepare (detach).
    {
        TriggerPrep p;
        p.start(0, 3, false);
        p.abort();
        CHECK_EQ(int(p.active()), 0);
        CHECK_EQ(int(p.tick(10, 3)), int(TriggerPrep::Act::None));
    }
    return test_report("test_pd_trigger_prep");
}
