// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The boot-time Ra decision of a self-powered K2 (k2/pd/boot_ra.hpp): arm the selected / journaled cable identity and put the
// Ra on before the source's first attach completes, only when it can be done safely.
#include <cstring>

#include "k2/pd/boot_ra.hpp"
#include "test_util.h"

using namespace k2;
using bootra::In;
using bootra::Verdict;

static In plus_boot() {   // a PLUS boot on a direct receptacle charger: an identity wanted, the other line shows an open Rp
    In in;
    in.self_powered = true; in.identity = true; in.pinned_rd = 2; in.cc1_mv = 3290; in.cc2_mv = 900;
    return in;
}

int main() {
    CHECK_EQ(int(bootra::decide(plus_boot())), int(Verdict::Arm));

    // Only a self-powered boot: a HID-powered (plain) boot keeps today's START-time arming
    { In in = plus_boot(); in.self_powered = false; CHECK_EQ(int(bootra::decide(in)), int(Verdict::NotSelfPowered)); }
    // No identity chosen: nothing changes
    { In in = plus_boot(); in.identity = false; CHECK_EQ(int(bootra::decide(in)), int(Verdict::NoIdentity)); }
    // The Ra rules still hold: the OK key pins CC1, where the Ra sits
    { In in = plus_boot(); in.pinned_rd = 1; CHECK_EQ(int(bootra::decide(in)), int(Verdict::Refused)); }
    { In in = plus_boot(); in.pinned_rd = 3; CHECK_EQ(int(bootra::decide(in)), int(Verdict::Refused)); }
    { In in = plus_boot(); in.inline_sink = true; CHECK_EQ(int(bootra::decide(in)), int(Verdict::Refused)); }
    // The guard: Rp on one line only is a cable in between: the Ra would hide the sink from the source
    { In in = plus_boot(); in.cc1_mv = 40; CHECK_EQ(int(bootra::decide(in)), int(Verdict::Cable)); }
    { In in = plus_boot(); in.cc1_mv = 2399; CHECK_EQ(int(bootra::decide(in)), int(Verdict::Cable)); }
    { In in = plus_boot(); in.cc1_mv = 2400; CHECK_EQ(int(bootra::decide(in)), int(Verdict::Arm)); }
    // No ADC reading: the guard cannot be evaluated, so a selection does not arm (logged, not armed) ...
    { In in = plus_boot(); in.cc1_mv = -1; CHECK_EQ(int(bootra::decide(in)), int(Verdict::NoReading)); }
    // ... but a resumed run's identity already passed the guard at its START, so it stands unless a reading says cable
    { In in = plus_boot(); in.cc1_mv = -1; in.journaled = true; CHECK_EQ(int(bootra::decide(in)), int(Verdict::Arm)); }
    { In in = plus_boot(); in.cc1_mv = 40; in.journaled = true; CHECK_EQ(int(bootra::decide(in)), int(Verdict::Cable)); }
    { In in = plus_boot(); in.cc1_mv = -1; in.journaled = true; in.pinned_rd = 1; CHECK_EQ(int(bootra::decide(in)), int(Verdict::Refused)); }

    // Names (the boot log and the K2B1 text)
    CHECK_STR(bootra::name(Verdict::NotRun), "-");
    CHECK_STR(bootra::name(Verdict::Arm), "arm");
    CHECK_STR(bootra::name(Verdict::NoIdentity), "noid");
    CHECK_STR(bootra::name(Verdict::NotSelfPowered), "plain");
    CHECK_STR(bootra::name(Verdict::Refused), "refused");
    CHECK_STR(bootra::name(Verdict::Cable), "cable");
    CHECK_STR(bootra::name(Verdict::NoReading), "noread");
    CHECK_STR(bootra::name(Verdict::Failed), "fail");

    // The last verdict of this boot, kept for the boot-info text
    bootra::note(Verdict::Arm);
    CHECK_EQ(int(bootra::last()), int(Verdict::Arm));
    bootra::note(Verdict::NotRun);
    return test_report("test_boot_ra");
}
