// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools hub readiness: the tag must agree with the refusal inside each tool.
#include <string>
#include <cstring>
#include "test_util.h"
#include "k2/ui/ui_connection.hpp"
#include "k2/ui/ui_tools.hpp"
#include "k2/ui/text/tools.hpp"

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
            CHECK_EQ(std::strlen(r.reason) <= 27, 1);
            CHECK_EQ(std::strlen(r.reason) > 0, r.level != ToolLevel::Ready);   // a ready tool's footer is the hub's fact
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
    CHECK_STR(tool_readiness(1,st(false, false, false), true).tag, "no PD");
    CHECK_STR(tool_readiness(1,st(false, false, false), true).reason, "No PD source on either port");
    CHECK_STR(tool_readiness(1,st(true, false, true), true).tag, "ready");
    CHECK_STR(tool_readiness(1,st(true, true, true), true).tag, "load");
}

static void test_fc() {
    CHECK_EQ(tool_readiness(0, st(false, false, false), true).level == ToolLevel::Ready, 1);
    CHECK_EQ(tool_readiness(0, st(true, true, true), true).level == ToolLevel::Blocked, 1);
    CHECK_STR(tool_readiness(0, st(true, true, true), true).reason, "Load attached: unplug it");
    CHECK_STR(tool_readiness(0, st(true, true, true), true).tag, "load");
}

// Charger ID, Cable read, Device ID, Black Box, PD diagnosis, Report cards and Diag never block: Connection's own actions carry the inline confirm.
static void test_always_ready_tools() {
    for (int t = 4; t < kToolCount; t++) {
        if (t == 3 || t == kToolLoopProbe || t == kToolPdStress) continue;   // the ones that need the power monitor (and PD Stress a charger)
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
    CHECK_STR(r.tag, "no PM");
    // Out-of-range tool numbers fall back to ready, not a crash.
    CHECK_EQ(tool_readiness(kToolCount, st(false, false, false), true).level == ToolLevel::Ready, 1);
}

// The loop probe reads everything through the power monitor: without one it is blocked with Cable R's tag, with one it is
// ready whatever is on the ports (its own start refuses VBUS above 5.5 V).
static void test_loop_probe_tool() {
    CHECK_STR(tool_hint(kToolLoopProbe), "cable into own HID port");
    for (int mask = 0; mask < 16; mask++) {
        const SetupState s = st(mask & 1, mask & 2, mask & 4, mask & 8, mask & 8 ? 5000 : 0);
        CHECK_EQ(int(tool_readiness(kToolLoopProbe, s, true).level), int(ToolLevel::Ready));
        const Readiness r = tool_readiness(kToolLoopProbe, s, false);
        CHECK_EQ(int(r.level), int(ToolLevel::Blocked));
        CHECK_STR(r.tag, "no PM");
        CHECK_STR(r.reason, "Power meter not found");
    }
}

// PD Stress needs the three things its run refuses without: an empty output, the power monitor and a PD source list.
static void test_pd_stress_tool() {
    CHECK_STR(tool_hint(kToolPdStress), "PD charger, no load");
    CHECK_STR(tool_readiness(kToolPdStress, st(true, false, true), true).tag, "ready");
    CHECK_STR(tool_readiness(kToolPdStress, st(true, true, true), true).tag, "load");
    CHECK_STR(tool_readiness(kToolPdStress, st(true, false, true), false).tag, "no PM");
    const Readiness r = tool_readiness(kToolPdStress, st(false, false, false), true);
    CHECK_EQ(int(r.level), int(ToolLevel::Blocked));
    CHECK_STR(r.tag, "no PD");
}

// The PD spec checks are the charger test's Deep section, not a Tools slot: PD Stress is the last one.
static void test_tool_slots_end() {
    CHECK_EQ(kToolSlots, 14);
    CHECK_EQ(kToolPdStress, kToolSlots - 1);
    CHECK_EQ(tool_built(kToolPdStress), true);   // the host tests build the opt-in tools
    CHECK_EQ(tool_built(kToolSlots), false);
    CHECK_EQ(tool_built(-1), false);
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
        ToolHurt::Nothing,   // 4 Charger ID: reads, Refresh confirms on its own
        ToolHurt::Nothing,   // 5 Cable read: its read carries the inline confirm
        ToolHurt::Nothing,   // 6 Device ID: Identify carries the inline confirm
        ToolHurt::Nothing,   // 7 Black Box: passive recorder
        ToolHurt::Nothing,   // 8 PD diagnosis: reads the Black Box records
        ToolHurt::Nothing,   // 9 Report cards: read-only
        ToolHurt::Nothing,   // 10 Diag: counters
        ToolHurt::Nothing,   // 11 Device twins: the live proof refuses a live sink itself
        ToolHurt::Nothing,   // 12 Loop probe: its far end is the K2's own HID port; it refuses above 5.5 V itself
        ToolHurt::Load,      // 13 PD Stress: asks the charger for every level, a load would take each one
    };
    for (int t = 0; t < kToolCount; t++) {
        CHECK_EQ(int(tool_hurt_by(t)), int(kExpect[t]));
        // A PD charger on one port, the far port empty vs occupied; power monitor present.
        const Readiness quiet = tool_readiness(t, st(true, false, true, true, 5000), true);
        const Readiness inl = tool_readiness(t, st(true, true, true, true, 5000), true);
        CHECK_EQ(std::strcmp(inl.tag, "load") == 0, kExpect[t] == ToolHurt::Load);
        CHECK_EQ(std::strcmp(inl.tag, "INLINE") == 0, kExpect[t] == ToolHurt::Inline);
        if (kExpect[t] == ToolHurt::Nothing) CHECK_EQ(int(inl.level), int(quiet.level));
        if (kExpect[t] == ToolHurt::Load) CHECK_EQ(int(inl.level), int(ToolLevel::Blocked));
        if (kExpect[t] == ToolHurt::Inline) CHECK_EQ(int(inl.level), int(ToolLevel::Caution));
    }
    CHECK_EQ(int(tool_hurt_by(-1)), int(ToolHurt::Nothing));
    CHECK_EQ(int(tool_hurt_by(kToolCount)), int(ToolHurt::Nothing));
}

