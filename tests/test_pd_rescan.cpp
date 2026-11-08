// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_rescan.hpp"
#include "test_util.h"

using namespace k2::pd;

int main() {
    // Inline: nothing may touch Rd, CC switches, SWITCHES0/1 or the FIFOs.
    CHECK_EQ(rescan_plan(true).size(), 0u);
    CHECK_EQ(plan_touches_cc(rescan_plan(true)) ? 1 : 0, 0);

    // Standalone: the CC switch cycle and nothing else (no Rd, no chip register, no FIFO).
    const auto p = rescan_plan(false);
    CHECK_EQ(p.size(), 4u);
    CHECK_EQ(int(p[0].kind), int(OpKind::CcSwitches));
    CHECK_EQ(p[0].value, 0);
    CHECK_EQ(int(p[1].kind), int(OpKind::Delay));
    CHECK_EQ(p[1].value, 150);
    CHECK_EQ(int(p[2].kind), int(OpKind::CcSwitches));
    CHECK_EQ(p[2].value, 1);
    CHECK_EQ(int(p[3].kind), int(OpKind::Delay));
    CHECK_EQ(p[3].value, 50);
    CHECK_EQ(plan_touches_cc(p) ? 1 : 0, 1);
    for (const Op& op : p) CHECK_EQ(int(op.kind == OpKind::CcSwitches || op.kind == OpKind::Delay), 1);
    return test_report("test_pd_rescan");
}
