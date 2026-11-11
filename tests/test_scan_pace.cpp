// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_scanpace.hpp"
#include "test_util.h"

using namespace k2::pd;

int main() {
    {   // the plain cadence: the first scan at once, then every kScanPeriodMs
        ScanPace p;
        CHECK_COND(p.due(1000));
        p.scanned(1000);
        CHECK_COND(!p.due(1020));
        CHECK_COND(!p.due(1499));
        CHECK_COND(p.due(1500));
    }
    {   // the h-side-inline capture: a detach 20 ms after a scan; the old pace scanned 350 ms later, past the offers
        ScanPace p;
        p.scanned(2538910);
        p.kick();   // detach at 2539057
        CHECK_COND(p.due(2539057));          // 147 ms since the last scan: due on the next poll
        p.scanned(2539077);
        CHECK_COND(!p.kicked());
        CHECK_COND(!p.due(2539097));
    }
    {   // a kick right after a scan waits kScanKickMinMs, never one scan per poll
        ScanPace p;
        p.scanned(100);
        p.kick();
        CHECK_COND(!p.due(120));
        CHECK_COND(!p.due(159));
        CHECK_COND(p.due(160));
    }
    {   // VBUS crossing up kicks once; staying up or falling does not; unknown (-1) never kicks
        ScanPace p;
        p.scanned(0);
        p.feed_vbus(-1);
        p.feed_vbus(5100);
        CHECK_COND(!p.kicked());             // the first known reading is no edge
        p.feed_vbus(0);
        p.feed_vbus(1880);
        CHECK_COND(!p.kicked());
        p.feed_vbus(5103);
        CHECK_COND(p.kicked());
        p.scanned(100);
        p.feed_vbus(20000);
        CHECK_COND(!p.kicked());
        p.feed_vbus(0);
        CHECK_COND(!p.kicked());
    }
    {   // the Rd-window line: a new line kicks, the same line again or none does not
        ScanPace p;
        p.scanned(0);
        p.feed_window_line(0);
        CHECK_COND(!p.kicked());
        p.feed_window_line(1);
        CHECK_COND(p.kicked());
        p.scanned(100);
        p.feed_window_line(1);
        CHECK_COND(!p.kicked());
        p.feed_window_line(0);
        p.feed_window_line(2);
        CHECK_COND(p.kicked());
    }
    {   // an edge while attached does not survive into the next detach
        ScanPace p;
        p.scanned(0);
        p.feed_vbus(0);
        p.feed_vbus(5000);
        CHECK_COND(p.kicked());
        p.attached();
        CHECK_COND(!p.kicked());
        CHECK_COND(!p.due(100));
    }
    {   // the millis() wrap
        ScanPace p;
        p.scanned(0xFFFFFF00u);
        p.kick();
        CHECK_COND(!p.due(0xFFFFFF10u));
        CHECK_COND(p.due(0xFFFFFF00u + kScanKickMinMs));
        CHECK_COND(p.due(0x00000100u));
    }
    return test_report("scan_pace");
}
