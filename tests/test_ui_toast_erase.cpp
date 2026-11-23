// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A toast that changes text and then expires leaves the page body exactly as a toast-free paint draws it, on pages whose
// rows are signature-cached (the erase band used to stay black), in all four rotations.
#include <cstdio>
#include <memory>
#include <vector>
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
using namespace k2;

static std::vector<uint16_t> frame() { return std::vector<uint16_t>(g_panel, g_panel + LCD_WIDTH * LCD_HEIGHT); }

static void settle(Ui &ui, uint32_t &t) {
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
}

static Ui &fresh_ui(uint8_t rotation) {
    static std::unique_ptr<Ui> ui;
    ui = std::make_unique<Ui>();
    g_stub = StubState();
    g_stub.ms = 10000;
    g_stub.cc = 0;
    g_stub.pm.vbus_mv = 0;
    ui->init(g_stub.ms);
    ui->set_rotation(rotation, g_stub.ms);
    for (uint32_t t = 10100; t <= 11500; t += 100) ui->tick(t);
    g_stub.ms = 12000;
    return *ui;
}

enum class Where { ChargerOverview, ChargerAttached, CableOverview, DeviceOverview, Settings, Lines, Graphs, Detail, Home, ToolsMenu, DisplayMenu, DiagnosisMenu };

// A PD charger behind a plain attach: the overview has live rows.
static void attach_charger(Ui &ui, uint32_t &t) {
    g_stub.cc = 1; g_stub.pm.vbus_mv = 9000; g_stub.pm.current_ma = 0; g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.pd_seen_attach = true;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
}

static void open(Ui &ui, Where w, uint32_t &t) {
    switch (w) {
    case Where::ChargerOverview: ui.open_place(bench::Place::Charger, t); break;
    case Where::ChargerAttached: attach_charger(ui, t); ui.open_place(bench::Place::Charger, t); break;
    case Where::CableOverview: ui.open_place(bench::Place::Cable, t); break;
    case Where::DeviceOverview: ui.open_place(bench::Place::Device, t); break;
    case Where::Settings: ui.goto_page(static_cast<uint8_t>(Page::Settings), t); break;
    case Where::Lines: ui.goto_page(static_cast<uint8_t>(Page::Lines), t); break;
    case Where::Graphs: ui.goto_page(static_cast<uint8_t>(Page::Graphs), t); break;
    case Where::Detail: ui.goto_page(static_cast<uint8_t>(Page::Detail), t); break;
    case Where::Home: ui.goto_page(static_cast<uint8_t>(Page::Home), t); break;
    case Where::ToolsMenu: ui.push_menu(Menu::Tools, t); break;
    case Where::DisplayMenu: ui.push_menu(Menu::Display, t); break;
    case Where::DiagnosisMenu: ui.push_menu(Menu::Diagnosis, t); break;
    }
    settle(ui, t);
}

// Rows of the panel that differ from `base`.
static int diff_px(const std::vector<uint16_t> &base, int &first_y) {
    int n = 0;
    first_y = -1;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++)
        if (g_panel[i] != base[static_cast<size_t>(i)]) { if (first_y < 0) first_y = i / LCD_WIDTH; n++; }
    return n;
}

enum class End { Expire, Clear };

static void run_case(Where w, const char *name, uint8_t rot, End end) {
    Ui &ui = fresh_ui(rot);
    uint32_t t = 20000;
    open(ui, w, t);
    for (int i = 0; i < 40; i++) ui.tick(t += 100);   // control: no toast, the page must hold still
    while (ui.painting()) ui.tick(t += 2);
    const std::vector<uint16_t> base = frame();
    for (int i = 0; i < 40; i++) ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    int cy = -1;
    if (diff_px(base, cy)) { printf("%s rot %u: page not static, skipped (first y=%d)\n", name, rot, cy); return; }
    // The survey's way: a status toast that changes text every second or two, then lapses.
    ui.context().toast.show(FooterTone::Info, "Reading charger 1/3", t, 1500);
    ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    CHECK_EQ(ui.context().toast.drawn, true);
    ui.context().toast.show(FooterTone::Info, "Reading charger 2/3", t, 1500);
    ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    ui.context().toast.show(FooterTone::Ok, "Read: 12 PDOs", t, 3000);
    ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    if (end == End::Clear) ui.context().toast.clear();   // a page dropping its own toast
    for (int i = 0; i < 40; i++) ui.tick(t += 100);   // past the dwell
    while (ui.painting()) ui.tick(t += 2);
    CHECK_EQ(ui.context().toast.active(t), false);
    int first_y = -1;
    const int n = diff_px(base, first_y);
    if (n) printf("%s rot %u %s: %d px differ from the toast-free paint (first y=%d)\n", name, rot, end == End::Clear ? "clear" : "expire", n, first_y);
    CHECK_EQ(n, 0);
}

