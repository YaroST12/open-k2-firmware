// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Battery place (Bench > Battery): the pure view (k2/ui/battery_view.hpp) - number formats, the rows of each recorder
// state, the identity block and the footer - and the page through the real Ui: the Bench row, opening the place, starting a
// run with OK, the running dot on another page, the stored runs, and the Classic render.
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "fake_lcd.h"
#include "k2/battery/battery_hw.hpp"
#include "k2/ui/battery_view.hpp"
#include "k2/ui/theme.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_battery.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_tools.hpp"
#include "test_util.h"
#include "ui_stubs.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;
namespace bt = k2::battery;
namespace view = k2::battery::view;

static bool eq(std::string_view a, std::string_view b) {
    char dec[64];
    a = strpack::unpack(a, dec);   // names may be packed
    if (a != b) std::printf("  got \"%.*s\", want \"%.*s\"\n", int(a.size()), a.data(), int(b.size()), b.data());
    return a == b;
}

static void test_numbers() {
    CHECK(eq(view::wh(0).view(), "0.000Wh"));
    CHECK(eq(view::wh(9'999'999).view(), "9.999Wh"));
    CHECK(eq(view::wh(10'001'000).view(), "10.00Wh"));
    CHECK(eq(view::wh(123'456'789).view(), "123.4Wh"));
    CHECK(eq(view::wh(1'234'567'890).view(), "1234Wh"));
    CHECK(eq(view::mah(2'000'400).view(), "2000mAh"));
    CHECK(eq(view::mah(123'456'000).view(), "123.4Ah"));
    CHECK(eq(view::hms(45).view(), "45s"));
    CHECK(eq(view::hms(754).view(), "12m34s"));
    CHECK(eq(view::hms(3912).view(), "1h05m12s"));
    CHECK(eq(view::hm(3912).view(), "1h05m"));
    CHECK(eq(view::hm(754).view(), "12m"));
    CHECK(eq(view::pct(800).view(), "80.0%"));
    CHECK(eq(view::pct(1234).view(), "123.4%"));
    CHECK_EQ(view::next_step(view::kEndMaSteps, 5, 50), 100);
    CHECK_EQ(view::next_step(view::kEndMaSteps, 5, 500), 20);
    CHECK_EQ(view::next_step(view::kEndMaSteps, 5, 77), 20);   // off the steps: back to the first
}

static bt::Record record_of(const bt::Recorder &r) {
    bt::Record rec;
    r.to_record(rec);
    return rec;
}

static std::uint32_t feed(bt::Recorder &r, std::uint32_t t, std::uint32_t ms, std::int32_t mv, std::int32_t ma) {
    for (std::uint32_t e = 0; e < ms; e += 1000, t += 1000) r.sample(t, mv, ma, true);
    return t;
}

static void test_rows() {
    using A = view::Act;
    bt::Recorder r;
    view::Shown s = view::shown(record_of(r), false);
    view::Rows rw = view::rows(s, false);
    CHECK_EQ(rw.facts, 0);   // nothing measured since boot: only what can be started
    CHECK_EQ(int(rw.a[0]), int(A::StartCharge));
    CHECK_EQ(int(rw.a[2]), int(A::StartRoundTrip));
    CHECK_EQ(int(rw.a[rw.n - 1]), int(A::Runs));
    r.start(bt::Mode::RoundTrip, bt::Wiring::SamePort, bt::Config{}, 3);
    std::uint32_t t = feed(r, 0, 600'000, 5000, 2000);
    s = view::shown(record_of(r), true);
    rw = view::rows(s, false);
    CHECK_EQ(rw.facts, 3);   // In, Out, Time (no efficiency before the discharge)
    CHECK_EQ(int(rw.a[3]), int(A::EndPhase));
    CHECK_EQ(int(rw.a[4]), int(A::Finish));
    CHECK(eq(view::act_name(A::EndPhase, s), "End charge"));
    r.end_phase();
    s = view::shown(record_of(r), true);
    rw = view::rows(s, false);
    CHECK_EQ(int(rw.a[rw.facts]), int(A::Next));
    CHECK(eq(view::act_name(A::Next, s), "Start discharge"));
    r.next();
    t = feed(r, t, 480'000, 5000, -2000);
    s = view::shown(record_of(r), true);
    rw = view::rows(s, false);
    CHECK_EQ(rw.facts, 4);   // and the efficiency so far
    CHECK_EQ(int(rw.a[3]), int(A::Eff));
    CHECK(s.eff >= 795 && s.eff <= 800);
    CHECK_EQ(s.eff_final, false);
    CHECK(eq(view::act_name(A::EndPhase, s), "End discharge"));
    r.finish();
    s = view::shown(record_of(r), true);
    CHECK_EQ(s.eff_final, true);
    rw = view::rows(s, false);
    CHECK_EQ(int(rw.a[rw.n - 1]), int(A::Share));
    CHECK_EQ(int(rw.a[rw.facts]), int(A::Continue));   // the run just ended goes on from here
    CHECK_EQ(rw.n, 13);                                 // 4 facts, Continue, 3 starts, 3 settings, Saved runs, Share
    CHECK(eq(view::act_name(A::Continue, s), "Continue run"));
    CHECK_EQ(int(view::act_verb(A::Continue)), int(sl::Resume));
    rw = view::rows(s, true, true);   // the newest stored run, nothing measuring: Continue too
    CHECK_EQ(rw.n, rw.facts + 2);
    CHECK_EQ(int(rw.a[rw.facts]), int(A::Continue));
    rw = view::rows(s, true);   // a stored run: its facts and Share only
    CHECK_EQ(rw.n, rw.facts + 1);
    for (int i = 0; i < rw.n; i++) CHECK(view::is_fact(rw.a[i]) == (i < rw.facts));
    (void)t;
}

static void test_ident_and_footer() {
    bt::Recorder r;
    kit::Ident id;
    view::ident(view::shown(record_of(r), false), id);
    CHECK(eq(id.head.view(), "No run"));
    CHECK(id.dim);
    StrBuf<28> f;
    CHECK_EQ(int(view::footer(view::shown(record_of(r), false), true, false, false, false, f)), int(FooterTone::Info));
    f.clear();
    CHECK_EQ(int(view::footer(view::shown(record_of(r), false), false, false, false, false, f)), int(FooterTone::Error));
    CHECK(eq(f.view(), "No power monitor"));
    r.start(bt::Mode::Charge, bt::Wiring::SamePort, bt::Config{}, 4);
    feed(r, 0, 3'912'000, 5000, 1000);
    view::Shown s = view::shown(record_of(r), true);
    id = kit::Ident{};
    view::ident(s, id);
    CHECK(eq(id.head.view(), "Charging"));
    CHECK(eq(id.line.view(), "Same port, 1h05m11s, 5.431Wh"));   // 5 W for 3911 s = 5.4319 Wh, cut
    f.clear();
    CHECK_EQ(int(view::footer(s, true, false, false, false, f)), int(FooterTone::Info));
    CHECK(eq(f.view(), "Ends under 50mA for 60s"));
    f.clear();
    CHECK_EQ(int(view::footer(s, true, true, false, false, f)), int(FooterTone::Caution));   // the last write failed
    StrBuf<29> b;
    view::bench_status(s, b);
    CHECK(eq(b.view(), "Charging 1h05m, 5.431Wh"));
    // a round trip, done
    bt::Recorder q;
    q.start(bt::Mode::RoundTrip, bt::Wiring::Moved, bt::Config{}, 5);
    std::uint32_t t = feed(q, 0, 3'601'000, 5000, 2000);
    q.end_phase();
    q.next();
    t = feed(q, t, 3'601'000, 5000, 1600);   // moved: the bank's outflow is forward
    q.finish();
    s = view::shown(record_of(q), true);
    id = kit::Ident{};
    view::ident(s, id);
    CHECK(eq(id.head.view(), "80.0%"));
    CHECK_EQ(id.n, 1);
    CHECK(eq(id.chip[0].t.view(), "Round trip"));
    f.clear();
    CHECK_EQ(int(view::footer(s, true, false, false, false, f)), int(FooterTone::Ok));
    CHECK(eq(f.view(), "Round trip 80.0% efficient"));
    f.clear();
    CHECK_EQ(int(view::footer(s, true, false, false, true, f)), int(FooterTone::Caution));   // ended empty: not saved
    CHECK(eq(f.view(), "Empty run, not saved"));
    b.clear();
    view::bench_status(s, b);
    CHECK(eq(b.view(), "Round trip 80.0%"));
    b.clear();
    view::bench_status(view::shown(record_of(r), false), b);
    CHECK(b.view().empty());   // no run: the Bench row says so itself
    (void)t;
}

// ---- through the Ui -------------------------------------------------------------------------------------------------------

static int count_px(std::uint16_t c, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) n += g_panel[y * LCD_WIDTH + x] == c;
    return n;
}

static Ui &fresh_ui() {
    static Ui ui;
    stub_battery_reset();
    g_stub.ms = 1000;
    ui.init(g_stub.ms);
    ui.set_rotation(0, g_stub.ms);
    return ui;
}

static void press(Ui &ui, Button b, std::uint32_t &t) {
    ui.on_button(b, ButtonEvent::Short, t);
    t += 100;
    ui.tick(t);
    while (ui.painting()) ui.tick(t += 20);
}

static void open_settled(Ui &ui, bench::Place pl, std::uint32_t &t) {
    ui.open_place(pl, t);
    ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 20);
}

static void test_page() {
    Ui &ui = fresh_ui();
    std::uint32_t t = 1000;
    ui.tick(t);
    // the Bench row
    bench::Facts f;
    CHECK(eq(bench::row(bench::Place::Battery, f).status.view(), "No run"));
    open_settled(ui, bench::Place::Battery, t);
    CHECK_EQ(int(ui.menu()), int(Menu::Battery));
    const BatteryPage *p = ui.battery_page();
    CHECK(p != nullptr);
    if (!p) return;
    CHECK_EQ(p->selected(), 0);   // Start charge
    // OK starts a charge (the stub monitor reads 5 V)
    press(ui, Button::Ok, t);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Measuring));
    CHECK_EQ(int(bt::hw::service().rec().mode()), int(bt::Mode::Charge));
    g_stub.pm.vbus_mv = 5000;
    g_stub.pm.current_ma = 1000;                                                   // 5 W for 3 s: a run worth saving
    for (int i = 0; i < 4; i++) bt::hw::poll(t += 1000, true);
    g_stub.pm.current_ma = 0;
    // the running dot on another page
    ui.on_button(Button::Back, ButtonEvent::Short, t);   // pops the place
    ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 20);
    open_settled(ui, bench::Place::Meter, t);
    CHECK(count_px(kFocus, 216, 0, 224, 8) >= 20);   // rotation 0: content x = panel x, the strip is x224..239
    // finish by hand: End charge needs a second OK
    open_settled(ui, bench::Place::Battery, t);
    p = ui.battery_page();
    if (!p) return;
    while (p->selected() < p->list_state().count - 1 && p->act() != view::Act::EndPhase) press(ui, Button::Plus, t);
    CHECK_EQ(int(p->act()), int(view::Act::EndPhase));
    press(ui, Button::Ok, t);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Measuring));   // armed only
    press(ui, Button::Ok, t);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Done));
    bt::hw::poll(t, true);                                                         // the record is written
    CHECK_EQ(bt::hw::service().rec().dirty(), false);
    // the stored runs: Saved runs > the run
    while (p->selected() < p->list_state().count - 1 && p->act() != view::Act::Runs) press(ui, Button::Plus, t);
    CHECK_EQ(int(p->act()), int(view::Act::Runs));
    press(ui, Button::Ok, t);
    CHECK_EQ(p->list_state().count, 1);
    press(ui, Button::Ok, t);
    CHECK(p->viewing_stored());
    press(ui, Button::Back, t);
    press(ui, Button::Back, t);
    CHECK(!p->viewing_stored());
    CHECK_EQ(int(ui.menu()), int(Menu::Battery));
}