// Bench places are info-only (actions() returns empty); active tools live in Tests tabs (TestsPage).
static void test_bench_places_cover_every_tool() {
    using namespace bench;
    for (int p = 0; p < kPlaces; p++) {
        const Acts a = actions(static_cast<Place>(p));
        CHECK_EQ(a.n, 0);
    }
    CHECK_EQ(std::strcmp(place_name(Place::Tests), "Tests"), 0);
    CHECK_EQ(std::strcmp(place_name(Place::K2), "K2"), 0);
}

// A place page's rows: info-only actions (Charger: Read again; Cable: Read e-marker, Save cable, Saved cables;
// Device: no action rows, OK always asks). items[0] is the read OK runs with no row focused.
static std::string place_rows(conn::Tab t, const conn::ActFacts &a) {
    std::array<conn::Item, conn::kMaxItems> it{};
    const int n = conn::place_actions(t, a, it);
    std::string s = std::string(it[0].label) + ":";
    for (int i = 1; i < n; i++) s += std::string(i > 1 ? "," : "") + it[static_cast<size_t>(i)].label;
    return s;
}

static void test_place_actions() {
    conn::ActFacts a;
    CHECK_EQ(place_rows(conn::Tab::Source, a) == "Read again:Read again", true);
    CHECK_EQ(place_rows(conn::Tab::Cable, a) == "Read e-marker:Read e-marker,Save cable,Saved cables", true);
    CHECK_EQ(place_rows(conn::Tab::Sink, a) == "Ask device:", true);
    std::array<conn::Item, conn::kMaxItems> it{};
    const int n = conn::place_actions(conn::Tab::Cable, a, it);
    CHECK_EQ(it[2].enabled, false);                                   // Save cable: no cable read
    CHECK_EQ(n, 4);
    a.attached = true;
    conn::place_actions(conn::Tab::Sink, a, it);
    CHECK_EQ(it[0].enabled, true);                                    // a device to ask
    a.pd_none = true;
    conn::place_actions(conn::Tab::Sink, a, it);
    CHECK_EQ(it[0].enabled, false);
    CHECK_EQ(std::strcmp(it[0].why, conn::kWhyNoPd), 0);
}