// While a toast is up, a replaced one must leave the same frame as the same toast shown alone (a shorter pill leaves no
// remnant of the longer one, and the page around it is intact).
static std::vector<uint16_t> shown_frame(Where w, uint8_t rot, const char *first, const char *second) {
    Ui &ui = fresh_ui(rot);
    uint32_t t = 20000;
    open(ui, w, t);
    for (int i = 0; i < 40; i++) ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    if (first) {
        ui.context().toast.show(FooterTone::Info, first, t, 4000);
        ui.tick(t += 100);
        while (ui.painting()) ui.tick(t += 2);
    }
    ui.context().toast.show(FooterTone::Ok, second, t, 4000);
    ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    for (int i = 0; i < 5; i++) ui.tick(t += 100);   // still inside the dwell
    while (ui.painting()) ui.tick(t += 2);
    return frame();
}

static void run_replace(Where w, const char *name, uint8_t rot, const char *first, const char *second) {
    const std::vector<uint16_t> alone = shown_frame(w, rot, nullptr, second);
    // The page may animate (Detail); the reference is a second, identical alone run.
    const std::vector<uint16_t> again = shown_frame(w, rot, nullptr, second);
    if (alone != again) { printf("%s rot %u: not reproducible, skipped\n", name, rot); return; }
    const std::vector<uint16_t> seq = shown_frame(w, rot, first, second);
    int n = 0, fy = -1;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++)
        if (seq[static_cast<size_t>(i)] != alone[static_cast<size_t>(i)]) { if (fy < 0) fy = i / LCD_WIDTH; n++; }
    if (n) printf("%s rot %u replace \"%s\" -> \"%s\": %d px differ from the toast shown alone (first y=%d)\n", name, rot, first, second, n, fy);
    CHECK_EQ(n, 0);
}

// The page the full repaint draws is the truth: whatever the incremental paints left must equal it.
static int vs_repaint(Ui &ui, uint32_t &t, const char *what, uint8_t rot) {
    const std::vector<uint16_t> inc = frame();
    ui.repaint(t);
    while (ui.painting()) ui.tick(t += 2);
    int n = 0, fy = -1;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++)
        if (g_panel[i] != inc[static_cast<size_t>(i)]) { if (fy < 0) fy = i / LCD_WIDTH; n++; }
    if (n) printf("%s rot %u: %d px differ from a full repaint (first y=%d)\n", what, rot, n, fy);
    return n;
}

static const pd::SvCtx kSurveyCtx{.contract = true, .inline_sink = false, .charger_test = false, .xiaomi_chain = false, .pd20 = false, .epr_capable = true};

// The survey's own toasts on the Charger tab ("Reading charger n/m", then the summary), the rows changing under the pill, and
// the lapse landing at different moments of the paint pipeline.
static void run_survey(uint8_t rot, bool jitter) {
    Ui &ui = fresh_ui(rot);
    uint32_t t = 20000;
    attach_charger(ui, t);
    ui.open_place(bench::Place::Charger, t);
    settle(ui, t);
    g_stub.survey.start(kSurveyCtx, t);
    const pd::SvField order[] = {pd::SvField::Caps, pd::SvField::MfrInfo};
    for (pd::SvField f : order) {
        for (int i = 0; i < 6; i++) {
            if (jitter) { g_stub.pm.vbus_mv = 9000 + 37u * static_cast<unsigned>(i); g_stub.pm.current_ma = 100u * static_cast<unsigned>(i); }
            ui.tick(t += 100);
        }
        g_stub.survey.set(f, pd::SvOutcome::Answered);
    }
    g_stub.survey.abandon();
    for (int i = 0; i < 40; i++) {   // the summary lapses while the paint pipeline is busy with the changing values
        if (jitter) { g_stub.pm.vbus_mv = 9000 + 53u * static_cast<unsigned>(i % 7); g_stub.pm.current_ma = 90u * static_cast<unsigned>(i % 5); }
        ui.tick(t += 100);
    }
    g_stub.pm.vbus_mv = 9000; g_stub.pm.current_ma = 0;
    settle(ui, t);
    CHECK_EQ(ui.context().toast.active(t), false);
    CHECK_EQ(vs_repaint(ui, t, jitter ? "survey (live values)" : "survey", rot), 0);
}

// The restore goes through the normal paint pipeline: a tick never writes more than the same page's first paint does, and the
// whole page is not repainted twice (the numbers are printed for the report).
struct Cost { unsigned total = 0, worst = 0, ticks = 0, unique = 0; };