// The rows are marker-less like the Tools and Settings menus: text at x28, nothing in the marker box. A text line is a band of
// rows with ink at x28.. at least 10 px tall (a font 12 note is shorter); its marker box x8..19 must stay empty.
static int rows_with_marker() {
    const int x0 = kOppRailThickness;
    int bad = 0;
    for (int y = kit::kIdDashY + 2; y < 240;) {
        auto ink_row = [&](int yy, int a, int b) {
            const std::uint16_t ground = g_panel[yy * LCD_WIDTH + x0 + 24];
            for (int x = a; x < b; x++)
                if (g_panel[yy * LCD_WIDTH + x0 + x] != ground) return true;
            return false;
        };
        if (!ink_row(y, 28, 200)) { y++; continue; }
        int y1 = y;
        while (y1 < 240 && ink_row(y1, 28, 200)) y1++;
        if (y1 - y >= 10) {
            bool marker = false;
            for (int yy = y; yy < y1 && !marker; yy++) marker = ink_row(yy, 8, 20);
            if (marker) std::printf("  a row at y%d..%d has a marker\n", y, y1 - 1);
            bad += marker;
        }
        y = y1;
    }
    return bad;
}

static void test_rows_have_no_marker() {
    Ui &ui = fresh_ui();
    std::uint32_t t = 1000;
    ui.tick(t);
    open_settled(ui, bench::Place::Battery, t);
    const BatteryPage *p = ui.battery_page();
    CHECK(p != nullptr);
    if (!p) return;
    for (int i = 0; i < 20 && p->selected() > 0; i++) press(ui, Button::Minus, t);
    int bad = rows_with_marker(), seen = 1;
    while (p->selected() < p->list_state().count - 1 && seen < 20) {
        ++seen;
        press(ui, Button::Plus, t);
        bad += rows_with_marker();
    }
    CHECK_EQ(seen >= 6, true);   // walked the whole list: three starts, three settings
    CHECK_EQ(bad, 0);
}

