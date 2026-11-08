// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// emarker_mode(): the scan method follows the link state.
#include "test_util.h"
#include "k2/pd/pd_emarker_plan.hpp"

using namespace k2::pd;

int main() {
    // Nothing attached: the K2 powers the cable itself, whatever a stale reading says.
    CHECK_EQ(int(emarker_mode(false, false, 0)), int(EmarkerMode::Vconn));
    CHECK_EQ(int(emarker_mode(false, true, 5117)), int(EmarkerMode::Vconn));
    // Attached, VBUS below vSafe0V: no source is driving the line, inject.
    CHECK_EQ(int(emarker_mode(true, true, 0)), int(EmarkerMode::Vconn));
    CHECK_EQ(int(emarker_mode(true, true, kVSafe0Mv - 1)), int(EmarkerMode::Vconn));
    // Attached with VBUS at or above vSafe0V: a source is present, listen only.
    CHECK_EQ(int(emarker_mode(true, true, kVSafe0Mv)), int(EmarkerMode::Sniff));
    CHECK_EQ(int(emarker_mode(true, true, 5117)), int(EmarkerMode::Sniff));
    CHECK_EQ(int(emarker_mode(true, true, 20000)), int(EmarkerMode::Sniff));
    // VBUS unknown while attached: never fight a source.
    CHECK_EQ(int(emarker_mode(true, false, 0)), int(EmarkerMode::Sniff));
    return test_report("test_pd_emarker_plan");
}
