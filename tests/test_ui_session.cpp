// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for Ui::session() (the GET_STATE part 1 block of the remote): the session facts the Home / Connection /
// chrome screens derive, read with no screenshot. Each field is the same value the page computes (one implementation:
// home::live_proto, conn::sense_passive, busy_why), so these tests pin the mapping, not a second copy of the logic.
#include "test_util.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include <memory>
#include <string.h>

using namespace k2;

// A new Ui every time: init() keeps the submenu stack and the link state of the previous case.
static Ui &fresh_ui() {
    static std::unique_ptr<Ui> ui;
    ui = std::make_unique<Ui>();
    g_stub = StubState();
    g_stub.ms = 10000;
    g_stub.cc = 0;
    g_stub.pm.vbus_mv = 0;
    ui->init(g_stub.ms);
    ui->set_rotation(0, g_stub.ms);
    for (uint32_t t = 10100; t <= 11500; t += 100) ui->tick(t);
    g_stub.ms = 12000;
    return *ui;
}

static void run(Ui &ui, int n, uint32_t step = 100) {
    for (int i = 0; i < n; i++) ui.tick(g_stub.ms += step);
}

static void set_cc(int cc1, int cc2) {
    adc::Reading r;
    r.cc1_mv = static_cast<uint16_t>(cc1);
    r.cc2_mv = static_cast<uint16_t>(cc2);
    g_stub.adc = r;
}

// A 20 V PD contract with 5 A offered on the third PDO, 2 A flowing.
static void live_pd(int ma) {
    g_stub.caps = {};
    static uint32_t rx = 0;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 3; g_stub.caps.spec_rev = 2;
    g_stub.caps.rx_count = ++rx;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    g_stub.contract = {};
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 3;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = 3000; g_stub.contract.max_ma = 3000;
    g_stub.pm = {};
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20010;
    g_stub.pm.current_ma = ma;
    g_stub.pm.power_mw = 20 * ma;
    g_stub.pm.flow = power::Flow::Forward;
    adc::Reading r;
    r.dp_mv = 600; r.dm_mv = 580; r.cc1_mv = 1620; r.cc2_mv = 70; r.vref_raw = 1489; r.vdd_mv = 3300;
    g_stub.adc = r;
    g_stub.cc = 1;
    g_stub.fc = fastcharge::Protocol::Pd;
}

static void idle_fixture() {
    g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 1; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2;
    g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300);
}

// The code and the footer text are one decision: every refusal has a code and busy_reason() is its text.
static void test_busy_code_is_the_reason() {
    {   Ui &ui = fresh_ui(); idle_fixture();
        CHECK_EQ(int(ui.busy_why()), int(BusyWhy::None));
        CHECK_EQ(ui.busy_reason() == nullptr, true);
        CHECK_EQ(int(ui.session().busy), int(BusyWhy::None));
    }
    {   Ui &ui = fresh_ui(); idle_fixture();
        g_stub.sink.held = true; g_stub.rd_run = false;
        CHECK_EQ(int(ui.busy_why()), int(BusyWhy::PdHold));
        CHECK_STR(ui.busy_reason(), "PD hold: release it");
        CHECK_EQ(int(ui.session().busy), int(BusyWhy::PdHold));
    }
    {   Ui &ui = fresh_ui(); idle_fixture();
        g_stub.sink.epr_mode = true;
        CHECK_EQ(int(ui.busy_why()), int(BusyWhy::Epr));
        CHECK_STR(ui.busy_reason(), "EPR contract: exit it");
    }
    {   Ui &ui = fresh_ui(); idle_fixture();
        g_stub.sink.state = pd::SinkState::Requesting;
        CHECK_EQ(int(ui.busy_why()), int(BusyWhy::PdRequest));
        CHECK_STR(ui.busy_reason(), "PD request: wait");
    }
    {   Ui &ui = fresh_ui(); idle_fixture();
        ui.context().em_scanning = true;
        CHECK_EQ(int(ui.busy_why()), int(BusyWhy::EmScan));
        CHECK_STR(ui.busy_reason(), "E-marker scan: wait");
        CHECK_EQ(int(ui.session().flags & kSessionEmScanning), int(kSessionEmScanning));
    }
    {   Ui &ui = fresh_ui(); idle_fixture();
        g_stub.fc_scanning = true;
        CHECK_EQ(int(ui.busy_why()), int(BusyWhy::QcTrigger));
        CHECK_STR(ui.busy_reason(), "QC trigger: stop it");
    }
}