// The place in Classic (the only theme): the identity's dashed rule in ink3, nothing left of the page under it from the Bench.
static void test_classic() {
    Ui &ui = fresh_ui();
    std::uint32_t t = 1000;
    ui.tick(t);
    bt::hw::service().start(bt::Mode::RoundTrip, bt::Wiring::SamePort, true);
    for (int i = 0; i < 30; i++) bt::hw::poll(t += 1000, true);
    open_settled(ui, bench::Place::Battery, t);
    CHECK(count_px(kInk3, kOppRailThickness + 8, kit::kIdDashY, kOppRailThickness + 200, kit::kIdDashY + 1) > 90);   // the perforation under the identity block (y kIdDashY)
    CHECK(count_px(kBg, kOppRailThickness, 0, kOppRailThickness + 208, 240) > 208 * 100);
}

// Continue: from the run just ended (main view), and after a boot from the newest stored run (Saved runs > the run); both arm
// first like End. An older stored run has no Continue row.
static int find_row(Ui &ui, const BatteryPage *p, view::Act a, std::uint32_t &t) {
    for (int i = 0; i < 20 && p->selected() > 0; i++) press(ui, Button::Minus, t);
    while (p->act() != a && p->selected() < p->list_state().count - 1) press(ui, Button::Plus, t);
    return p->act() == a;
}

