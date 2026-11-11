// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools hub readiness: the tag must agree with the refusal inside each tool.
#include <string>
#include <cstring>
#include "test_util.h"
#include "k2/ui/ui_tools.hpp"

using namespace k2;

static SetupState st(bool attached, bool inl, bool caps, bool vbus = false, int mv = 0) {
    SetupState s;
    s.attached = attached; s.inline_mode = inl; s.caps_valid = caps; s.vbus_known = vbus; s.vbus_mv = mv;
    return s;
}

static void test_every_tool_has_text() {
    for (int t = 0; t < kToolCount; t++) {
        CHECK_EQ(std::strlen(tool_hint(t)) <= 24, 1);
        CHECK_EQ(std::strlen(tool_hint(t)) > 0, 1);
        for (int mask = 0; mask < 32; mask++) {
            const Readiness r = tool_readiness(t, st(mask & 1, mask & 2, mask & 4, mask & 8, mask & 16 ? 5000 : 0), mask & 8);
            CHECK_EQ(std::strlen(r.tag) <= 6, 1);
            CHECK_EQ(std::strlen(r.reason) <= 28, 1);
            CHECK_EQ(std::strlen(r.reason) > 0, 1);
        }
    }
    // Hints never name a port by position.
    for (int t = 0; t < kToolCount; t++) {
        const std::string h = tool_hint(t);
        CHECK_EQ(h.find("INPUT") == std::string::npos && h.find("OUTPUT") == std::string::npos, 1);
    }
}

static void test_pd_trigger_matches_refusal() {
    // Blocked exactly when the page's setup hint is not ready, or a load is attached.
    for (int mask = 0; mask < 8; mask++) {
        const SetupState s = st(mask & 1, mask & 2, mask & 4);
        const Readiness r = tool_readiness(1,s, true);
        const bool refused = s.inline_mode || !setup_hint(Tool::PdTrigger, s).ready;
        CHECK_EQ(r.level == ToolLevel::Blocked, refused);
    }
    CHECK_STR(tool_readiness(1,st(false, false, false), true).tag, "NO PD");
    CHECK_STR(tool_readiness(1,st(false, false, false), true).reason, "No PD source on either port");
    CHECK_STR(tool_readiness(1,st(true, false, true), true).tag, "READY");
    CHECK_STR(tool_readiness(1,st(true, true, true), true).tag, "LOAD");
}

static void test_fc() {
    CHECK_EQ(tool_readiness(0, st(false, false, false), true).level == ToolLevel::Ready, 1);
    CHECK_EQ(tool_readiness(0, st(true, true, true), true).level == ToolLevel::Blocked, 1);
    CHECK_STR(tool_readiness(0, st(true, true, true), true).reason, "Load attached: unplug it");
    CHECK_STR(tool_readiness(0, st(true, true, true), true).tag, "LOAD");
}

// Connection, Black Box, PD diagnosis, Report cards and Diag never block: Connection's own actions carry the inline confirm.
static void test_always_ready_tools() {
    for (int t = 4; t < kToolCount; t++) {
        if (t == 3) continue;
        for (int mask = 0; mask < 32; mask++) {
            const Readiness r = tool_readiness(t, st(mask & 1, mask & 2, mask & 4, mask & 8, mask & 16 ? 5000 : 0), mask & 8);
            CHECK_EQ(int(r.level), int(ToolLevel::Ready));
        }
    }
}

static void test_cable_r() {
    CHECK_EQ(tool_readiness(3, st(false, false, false), true).level == ToolLevel::Ready, 1);
    const Readiness r = tool_readiness(3, st(false, false, false), false);
    CHECK_EQ(r.level == ToolLevel::Blocked, 1);
    CHECK_STR(r.tag, "NO PM");
    // Out-of-range tool numbers fall back to ready, not a crash.
    CHECK_EQ(tool_readiness(kToolCount, st(false, false, false), true).level == ToolLevel::Ready, 1);
}

static void test_spoof_tool() {
    SetupState st;
    CHECK_EQ(int(tool_readiness(kToolSpoof, st, true).level), int(ToolLevel::Ready));
    st.inline_mode = true;
    CHECK_EQ(int(tool_readiness(kToolSpoof, st, true).level), int(ToolLevel::Blocked));
}

// Each tool owns its predicate and declares what it is hurt by: a tool that draws or asks for power
// (FC trigger, PD trigger, Spoof) is hurt by a load downstream (tag LOAD, blocked); the rest do not care
// (Connection's active actions, Identify and Read cable, carry their own inline confirm). The declaration and
// the live tag must agree.
static void test_hurt_table() {
    const ToolHurt kExpect[kToolCount] = {
        ToolHurt::Load,      // 0 FC trigger: handshakes on D+/D-, a load would take the new voltage
        ToolHurt::Load,      // 1 PD trigger: requests a contract, a load would take it
        ToolHurt::Load,      // 2 Spoof: answers as a cable with Auto-CRC off, a live sink is cut
        ToolHurt::Nothing,   // 3 Cable R: measures under the user's own load
        ToolHurt::Nothing,   // 4 Connection: reads, its active actions confirm on their own
        ToolHurt::Nothing,   // 5 Black Box: passive recorder
        ToolHurt::Nothing,   // 6 PD diagnosis: reads the Black Box records
        ToolHurt::Nothing,   // 7 Report cards: read-only
        ToolHurt::Nothing,   // 8 Diag: counters
        ToolHurt::Nothing,   // 9 Device twins: the live proof refuses a live sink itself
    };
    for (int t = 0; t < kToolCount; t++) {
        CHECK_EQ(int(tool_hurt_by(t)), int(kExpect[t]));
        // A PD charger on one port, the far port empty vs occupied; power monitor present.
        const Readiness quiet = tool_readiness(t, st(true, false, true, true, 5000), true);
        const Readiness inl = tool_readiness(t, st(true, true, true, true, 5000), true);
        CHECK_EQ(std::strcmp(inl.tag, "LOAD") == 0, kExpect[t] == ToolHurt::Load);
        CHECK_EQ(std::strcmp(inl.tag, "INLINE") == 0, kExpect[t] == ToolHurt::Inline);
        if (kExpect[t] == ToolHurt::Nothing) CHECK_EQ(int(inl.level), int(quiet.level));
        if (kExpect[t] == ToolHurt::Load) CHECK_EQ(int(inl.level), int(ToolLevel::Blocked));
        if (kExpect[t] == ToolHurt::Inline) CHECK_EQ(int(inl.level), int(ToolLevel::Caution));
    }
    CHECK_EQ(int(tool_hurt_by(-1)), int(ToolHurt::Nothing));
    CHECK_EQ(int(tool_hurt_by(kToolCount)), int(ToolHurt::Nothing));
}

int main() {
    test_hurt_table();
    test_every_tool_has_text();
    test_pd_trigger_matches_refusal();
    test_fc();
    test_always_ready_tools();
    test_cable_r();
    test_spoof_tool();
    return test_report("test_ui_tools");
}