static std::string brow(bench::Place p, const bench::Facts &f) {
    const bench::Row r = bench::row(p, f);
    return std::string(r.status.view());
}

static void test_bench_rows() {
    using namespace bench;
    Facts f;
    f.version = "1.0.1-6-e67edd0e-dirty";
    CHECK_EQ(brow(Place::Meter, f) == "No VBUS", true);
    CHECK_EQ(row(Place::Meter, f).known, false);
    CHECK_EQ(brow(Place::Charger, f) == "None on either port", true);
    CHECK_EQ(brow(Place::Cable, f) == "Not read", true);
    CHECK_EQ(brow(Place::Device, f) == "None inline", true);
    CHECK_EQ(brow(Place::Tests, f) == "Charger, cable, batt, dev", true);
    CHECK_EQ(row(Place::Tests, f).known, false);
    CHECK_EQ(brow(Place::Log, f) == "No session", true);
    f.captures = 3;
    CHECK_EQ(brow(Place::Log, f) == "No session, 3 captures", true);
    f.captures = 0;
    CHECK_EQ(brow(Place::K2, f) == "1.0.1-6, HID off", true);
    StrBuf<28> fl;
    fact(f, fl);
    CHECK_EQ(std::string(fl.view()) == "Nothing attached", true);

    f.vbus = true; f.mv = 20040; f.ma = 1834; f.mw = 36753;
    f.caps = true; f.spec_rev = 2; f.max_mw = 100000; f.offers = 7;
    f.cable = true; f.cable_a = 5; f.cable_v = 20; f.cable_w = 100;
    f.contract = true; f.ctr_mv = 20000; f.ctr_ma = 3000;
    f.diag.put("Healthy"); f.captures = 1; f.hid = true;
    f.batt = true; f.batt_status.put("Round trip 80.0%");
    CHECK_EQ(brow(Place::Tests, f) == "Round trip 80.0%", true);
    CHECK_EQ(brow(Place::Meter, f) == "36.75W  20.04V  1.834A", true);
    CHECK_EQ(brow(Place::Charger, f) == "PD3.x 100W, 7 offers", true);
    CHECK_EQ(brow(Place::Cable, f) == "5A 20V, 100W", true);
    CHECK_EQ(brow(Place::Device, f) == "Asks 20V 3A, gets 37W", true);
    CHECK_EQ(brow(Place::Log, f) == "Healthy, 1 capture", true);
    CHECK_EQ(brow(Place::K2, f) == "1.0.1-6, HID on", true);
    f.hid = false; f.webusb = true;
    CHECK_EQ(brow(Place::K2, f) == "1.0.1-6, WebUSB on", true);
    f.hid = true; f.webusb = false;
    f.ctr_mv = 9000; f.ctr_ma = 2220;
    CHECK_EQ(brow(Place::Device, f) == "Asks 9V 2.22A, gets 37W", true);
    for (int p = 0; p < kPlaces; p++) CHECK_EQ(row(static_cast<Place>(p), f).known, true);
    fl.clear();
    fact(f, fl);
    CHECK_EQ(std::string(fl.view()) == "Charger on a port, PD", true);
    f.caps = false; f.fc = "QC3";
    CHECK_EQ(brow(Place::Charger, f) == "QC3 on the lines", true);
}

int main() {
    test_bench_places_cover_every_tool();
    test_place_actions();
    test_bench_rows();
    test_hurt_table();
    test_every_tool_has_text();
    test_pd_trigger_matches_refusal();
    test_fc();
    test_always_ready_tools();
    test_cable_r();
    test_spoof_tool();
    test_loop_probe_tool();
    test_pd_stress_tool();
    test_tool_slots_end();
    return test_report("test_ui_tools");
}
