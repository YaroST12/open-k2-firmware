// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_ccscan.hpp"
#include "test_util.h"

using namespace k2::pd;

int main() {
    // Nothing answers on any switch setting: not found, the default routing stays.
    {
        const CcSample s[] = {{3, 1, false, 0}, {3, 2, false, 0}, {1, 1, false, 0}, {1, 2, false, 0}, {2, 1, false, 0}, {2, 2, false, 0}};
        const CcChoice c = choose_cc(s);
        CHECK_EQ(int(c.found), 0);
        CHECK_EQ(int(c.mask), 3);
        CHECK_EQ(int(c.line), 0);
    }
    // The bottom port as before: both switches closed, Rp on CC1.
    {
        const CcSample s[] = {{3, 1, true, 2}, {3, 2, false, 0}, {1, 1, true, 2}, {1, 2, false, 0}, {2, 1, false, 0}, {2, 2, false, 0}};
        const CcChoice c = choose_cc(s);
        CHECK_EQ(int(c.found), 1);
        CHECK_EQ(int(c.mask), 3);
        CHECK_EQ(int(c.line), 1);
    }
    // The top port: only one switch routes the live line, and only on CC2.
    {
        const CcSample s[] = {{3, 1, false, 0}, {3, 2, false, 0}, {1, 1, false, 0}, {1, 2, false, 0}, {2, 1, false, 0}, {2, 2, true, 1}};
        const CcChoice c = choose_cc(s);
        CHECK_EQ(int(c.found), 1);
        CHECK_EQ(int(c.mask), 2);
        CHECK_EQ(int(c.line), 2);
    }
    // A tie keeps the first sample (the default routing, CC1 before CC2).
    {
        const CcSample s[] = {{3, 1, true, 2}, {3, 2, true, 2}};
        CHECK_EQ(int(choose_cc(s).line), 1);
    }
    // A stronger Rp level wins over an earlier weaker one.
    {
        const CcSample s[] = {{3, 1, true, 1}, {3, 2, true, 3}};
        CHECK_EQ(int(choose_cc(s).line), 2);
    }
    // Real top-port sample (trig2.log: sw=3:3/3 sw=1:-/3 sw=2:3/-): the connector's one CC reaches both
    // FUSB302 lines when both switches are closed. Take an isolating setting, not the shorted pair.
    {
        const CcSample s[] = {{3, 1, true, 3}, {3, 2, true, 3}, {1, 1, false, 0}, {1, 2, true, 3}, {2, 1, true, 3}, {2, 2, false, 0}};
        const CcChoice c = choose_cc(s);
        CHECK_EQ(int(c.found), 1);
        CHECK_EQ(int(c.mask), 1);
        CHECK_EQ(int(c.line), 2);   // CC2
    }
    // An isolated line beats a stronger level on a shorted pair.
    {
        const CcSample s[] = {{3, 1, true, 3}, {3, 2, true, 3}, {1, 1, true, 2}, {1, 2, false, 0}};
        const CcChoice c = choose_cc(s);
        CHECK_EQ(int(c.mask), 1);
        CHECK_EQ(int(c.line), 1);
    }
    // Forced line (debug option): 0 = the normal choice; otherwise only that line can be chosen.
    {
        const CcSample s[] = {{3, 1, true, 3}, {3, 2, true, 1}};   // Rp over Rd on CC1, Ra||Rd on CC2
        CHECK_EQ(int(choose_cc_forced(s, 0).line), 1);
        const CcChoice c = choose_cc_forced(s, 2);
        CHECK_EQ(int(c.found), 1);
        CHECK_EQ(int(c.line), 2);
        CHECK_EQ(int(c.mask), 3);
    }
    // The Baseus + e-marked cable seen passively: only CC2 (Ra level) is comp. Forcing CC1 finds nothing
    // (CC1 is above the VCONN/unloaded-Rp limit), so the caller keeps scanning instead of taking the Ra net.
    {
        const CcSample s[] = {{3, 1, false, 0}, {3, 2, true, 1}};
        CHECK_EQ(int(choose_cc_forced(s, 2).line), 2);
        CHECK_EQ(int(choose_cc_forced(s, 1).found), 0);
        CHECK_EQ(int(choose_cc_forced(s, 1).mask), 3);
    }
    // A forced line takes an isolating switch setting first, like the normal choice.
    {
        const CcSample s[] = {{3, 1, true, 2}, {3, 2, true, 2}, {1, 1, false, 0}, {1, 2, true, 2}, {2, 1, true, 2}, {2, 2, false, 0}};
        const CcChoice c = choose_cc_forced(s, 1);
        CHECK_EQ(int(c.found), 1);
        CHECK_EQ(int(c.mask), 2);   // CC1 alone on switch setting 2
        CHECK_EQ(int(c.line), 1);
    }
    // Self-powered boot: the charger is also the K2's supply, so the Rd the K2 pins must reach the connector. A one-switch
    // routing found by the sweep is only a means to tell the lines apart; the final routing closes both and the pinned Rd
    // sits on the active line alone.
    CHECK_EQ(int(self_power_mask(1, true)), 3);
    CHECK_EQ(int(self_power_mask(2, true)), 3);
    CHECK_EQ(int(self_power_mask(3, true)), 3);
    CHECK_EQ(int(self_power_mask(1, false)), 1);     // a plain boot keeps what the scan chose
    CHECK_EQ(int(self_power_mask(2, false)), 2);
    // The sweep opens a switch, and with it the Rd the charger holds VBUS for: skip it when the closed pair already
    // names one live line.
    {
        const CcSample one[] = {{3, 1, false, 0}, {3, 2, true, 2}};
        CHECK_EQ(int(sweep_needed(one)), 0);
        const CcSample tied[] = {{3, 1, true, 2}, {3, 2, true, 2}};   // one connector CC reaching both lines
        CHECK_EQ(int(sweep_needed(tied)), 1);
        const CcSample none[] = {{3, 1, false, 0}, {3, 2, false, 0}};
        CHECK_EQ(int(sweep_needed(none)), 1);
    }
    // A key mode's pinned line wins a direct plug's tie (both lines see the Rp): the charger is attached on it.
    // Hardware 2026-11-06 (boot QR, OnePlus brick on the top plug): the tie went to CC1, the pinned Rd followed it, and
    // releasing PLUS was a detach.
    {
        const CcSample tied[] = {{3, 1, true, 3}, {3, 2, true, 2}};
        CHECK_EQ(int(pinned_live_line(2, tied)), 2);   // PLUS: CC2
        CHECK_EQ(int(pinned_live_line(1, tied)), 1);   // OK: CC1
        CHECK_EQ(int(pinned_live_line(0, tied)), 0);   // plain boot: no preference
        CHECK_EQ(int(choose_cc_forced(tied, pinned_live_line(2, tied)).line), 2);
        CHECK_EQ(int(choose_cc_forced(tied, 0).line), 1);   // what the tie did before
        const CcSample other_only[] = {{3, 1, true, 3}, {3, 2, false, 0}};   // flipped plug: Rp only on the other line
        CHECK_EQ(int(pinned_live_line(2, other_only)), 0);   // no preference; the move rule handles it
        const CcSample swept[] = {{1, 2, true, 3}};          // only the closed pair counts
        CHECK_EQ(int(pinned_live_line(2, swept)), 0);
    }
    // The scan order starts with both closed.
    CHECK_EQ(int(kCcScanMasks[0]), 3);
    // The ADC cross-check of the attach scan: the live wire is the one net inside the Rd window
    // (~0.2-2.0 V); an open Rp reads ~3.3 V and must never win, both-open and both-driven name nothing.
    CHECK_EQ(int(rd_window_line(1678, 3307)), 1);    // Rp over Rd on CC1, open Rp on CC2
    CHECK_EQ(int(rd_window_line(3307, 1678)), 2);   // flipped plug
    CHECK_EQ(int(rd_window_line(3300, 3300)), 0);    // both open
    CHECK_EQ(int(rd_window_line(408, 1)), 0);       // neither reads like one live + one open net
    CHECK_EQ(int(rd_window_line(1500, 3300)), 1);
    CHECK_EQ(int(rd_window_line(-1, 1600)), 2);      // unknown (-1) counts as not-in-window
    // Window edges: [200, 2000] mV inclusive.
    CHECK_EQ(int(rd_window_line(200, 3300)), 1);
    CHECK_EQ(int(rd_window_line(2000, 3300)), 1);
    CHECK_EQ(int(rd_window_line(199, 3300)), 0);
    CHECK_EQ(int(rd_window_line(2001, 3300)), 0);
    CHECK_EQ(int(rd_window_line(1600, 1600)), 0);   // shorted pair: both in the window
    CHECK_EQ(int(rd_window_line(-1, -1)), 0);       // nothing known
    return test_report("test_pd_ccscan");
}