static void run_and_end(std::uint32_t &t) {
    bt::hw::service().start(bt::Mode::Charge, bt::Wiring::SamePort, true);
    g_stub.pm.vbus_mv = 5000;
    g_stub.pm.current_ma = 1000;
    for (int i = 0; i < 5; i++) bt::hw::poll(t += 1000, true);
    g_stub.pm.current_ma = 0;
    bt::hw::service().rec().end_phase();
    bt::hw::poll(t += 1000, true);
}

static void test_continue_page() {
    Ui &ui = fresh_ui();
    std::uint32_t t = 1000;
    ui.tick(t);
    run_and_end(t);
    open_settled(ui, bench::Place::Battery, t);
    const BatteryPage *p = ui.battery_page();
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(find_row(ui, p, view::Act::Continue, t));
    press(ui, Button::Ok, t);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Done));        // armed only
    CHECK_EQ(int(p->labels()[Button::Ok]), int(sl::Confirm));
    press(ui, Button::Ok, t);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Measuring));
    CHECK_EQ(int(p->act()), int(view::Act::EndPhase));                              // the next thing to do
    bt::hw::service().rec().finish();
    bt::hw::poll(t += 1000, true);
    run_and_end(t);                                                                 // run 2, the newest
    // a boot: the recorder is idle, the runs are on flash
    bt::hw::init(false);
    press(ui, Button::Back, t);
    open_settled(ui, bench::Place::Battery, t);
    p = ui.battery_page();
    if (!p) return;
    CHECK(!find_row(ui, p, view::Act::Continue, t));                               // nothing in RAM to continue here
    CHECK(find_row(ui, p, view::Act::Runs, t));
    press(ui, Button::Ok, t);
    CHECK_EQ(p->list_state().count, 2);
    press(ui, Button::Plus, t);                                                     // the older run: no Continue
    press(ui, Button::Ok, t);
    CHECK(p->viewing_stored());
    CHECK(!find_row(ui, p, view::Act::Continue, t));
    press(ui, Button::Back, t);
    press(ui, Button::Minus, t);                                                    // the newest
    press(ui, Button::Ok, t);
    CHECK(p->viewing_stored());
    CHECK(find_row(ui, p, view::Act::Continue, t));
    press(ui, Button::Ok, t);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Idle));
    press(ui, Button::Ok, t);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Measuring));
    CHECK_EQ(bt::hw::service().rec().id(), 2);
    CHECK(!p->viewing_stored());                                                    // back on the main view, measuring
    bt::hw::service().rec().finish();
    bt::hw::poll(t += 1000, true);
}

