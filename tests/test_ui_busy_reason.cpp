// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for Ui::busy_reason() (src/ui/ui.cpp): what keeps a charger-test START out, and what must not. The self-hold of a
// self-powered or bus-powered K2 (its own plain 5 V PDO 1 contract that keeps it alive) is the K2's supply, not a foreign tool,
// so it never blocks; a hold made with the PD trigger on a HID-powered K2 still does.
#include "test_util.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include <string.h>

using namespace k2;

static void fixture() {
    g_stub = {};
    g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 1; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2;
    g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300);
}

// A self-powered K2 (boot keys held) holds a plain 5 V contract to stay alive: releasing it would cut the K2's power, so the
// hold never refuses START.
static void test_self_powered_hold_does_not_block() {
    fixture(); Ui ui;
    g_stub.self_powered = true;
    g_stub.sink.held = true; g_stub.rd_run = false;   // held, but not by a run's Rd
    CHECK_EQ(ui.busy_reason() == nullptr, true);
}

// A bus-powered K2 (no host, VBUS at boot) is the same: the self-hold is its supply.
static void test_bus_powered_hold_does_not_block() {
    fixture(); Ui ui;
    g_stub.bus_powered = true;
    g_stub.sink.held = true; g_stub.rd_run = false;
    CHECK_EQ(ui.busy_reason() == nullptr, true);
}

// A HID-powered K2 whose PD trigger holds a contract: still refused, named with the way out.
static void test_hid_powered_trigger_hold_blocks() {
    fixture(); Ui ui;
    g_stub.self_powered = false; g_stub.bus_powered = false;
    g_stub.sink.held = true; g_stub.rd_run = false;
    CHECK_EQ(ui.busy_reason() != nullptr, true);
    CHECK_STR(ui.busy_reason(), "PD hold: release it");
}

// The hold START's own PDO 1 request puts on the sink (Rd presented for a run) never blocked, self-supplied or not.
static void test_run_rd_hold_does_not_block() {
    fixture(); Ui ui;
    g_stub.sink.held = true; g_stub.rd_run = true;
    CHECK_EQ(ui.busy_reason() == nullptr, true);
}

int main() {
    test_self_powered_hold_does_not_block();
    test_bus_powered_hold_does_not_block();
    test_hid_powered_trigger_hold_blocks();
    test_run_rd_hold_does_not_block();
    return test_report("test_ui_busy_reason");
}