static Cost measure(Ui &ui, uint32_t &t, int ticks, uint32_t step) {
    Cost c;
    lcd_meter_reset();
    for (int i = 0; i < ticks; i++) {
        const unsigned before = g_lcd_pixels;
        ui.tick(t += step);
        const unsigned px = g_lcd_pixels - before;
        c.total += px; c.ticks += px ? 1 : 0;
        if (px > c.worst) c.worst = px;
    }
    c.unique = g_meter_unique;
    return c;
}

static void run_cost(Where w, const char *name, uint8_t rot) {
    Cost entry, expire, shrink;
    {
        Ui &ui = fresh_ui(rot);
        uint32_t t = 20000;
        lcd_meter_reset();
        const unsigned b0 = g_lcd_pixels;
        (void)b0;
        switch (w) {
        case Where::ChargerAttached: attach_charger(ui, t); ui.open_place(bench::Place::Charger, t); break;
        default: ui.goto_page(static_cast<uint8_t>(Page::Lines), t); break;
        }
        entry = measure(ui, t, 60, 2);
    }
    {
        Ui &ui = fresh_ui(rot);
        uint32_t t = 20000;
        open(ui, w, t);
        for (int i = 0; i < 40; i++) ui.tick(t += 100);
        while (ui.painting()) ui.tick(t += 2);
        ui.context().toast.show(FooterTone::Info, "Reading charger 2/3", t, 1500);
        ui.tick(t += 100);
        while (ui.painting()) ui.tick(t += 2);
        ui.context().toast.show(FooterTone::Ok, "Read: 1", t, 30000);   // a shorter pill: the long one must be restored
        shrink = measure(ui, t, 80, 2);
    }
    {
        Ui &ui = fresh_ui(rot);
        uint32_t t = 20000;
        open(ui, w, t);
        for (int i = 0; i < 40; i++) ui.tick(t += 100);
        while (ui.painting()) ui.tick(t += 2);
        ui.context().toast.show(FooterTone::Ok, "Read: 1", t, 1500);
        ui.tick(t += 100);
        while (ui.painting()) ui.tick(t += 2);
        expire = measure(ui, t, 40, 100);   // the lapse and its repaint, then quiet
    }
    printf("cost %s rot %u: page entry %u px (worst tick %u) | pill shrinks %u px in %u ticks, worst %u, %u distinct | pill lapses %u px in %u ticks, worst %u, %u distinct\n",
           name, rot, entry.total, entry.worst, shrink.total, shrink.ticks, shrink.worst, shrink.unique, expire.total, expire.ticks, expire.worst, expire.unique);
    CHECK_EQ(shrink.worst <= entry.worst * 12 / 10, true);   // 20 % slack: the shrink also wipes the band first
    CHECK_EQ(expire.worst <= entry.worst * 12 / 10, true);
}

// A status toast raised by the page itself (Diagnosis) must not come back every time the restore repaints the page.
static void run_status_loop(uint8_t rot) {
    Ui &ui = fresh_ui(rot);
    uint32_t t = 20000;
    open(ui, Where::DiagnosisMenu, t);
    int raised = 0;
    bool was = false;
    for (int i = 0; i < 400; i++) {
        ui.tick(t += 100);
        const bool on = ui.context().toast.active(t);
        if (on && !was) raised++;
        was = on;
    }
    printf("diagnosis rot %u: status toast raised %d time(s) in 40 s\n", rot, raised);
    CHECK_EQ(raised <= 1, true);
}

int main() {
    for (uint8_t rot = 0; rot < 4; rot++) {
        run_survey(rot, false);
        run_survey(rot, true);
        run_status_loop(rot);
        run_cost(Where::ChargerAttached, "charger", rot);
        run_cost(Where::Lines, "lines", rot);
    }
    const struct { Where w; const char *name; } cases[] = {
        {Where::ChargerOverview, "charger overview"}, {Where::ChargerAttached, "charger attached"},
        {Where::CableOverview, "cable"}, {Where::DeviceOverview, "device"}, {Where::Settings, "settings"}, {Where::Lines, "lines"},
        {Where::Graphs, "graphs"}, {Where::Detail, "detail"}, {Where::Home, "home"}, {Where::ToolsMenu, "tools"},
        {Where::DisplayMenu, "display"}, {Where::DiagnosisMenu, "diagnosis"},
    };
    for (const auto &c : cases)
        for (uint8_t rot = 0; rot < 4; rot++)
        {
            for (End e : {End::Expire, End::Clear}) run_case(c.w, c.name, rot, e);
            run_replace(c.w, c.name, rot, "Reading charger 2/3", "Read: 12 PDOs");
            run_replace(c.w, c.name, rot, "Read: 12 PDOs", "Reading charger 2/3");
        }
    return test_report("test_ui_toast_erase");
}
