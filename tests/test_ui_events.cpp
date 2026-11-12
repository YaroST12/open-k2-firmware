// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the input event layer: ButtonEvent -> Event, then global handler first, page second.
#include "test_util.h"
#include "k2/ui/ui_events.hpp"

using namespace k2;

// A page stand-in: consumes exactly the events it is told to and records what it saw.
struct FakePage {
    bool consume_short_ok = false, consume_short_plus = false;
    bool swallow_plus_minus = false;   // the charger test's Waiting stage: + / - in any press length are the page's, not navigation
    int seen = 0;
    bool on_event(Event e) {
        seen++;
        if (swallow_plus_minus && (e.button == Button::Plus || e.button == Button::Minus)) return true;
        if (e.press == Press::Short && e.button == Button::Ok) return consume_short_ok;
        if (e.press == Press::Short && e.button == Button::Plus) return consume_short_plus;
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

static void test_global_first() {
    FakePage p;
    // Long + / - page; the page never sees them.
    auto r = dispatch({Button::Plus, Press::Long}, 1, 4, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Global));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::PageNext));
    r = dispatch({Button::Minus, Press::Long}, 1, 4, p);
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::PagePrev));
    CHECK_EQ(p.seen, 0);
    // The 5 s BACK hold is no gesture any more (the reboot is the OK + Back chord, in the Ui): nothing global, and the page
    // never sees it either, so a page's long BACK action cannot fire a second time.
    r = dispatch({Button::Back, Press::Hold}, 0, 4, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    r = dispatch({Button::Back, Press::Hold}, 2, 4, p);
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    CHECK_EQ(p.seen, 0);
    r = dispatch({Button::Back, Press::Long}, 0, 4, p);
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    // Holding any other button does nothing globally.
    r = dispatch({Button::Plus, Press::Hold}, 1, 4, p);
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
}

static void test_no_paging_past_the_ends() {
    FakePage p;
    // Not wrapping: the global handler does not act, so the page is asked and declines.
    auto r = dispatch({Button::Minus, Press::Long}, 0, 4, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    r = dispatch({Button::Plus, Press::Long}, 3, 4, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    CHECK_EQ(p.seen, 2);
}

static void test_page_consumption() {
    FakePage p;
    p.consume_short_ok = true;
    auto r = dispatch({Button::Ok, Press::Short}, 1, 4, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    // Unconsumed short +/- has no fallback paging.
    r = dispatch({Button::Plus, Press::Short}, 1, 4, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    r = dispatch({Button::Minus, Press::Short}, 1, 4, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    p.consume_short_plus = true;
    r = dispatch({Button::Plus, Press::Short}, 1, 4, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
}

// Modal routing (a submenu is open): no paging, the page sees everything first, short BACK pops.
static void test_modal() {
    FakePage p;
    p.consume_short_plus = true;
    auto r = dispatch_modal({Button::Plus, Press::Long}, p);       // paging is off: the menu sees it
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    CHECK_EQ(p.seen, 1);
    r = dispatch_modal({Button::Minus, Press::Long}, p);
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    CHECK_EQ(p.seen, 2);
    r = dispatch_modal({Button::Plus, Press::Short}, p);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
    r = dispatch_modal({Button::Back, Press::Short}, p);           // declined BACK pops the menu
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Pop));
    r = dispatch_modal({Button::Back, Press::Long}, p);            // a 500 ms BACK is no gesture
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    const int seen = p.seen;
    r = dispatch_modal({Button::Back, Press::Hold}, p);            // no gesture, and the menu is not asked
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    CHECK_EQ(p.seen, seen);
}

// A page that locks paging (the Waiting stage: the operator holds + to wake a charger): long + / - reach the page instead of
// paging; the BACK hold stays no gesture, and an unlocked page pages as before.
static void test_paging_lock() {
    FakePage p;
    p.swallow_plus_minus = true;
    auto r = dispatch({Button::Plus, Press::Long}, 1, 4, p, true);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::None));
    r = dispatch({Button::Minus, Press::Long}, 1, 4, p, true);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
    r = dispatch({Button::Plus, Press::Hold}, 1, 4, p, true);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Page));
    CHECK_EQ(p.seen, 3);
    r = dispatch({Button::Back, Press::Hold}, 1, 4, p, true);
    CHECK_EQ(static_cast<int>(r.route), static_cast<int>(Route::Unhandled));
    CHECK_EQ(p.seen, 3);
    r = dispatch({Button::Plus, Press::Long}, 1, 4, p, false);   // unlocked: the global handler first, as ever
    CHECK_EQ(static_cast<int>(r.global), static_cast<int>(Global::PageNext));
    CHECK_EQ(p.seen, 3);
}

int main() {
    test_paging_lock();
    test_modal();
    test_conversion();
    test_global_first();
    test_no_paging_past_the_ends();
    test_page_consumption();
    return test_report("test_ui_events");
}