// Energy and elapsed time are the Home numbers: the meter's mAh, and the live elapsed seconds.
static void test_energy_and_elapsed() {
    Ui &ui = fresh_ui();
    live_pd(2000);
    run(ui, 300, 100);   // 30 s of 2 A; the meter started at init, 2 s earlier
    const Ui::Session s = ui.session();
    CHECK_EQ(s.mah, ui.context().meter.mah());
    CHECK_COND(s.mah >= 15 && s.mah <= 17);                       // 2 A for 30 s = 16.7 mAh
    CHECK_EQ(int(s.elapsed_s), 32);
    // VBUS gone: the screens keep the last live snapshot, elapsed freezes with it
    g_stub.pm = {}; g_stub.pm_online = true;
    g_stub.contract = {}; g_stub.caps = {}; g_stub.fc = fastcharge::Protocol::NoInput;
    run(ui, 40, 100);
    CHECK_EQ(int(ui.session().elapsed_s), 32);
}

// The protocol row, limit and highest voltage are home::live_proto's.
static void test_proto_fields() {
    Ui &ui = fresh_ui();
    live_pd(1500);
    run(ui, 40);
    const Ui::Session s = ui.session();
    CHECK_EQ(int(s.proto_kind), int(home::ProtoKind::PdFixed));
    CHECK_EQ(int(s.limit_ma), 5000);
    CHECK_EQ(int(s.max_mv), 20000);
    CHECK_EQ(int(s.typec_ma), int(fusb302::typec_ma()));
    g_stub.pm = {}; g_stub.pm_online = true; g_stub.contract = {}; g_stub.caps = {}; g_stub.fc = fastcharge::Protocol::NoInput;
    run(ui, 40);
    CHECK_EQ(int(ui.session().proto_kind), int(home::ProtoKind::None));
    CHECK_EQ(int(ui.session().limit_ma), 0);
}

// The topology, the waiting classifier and the standby evidence come from the Connection page's classifier.
static void test_passive_fields() {
    Ui &ui = fresh_ui();
    g_stub.pm.vbus_mv = 0;
    set_cc(3300, 3300);
    run(ui, 12);
    const conn::Facts f = ui.passive_facts();
    const Ui::Session s = ui.session();
    CHECK_EQ(int(s.topology), int(f.topo));
    CHECK_EQ(int(s.topology), int(raguard::Topology::DirectReceptacle));
    CHECK_EQ(int(s.wait_kind), int(f.wait));
    CHECK_EQ(int(s.open_cc_mv), int(f.open_cc_mv < 0 ? 0xFFFF : f.open_cc_mv));
    CHECK_EQ(int(s.pd_none_why), int(ui.link().pd_none_why()));
}

// The chrome: where the user is. The stack lists the open menus, the sub byte the Connection tab and its pop-up.
static void test_stack_and_connection() {
    Ui &ui = fresh_ui();
    CHECK_EQ(int(ui.session().depth), 0);
    CHECK_EQ(int(ui.session().sel), 0xFF);   // Home has no list
    ui.goto_page(kMenuPageBase + uint8_t(Menu::Connection), g_stub.ms);
    run(ui, 5);
    Ui::Session s = ui.session();
    CHECK_EQ(int(s.depth), 1);
    CHECK_EQ(int(s.stack[0]), int(Menu::Connection));
    CHECK_EQ(int(s.sub & kSubTabMask), int(conn::Tab::Source));
    CHECK_EQ(int(s.sub & kSubPopup), 0);
    // a list page reports the cursor the rows show
    ui.goto_page(static_cast<uint8_t>(Page::Tools), g_stub.ms);
    run(ui, 5);
    s = ui.session();
    CHECK_EQ(int(s.depth), 0);
    CHECK_COND(s.count > 0);
    CHECK_COND(s.sel < s.count);
}

static void test_home_window_and_flags() {
    Ui &ui = fresh_ui();
    CHECK_EQ(int(ui.session().home_window), int(trend::Window::S10));
    ui.context().keep.home_window = trend::Window::M10;
    CHECK_EQ(int(ui.session().home_window), int(trend::Window::M10));
    CHECK_EQ(int(ui.session().flags & kSessionIdNoDevice), 0);
    ui.context().id_no_device = true;
    CHECK_EQ(int(ui.session().flags & kSessionIdNoDevice), int(kSessionIdNoDevice));
    ui.context().em_quiet = true;
    CHECK_EQ(int(ui.session().flags & kSessionEmQuiet), int(kSessionEmQuiet));
}

int main() {
    test_busy_code_is_the_reason();
    test_energy_and_elapsed();
    test_proto_fields();
    test_passive_fields();
    test_stack_and_connection();
    test_home_window_and_flags();
    return test_report("test_ui_session");
}
