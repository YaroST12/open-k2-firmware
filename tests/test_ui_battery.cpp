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
    CHECK_EQ(int(view::footer(view::shown(record_of(r), false), true, false, false, f)), int(FooterTone::Info));
    f.clear();
    CHECK_EQ(int(view::footer(view::shown(record_of(r), false), false, false, false, f)), int(FooterTone::Error));
    CHECK(eq(f.view(), "No power monitor"));
    r.start(bt::Mode::Charge, bt::Wiring::SamePort, bt::Config{}, 4);
    feed(r, 0, 3'912'000, 5000, 1000);
    view::Shown s = view::shown(record_of(r), true);
    id = kit::Ident{};
    view::ident(s, id);
    CHECK(eq(id.head.view(), "Charging"));
    CHECK(eq(id.line.view(), "Same port, 1h05m11s, 5.431Wh in"));   // 5 W for 3911 s = 5.4319 Wh, cut
    f.clear();
    CHECK_EQ(int(view::footer(s, true, false, false, f)), int(FooterTone::Info));
    CHECK(eq(f.view(), "Ends under 50mA for 60s"));
    f.clear();
    CHECK_EQ(int(view::footer(s, true, true, false, f)), int(FooterTone::Caution));   // the last write failed
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
    CHECK_EQ(int(view::footer(s, true, false, false, f)), int(FooterTone::Ok));
    CHECK(eq(f.view(), "Round trip 80.0% efficient"));
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

// The place in Classic (the only theme): the identity's dashed rule in ink3, nothing left of the page under it from the Bench.
static void test_classic() {
    Ui &ui = fresh_ui();
    std::uint32_t t = 1000;
    ui.tick(t);
    bt::hw::service().start(bt::Mode::RoundTrip, bt::Wiring::SamePort, true);
    for (int i = 0; i < 30; i++) bt::hw::poll(t += 1000, true);
    open_settled(ui, bench::Place::Battery, t);
    CHECK(count_px(kInk3, 8, 66, 216, 67) > 100);   // the perforation under the identity block (y kIdDashY)
    CHECK(count_px(kBg, 0, 22, 224, 220) > 224 * 100);
}

int main() {
    test_numbers();
    test_rows();
    test_ident_and_footer();
    test_page();
    test_classic();
    return test_report("test_ui_battery");
}