// Continue is only ever where the user moved the cursor: not where it lands after End, not on a row that turned into Continue
// under a parked cursor when the end rule closed the run, not on the place's remembered row (run 6 of 2026-11-25).
static void test_continue_never_under_the_cursor() {
    Ui &ui = fresh_ui();
    std::uint32_t t = 1000;
    ui.tick(t);
    bt::hw::service().start(bt::Mode::Charge, bt::Wiring::SamePort, true);
    g_stub.pm.vbus_mv = 5000;
    g_stub.pm.current_ma = 1000;
    for (int i = 0; i < 5; i++) bt::hw::poll(t += 1000, true);
    open_settled(ui, bench::Place::Battery, t);
    const BatteryPage *p = ui.battery_page();
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(find_row(ui, p, view::Act::EndPhase, t));
    press(ui, Button::Ok, t);
    press(ui, Button::Ok, t);                                                       // End charge, confirmed
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Done));
    CHECK(p->act() != view::Act::Continue);
    press(ui, Button::Ok, t);
    press(ui, Button::Ok, t);                                                       // the same two presses again
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Done));
    // a parked cursor on End charge while the end rule closes the run
    bt::hw::service().rec().finish();
    bt::hw::poll(t += 1000, true);
    bt::hw::service().start(bt::Mode::Charge, bt::Wiring::SamePort, true);
    g_stub.pm.current_ma = 1000;
    for (int i = 0; i < 5; i++) bt::hw::poll(t += 1000, true);
    press(ui, Button::Back, t);
    open_settled(ui, bench::Place::Battery, t);
    p = ui.battery_page();
    if (!p) return;
    CHECK(find_row(ui, p, view::Act::EndPhase, t));
    g_stub.pm.current_ma = 0;
    for (int i = 0; i < 65; i++) {
        bt::hw::poll(t += 1000, true);
        ui.tick(t);
    }
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Done));
    CHECK(p->act() != view::Act::Continue);
    press(ui, Button::Ok, t);
    press(ui, Button::Ok, t);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Done));
    // left on End charge, the run ends elsewhere, the place opens again
    bt::hw::service().rec().finish();
    bt::hw::poll(t += 1000, true);
    bt::hw::service().start(bt::Mode::Charge, bt::Wiring::SamePort, true);
    g_stub.pm.current_ma = 1000;
    for (int i = 0; i < 5; i++) bt::hw::poll(t += 1000, true);
    while (ui.painting()) ui.tick(t += 20);
    CHECK(find_row(ui, p, view::Act::EndPhase, t));
    press(ui, Button::Back, t);
    g_stub.pm.current_ma = 0;
    for (int i = 0; i < 65; i++) bt::hw::poll(t += 1000, true);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Done));
    open_settled(ui, bench::Place::Battery, t);
    p = ui.battery_page();
    if (!p) return;
    CHECK(p->act() != view::Act::Continue);
    press(ui, Button::Ok, t);
    press(ui, Button::Ok, t);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Done));
    // the newest stored run open on Share while a run started elsewhere (the remote) measures and then ends empty: the
    // stored run's Continue row comes back under the cursor
    CHECK(find_row(ui, p, view::Act::Runs, t));
    press(ui, Button::Ok, t);
    press(ui, Button::Ok, t);
    CHECK(p->viewing_stored());
    CHECK(find_row(ui, p, view::Act::Share, t));
    bt::hw::service().start(bt::Mode::Charge, bt::Wiring::SamePort, true);
    for (int i = 0; i < 3; i++) bt::hw::poll(t += 1000, true);
    ui.tick(t += 1000);
    while (ui.painting()) ui.tick(t += 20);
    bt::hw::service().rec().end_phase();
    bt::hw::poll(t += 1000, true);
    ui.tick(t += 1000);
    while (ui.painting()) ui.tick(t += 20);
    CHECK(p->act() != view::Act::Continue);
    press(ui, Button::Ok, t);
    press(ui, Button::Ok, t);
    CHECK_EQ(int(bt::hw::service().rec().state()), int(bt::State::Done));
}

