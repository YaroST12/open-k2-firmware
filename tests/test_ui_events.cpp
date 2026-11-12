// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the input event layer: ButtonEvent -> Event, then the page first and the global fallback second
// (Fieldbook: no paging press; a long BACK nobody takes goes straight to the Meter).
#include "test_util.h"
#include "k2/ui/ui_events.hpp"

using namespace k2;

// A page stand-in: consumes exactly the events it is told to and records what it saw.
struct FakePage {
    bool consume_short_ok = false, consume_short_plus = false, consume_long_back = false;
    bool swallow_plus_minus = false;   // the charger test's Waiting stage: + / - in any press length are the page's
    int seen = 0;
    bool on_event(Event e) {
        seen++;
        if (swallow_plus_minus && (e.button == Button::Plus || e.button == Button::Minus)) return true;
        if (e.press == Press::Short && e.button == Button::Ok) return consume_short_ok;
        if (e.press == Press::Short && e.button == Button::Plus) return consume_short_plus;
        if (e.press == Press::Long && e.button == Button::Back) return consume_long_back;
        return false;
    }
};

static void test_conversion() {
    CHECK_EQ(to_event(Button::Ok, ButtonEvent::None).has_value(), 0);
    auto s = to_event(Button::Minus, ButtonEvent::Short);
    CHECK_EQ(s.has_value(), 1);
    CHECK_EQ(static_cast<int>(s->button), static_cast<int>(Button::Minus));
    CHECK_EQ(static_cast<int>(s->press), static_cast<int>(Press::Short));
    auto l = to_event(Button::Back, ButtonEvent::Long);
    CHECK_EQ(static_cast<int>(l->press), static_cast<int>(Press::Long));
    auto h = to_event(Button::Back, ButtonEvent::Hold);
    CHECK_EQ(static_cast<int>(h->press), static_cast<int>(Press::Hold));
}

// Long + / - are no paging presses any more: the page gets them (Flow's trend span, a value page's coarse step).
static void test_long_plus_minus_reach_the_page() {
    FakePage p;
    auto r = dispatch({Button::Plus, Press::Long}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    r = dispatch({Button::Minus, Press::Long}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    CHECK_EQ(p.seen, 2);
    p.swallow_plus_minus = true;
    r = dispatch({Button::Plus, Press::Long}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
    CHECK_EQ(p.seen, 3);
}

// A long BACK the page leaves goes to the Meter; a page that takes it (Flow: the hero cycle) keeps it.
static void test_long_back_to_the_meter() {
    FakePage p;
    auto r = dispatch({Button::Back, Press::Long}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Global));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::ToMeter));
    CHECK_EQ(p.seen, 1);
    p.consume_long_back = true;
    r = dispatch({Button::Back, Press::Long}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    // The 5 s BACK hold is no gesture (the reboot is the OK + Back chord, in the Ui): nothing global, and the page never
    // sees it, so a page's long BACK action cannot fire a second time.
    const int seen = p.seen;
    r = dispatch({Button::Back, Press::Hold}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    CHECK_EQ(p.seen, seen);
    r = dispatch({Button::Plus, Press::Hold}, p);   // holding any other button does nothing globally
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
}

static void test_page_consumption() {
    FakePage p;
    p.consume_short_ok = true;
    auto r = dispatch({Button::Ok, Press::Short}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    r = dispatch({Button::Plus, Press::Short}, p);   // unconsumed short +/- has no fallback
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    r = dispatch({Button::Minus, Press::Short}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    r = dispatch({Button::Back, Press::Short}, p);   // a short BACK a page declines: nothing global (the Ui steps up a level)
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    p.consume_short_plus = true;
    r = dispatch({Button::Plus, Press::Short}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
}

// Modal routing (a submenu is open): the menu sees everything first, a short BACK it declines pops it, a long BACK it
// declines goes to the Meter.
static void test_modal() {
    FakePage p;
    p.consume_short_plus = true;
    auto r = dispatch_modal({Button::Plus, Press::Long}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    CHECK_EQ(p.seen, 1);
    r = dispatch_modal({Button::Plus, Press::Short}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
    r = dispatch_modal({Button::Back, Press::Short}, p);           // declined BACK pops the menu
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Pop));
    r = dispatch_modal({Button::Back, Press::Long}, p);            // declined long BACK: straight to the Meter
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Global));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::ToMeter));
    const int seen = p.seen;
    r = dispatch_modal({Button::Back, Press::Hold}, p);            // no gesture, and the menu is not asked
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    CHECK_EQ(p.seen, seen);
}

int main() {
    test_modal();
    test_conversion();
    test_long_plus_minus_reach_the_page();
    test_long_back_to_the_meter();
    test_page_consumption();
    return test_report("test_ui_events");
}
