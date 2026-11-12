// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "test_util.h"
#include "k2/ui/ui_trigger.hpp"
#include "k2/ui/ui_fc_release.hpp"
#include <string>

using namespace k2;
using namespace k2::trig;

static Model::Req event(Model &m, Button b, Press p = Press::Short, Run run = Run::Idle, uint32_t now = 100) {
    Model::Req r;
    m.on_event({b, p}, run, now, r);
    return r;
}
static void select(Model &m, int index) {
    while (m.sel() < index) event(m, Button::Plus);
    while (m.sel() > index) event(m, Button::Minus);
}
static void review(Model &m) {
    event(m, Button::Ok);
    if (m.stage() == Stage::Edit) event(m, Button::Ok);
    CHECK_EQ(int(m.stage()), int(Stage::Review));
}
static void test_pick_edit() {
    Model m;
    CHECK_EQ(m.sel(), 1);
    CHECK_EQ(m.target_mv(), 9000);
    event(m, Button::Plus, Press::Long);
    CHECK_EQ(m.sel(), 1);                         // no hidden protocol hold
    select(m, 2);                                // QC3
    CHECK_STR(entry(m.sel()).name, "QC3");
    CHECK_EQ(int(event(m, Button::Ok).kind), int(Model::Req::None));
    CHECK_EQ(int(m.stage()), int(Stage::Edit));
    event(m, Button::Plus);
    CHECK_EQ(m.target_mv(), 9200);
    event(m, Button::Plus, Press::Long);
    CHECK_EQ(m.target_mv(), 10200);
    event(m, Button::Plus, Press::Hold);
    CHECK_EQ(m.target_mv(), 10200);               // holding produces no repeat
    event(m, Button::Minus, Press::Long);
    CHECK_EQ(m.target_mv(), 9200);
    for (int n = 0; n < 50; ++n) event(m, Button::Minus);
    CHECK_EQ(m.target_mv(), 3600);
    for (int n = 0; n < 100; ++n) event(m, Button::Plus);
    CHECK_EQ(m.target_mv(), 20000);
    m.set_ceiling(9100);
    CHECK_EQ(m.target_mv(), 9000);               // quantized cap
    Model plus; select(plus, 3); event(plus, Button::Ok);
    for (int n = 0; n < 20; ++n) event(plus, Button::Plus, Press::Long);
    CHECK_EQ(plus.target_mv(), 12000);           // QC3+ remains Class-A capped
    Model class_b; select(class_b, 2); event(class_b, Button::Ok);
    for (int n = 0; n < 20; ++n) event(class_b, Button::Plus, Press::Long);
    event(class_b, Button::Ok);
    CHECK_EQ(int(event(class_b, Button::Ok).kind), int(Model::Req::None));
    const auto high = event(class_b, Button::Ok);
    CHECK_EQ(high.mv, 20000);
    CHECK_EQ(high.confirmed, true);
    event(m, Button::Back);
    CHECK_EQ(int(m.stage()), int(Stage::Pick));
    select(m, 0);
    event(m, Button::Minus);
    CHECK_EQ(m.sel(), 0);
    select(m, kEntries - 1);
    event(m, Button::Plus);
    CHECK_EQ(m.sel(), kEntries - 1);              // clamped browsing
}
static void test_levels_fixed() {
    for (int i = 1; i < kEntries; ++i) {
        Model m; select(m, i); event(m, Button::Ok);
        if (entry(i).kind == Kind::Fixed) {
            CHECK_EQ(int(m.stage()), int(Stage::Review));
            CHECK_EQ(m.target_mv(), 5000);
            continue;
        }
        const int before = m.target_mv();
        event(m, Button::Plus, Press::Long);
        const auto &e = entry(i);
        if (e.kind == Kind::Range)
            CHECK_EQ(m.target_mv(), before + (e.step < 1000 ? 1000 : e.step));
        else CHECK_EQ(m.target_mv(), e.mv[e.proto == fc::Proto::Fcp ? 2 : 1 + (before == 9000 && e.n == 4)]);
    }
    Model m; event(m, Button::Ok); event(m, Button::Minus);
    CHECK_EQ(m.target_mv(), 5000);
    event(m, Button::Ok);
    const auto r = event(m, Button::Ok);
    CHECK_EQ(int(r.kind), int(Model::Req::Start));
    CHECK_EQ(r.confirmed, false);
    Model a; select(a, 4); a.set_ceiling(5000); review(a);
    CHECK_EQ(a.available(), false); // AFC has no 5V offered level
    CHECK_EQ(int(event(a, Button::Ok).kind), int(Model::Req::None));
}
static void test_confirmation() {
    Model m; m.session(42); review(m);
    CHECK_EQ(int(event(m, Button::Ok, Press::Long).kind), int(Model::Req::None));
    CHECK_EQ(m.armed(), false);
    event(m, Button::Ok, Press::Short, Run::Idle, 200);
    CHECK_EQ(m.armed(), true);
    event(m, Button::Plus);                      // unused review key invalidates arm
    CHECK_EQ(m.armed(), false);
    event(m, Button::Ok, Press::Short, Run::Idle, 300);
    m.session(43);
    CHECK_EQ(m.armed(), false);
    CHECK_EQ(int(event(m, Button::Ok, Press::Short, Run::Idle, 400).kind), int(Model::Req::None));
    const auto r = event(m, Button::Ok, Press::Short, Run::Idle, 500);
    CHECK_EQ(int(r.kind), int(Model::Req::Start));
    CHECK_EQ(r.mv, 9000);
    CHECK_EQ(r.confirmed, true);
    event(m, Button::Ok, Press::Short, Run::Idle, 600);
    CHECK_EQ(int(event(m, Button::Ok, Press::Short, Run::Idle, 3600).kind), int(Model::Req::None));
    CHECK_EQ(m.armed(), true);                   // expiration re-arms, never sends
    m.set_ceiling(8000);
    CHECK_EQ(m.armed(), false);
    CHECK_EQ(m.target_mv(), 5000);
    m.invalidate_confirmation();                // rotation chord/disconnect/refusal
}
static void test_retarget_stop() {
    Model m; select(m, 2); review(m);
    event(m, Button::Ok);
    event(m, Button::Ok);
    m.applied();
    CHECK_EQ(m.held_mv(), 9000);
    event(m, Button::Plus, Press::Long, Run::Live);
    CHECK_EQ(m.target_mv(), 9000);
    CHECK_EQ(int(m.stage()), int(Stage::Live));
    CHECK_EQ(int(event(m, Button::Plus, Press::Short, Run::Live).kind), int(Model::Req::None));
    CHECK_EQ(m.target_mv(), 9200);
    CHECK_EQ(m.held_mv(), 9000);
    CHECK_EQ(m.retargeting(), true);
    event(m, Button::Ok, Press::Short, Run::Live); // review only
    CHECK_EQ(int(event(m, Button::Ok, Press::Short, Run::Live).kind), int(Model::Req::None));
    const auto r = event(m, Button::Ok, Press::Short, Run::Live);
    CHECK_EQ(int(r.kind), int(Model::Req::Retarget));
    CHECK_EQ(r.mv, 9200); CHECK_EQ(r.confirmed, true);
    m.applied();
    event(m, Button::Minus, Press::Short, Run::Live);
    CHECK_EQ(m.target_mv(), 9000); CHECK_EQ(m.held_mv(), 9200);
    event(m, Button::Ok, Press::Short, Run::Live);
    CHECK_EQ(int(event(m, Button::Ok, Press::Short, Run::Live).kind), int(Model::Req::None));
    CHECK_EQ(int(event(m, Button::Ok, Press::Short, Run::Live).kind), int(Model::Req::Retarget));
    CHECK_EQ(int(event(m, Button::Back, Press::Long, Run::Live).kind), int(Model::Req::Stop));
    CHECK_EQ(int(m.stage()), int(Stage::Pick));
    Model::Req out;
    CHECK_EQ(m.on_event({Button::Back, Press::Short}, Run::Idle, 0, out), false);
    for (auto run : {Run::Scan, Run::Negotiating}) {
        CHECK_EQ(int(event(m, Button::Plus, Press::Long, run).kind), int(Model::Req::None));
        CHECK_EQ(int(event(m, Button::Back, Press::Short, run).kind), int(Model::Req::Stop));
        CHECK_EQ(int(event(m, Button::Ok, Press::Short, run).kind), int(Model::Req::None));   // OK is free: BACK reads Stop
    }
}
static void test_scan_labels() {
    Model m; select(m, 0); review(m);
    CHECK_EQ(int(event(m, Button::Ok).kind), int(Model::Req::None));
    CHECK_EQ(int(event(m, Button::Ok).kind), int(Model::Req::Scan));
    for (int i = 0; i < kEntries; ++i) {
        Model q; select(q, i);
        for (int s = 0; s < 3; ++s) {
            for (auto pair : {std::pair{Button::Plus, plus_label(q)}, std::pair{Button::Minus, minus_label(q)},
                              std::pair{Button::Ok, ok_label(q, Run::Idle)}}) {
                char text[24];
                label_text(text, pair.second, label_max_chars(pair.first));
                CHECK_EQ(std::string(text).size() <= label_max_chars(pair.first), true);
            }
            event(q, Button::Ok);
        }
    }
}
static void test_release() {
    for (int mv : {4750, 5269, 5500}) {
        Release safe; safe.start(0);
        for (uint32_t t = 100; t <= 600; t += 100) safe.poll(t, mv, true);
        CHECK_EQ(int(safe.state()), int(Release::State::Safe));
    }
    for (int mv : {4749, 5501}) {
        Release unsafe; unsafe.start(0);
        for (uint32_t t = 100; t <= 600; t += 100) unsafe.poll(t, mv, true);
        CHECK_EQ(unsafe.busy(), true);
    }
    Release r; r.start(0);
    r.poll(100, 9000, true);
    CHECK_EQ(r.busy(), true);
    r.poll(200, 5000, true);
    r.poll(400, 5000, true);
    r.poll(700, 5000, true);                     // sampling gap resets dwell
    CHECK_EQ(r.busy(), true);
    r.poll(900, 5000, true); r.poll(1100, 5000, true); r.poll(1200, 5000, true);
    CHECK_EQ(int(r.state()), int(Release::State::Safe));
    r.start(2000);
    r.poll(2100, 5000, false);
    r.start(9000);                              // repeated cancel preserves timeout
    r.poll(10000, -1, true);
    CHECK_EQ(int(r.state()), int(Release::State::Unsafe));
    CHECK_EQ(r.busy(), true);
    for (uint32_t t = 10100; t <= 10600; t += 100) r.poll(t, 5250, true);
    CHECK_EQ(int(r.state()), int(Release::State::Safe));
    r.start(11000);
    for (uint32_t t = 11100; t <= 12000; t += 100) r.poll(t, 4749, true);
    CHECK_EQ(r.busy(), true);
}
static void test_widgets() {
    CHECK_EQ(volt_fill_px(9000, 64), 28);
    CHECK_EQ(trace_level(12000, 72), 42);
    CHECK_EQ(trace_level(20000, 72), 71);
    CHECK_EQ(trace_level(25000, 72), 71);
    fc::ScanResult r[] = {{fc::Proto::Qc2, fc::Outcome::Accepted, 9020},
                         {fc::Proto::Qc3, fc::Outcome::Accepted, 5960}};
    CHECK_EQ(scan_accepted(r, 2), 2); CHECK_EQ(scan_max_mv(r, 2), 9000);
    CHECK_STR(fail_long(fc::Fail::Inline), "Load/PD active: refused");
}
int main() {
    test_pick_edit(); test_levels_fixed(); test_confirmation(); test_retarget_stop();
    test_scan_labels(); test_release(); test_widgets();
    return test_report("test_ui_trigger");
}