// K2_DUMP=dir: the screen as k2_bt_<name>_r<rot>.panel (tools/panel2png.py), for the renders of a change.
static void dump(const char *name, int rot) {
    const char *dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char path[256];
    std::snprintf(path, sizeof path, "%s/k2_bt_%s_r%d.panel", dir, name, rot);
    if (FILE *f = std::fopen(path, "wb")) {
        std::fwrite(g_panel, sizeof g_panel[0], LCD_WIDTH * LCD_HEIGHT, f);
        std::fclose(f);
    }
}

// One-phase and round-trip records with the longest values a run can show: a 3-digit id, 123.4 Wh, 99999 mAh, 99h59m59s.
static bt::Record long_record(std::uint16_t id, bt::Mode m, bool lost, bt::RunState st = bt::RunState::Done) {
    bt::Record r;
    r.mode = m;
    r.wiring = bt::Wiring::SamePort;
    r.state = st;
    r.armed = true;
    r.id = id;
    r.resumes = 12;
    r.gap_s = 3599;
    const bool trip = m == bt::Mode::RoundTrip;
    const bool run1 = st != bt::RunState::Done && !trip;
    r.phases = trip && st != bt::RunState::Between ? 2 : 1;
    r.ph[0] = bt::PhaseRec{m == bt::Mode::Discharge ? bt::Kind::Discharge : bt::Kind::Charge,
                           run1 ? bt::End::Running : lost && !trip ? bt::End::Lost : bt::End::Auto, std::uint16_t(trip ? 0 : 999), 359'999,
                           123'456'789, 1'234'567, 99'999'000, 99'999};
    if (m == bt::Mode::Discharge) { std::swap(r.ph[0].uwh_in, r.ph[0].uwh_out); std::swap(r.ph[0].uah_in, r.ph[0].uah_out); }
    if (r.phases == 2)
        r.ph[1] = bt::PhaseRec{bt::Kind::Discharge, st == bt::RunState::Measuring ? bt::End::Running : lost ? bt::End::Lost : bt::End::Manual, 0,
                               359'999, 1'234'567, 123'456'789, 99'999, 99'999'000};
    return r;
}

// The real runs of 2026-11-24/25 as kept after id 5 (two charges and the discharge before them: the pair 84.0 %).
static bt::Record real_record(std::uint16_t id, bt::Mode m, std::uint32_t secs, std::uint32_t uwh, std::uint32_t uah) {
    bt::Record r;
    r.mode = m;
    r.wiring = m == bt::Mode::Discharge ? bt::Wiring::Moved : bt::Wiring::SamePort;
    r.state = bt::RunState::Done;
    r.id = id;
    r.phases = 1;
    const bool dis = m == bt::Mode::Discharge;
    r.ph[0] = bt::PhaseRec{dis ? bt::Kind::Discharge : bt::Kind::Charge, bt::End::Auto, 0, secs, dis ? 0 : uwh, dis ? uwh : 0, dis ? 0 : uah, dis ? uah : 0};
    return r;
}

static void walk_page(Ui &ui, const BatteryPage *p, std::uint32_t &t, const char *name, int rot) {
    char nm[64];
    for (int i = 0; i < 20 && p->selected() > 0; i++) press(ui, Button::Minus, t);
    for (int i = 0;; i++) {
        std::snprintf(nm, sizeof nm, "%s_%02d", name, i);
        dump(nm, rot);
        if (p->selected() >= p->list_state().count - 1 || i >= 20) break;
        press(ui, Button::Plus, t);
    }
}

// Every row of the main, Saved runs and stored views focused, in all four rotations, with long values and with the real runs.
// Nothing may be cut (test_report fails on a cut).
static void walk_set(const bt::Record *runs, int n, const bt::Record &live, const char *set) {
    for (int rot = 0; rot < 4; rot++) {
        Ui &ui = fresh_ui();
        ui.set_rotation(std::uint8_t(rot), g_stub.ms);
        std::uint32_t t = 1000;
        ui.tick(t);
        fs::Fs *f = bt::hw::service().volume();
        CHECK(f != nullptr);
        if (!f) return;
        for (int i = 0; i < n; i++) CHECK(bt::store::save(*f, runs[i]));
        bt::hw::service().rec().restore(live);
        open_settled(ui, bench::Place::Battery, t);
        const BatteryPage *p = ui.battery_page();
        CHECK(p != nullptr);
        if (!p) return;
        char nm[48];
        std::snprintf(nm, sizeof nm, "%s_main", set);
        walk_page(ui, p, t, nm, rot);
        if (!find_row(ui, p, view::Act::Runs, t)) continue;
        press(ui, Button::Ok, t);
        std::snprintf(nm, sizeof nm, "%s_runs", set);
        walk_page(ui, p, t, nm, rot);
        for (int k = 0; k < n; k++) {
            for (int i = 0; i < 5 && p->selected() > 0; i++) press(ui, Button::Minus, t);
            for (int i = 0; i < k; i++) press(ui, Button::Plus, t);
            press(ui, Button::Ok, t);
            CHECK(p->viewing_stored());
            std::snprintf(nm, sizeof nm, "%s_stored%d", set, k);
            walk_page(ui, p, t, nm, rot);
            press(ui, Button::Back, t);
        }
        press(ui, Button::Back, t);
        press(ui, Button::Back, t);
    }
    stub_battery_reset();
}

// The identity line is painted through fit_data (counted, not failed): its longest texts are checked here instead.
static void test_long_ident_lines() {
    const bt::RunState st[] = {bt::RunState::Measuring, bt::RunState::Between, bt::RunState::Done};
    for (bt::Mode m : {bt::Mode::Charge, bt::Mode::Discharge, bt::Mode::RoundTrip})
        for (bt::RunState s : st)
            for (bool lost : {false, true}) {
                kit::Ident id;
                view::ident(view::shown(long_record(999, m, lost, s), true), id);
                if (id.line.view().size() > kit::kIdLineCells) std::printf("  identity line \"%s\" too long\n", id.line.c_str());
                CHECK(id.line.view().size() <= kit::kIdLineCells);
            }
    kit::Ident id;
    view::ident(view::shown(long_record(999, bt::Mode::Discharge, false, bt::RunState::Measuring), true), id);
    CHECK(eq(id.line.view(), "Same port, 99h59m59s, 123.4Wh"));
}

static void test_long_values() {
    const bt::Record longs[] = {long_record(997, bt::Mode::Charge, true), long_record(998, bt::Mode::Discharge, false),
                                long_record(999, bt::Mode::RoundTrip, false)};
    walk_set(longs, 3, long_record(999, bt::Mode::RoundTrip, false), "long");
    walk_set(longs, 3, long_record(999, bt::Mode::Discharge, false, bt::RunState::Measuring), "long_meas");
    walk_set(longs, 3, long_record(999, bt::Mode::RoundTrip, false, bt::RunState::Measuring), "long_trip");
    walk_set(longs, 3, long_record(999, bt::Mode::RoundTrip, false, bt::RunState::Between), "long_between");
    walk_set(longs, 3, long_record(999, bt::Mode::RoundTrip, true), "long_lost");
    const bt::Record real[] = {real_record(3, bt::Mode::Discharge, 3170, 30'994'272, 1'538'604),
                               real_record(4, bt::Mode::Charge, 3006, 23'804'464, 1'205'674),
                               real_record(5, bt::Mode::Charge, 1592, 13'075'722, 673'138)};
    walk_set(real, 3, real[2], "real");
}

int main() {
    test_numbers();
    test_rows();
    test_ident_and_footer();
    test_page();
    test_continue_page();
    test_continue_never_under_the_cursor();
    test_rows_have_no_marker();
    test_classic();
    test_long_ident_lines();
    test_long_values();
    return test_report("test_ui_battery");
}
