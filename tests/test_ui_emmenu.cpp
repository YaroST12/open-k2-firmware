// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_emmenu.hpp"
#include "test_util.h"

using namespace k2;

static Event S(Button b) { return {b, Press::Short}; }

static void test_flow() {
    EmMenu m; EmCmd c;
    CHECK_EQ(int(m.view), int(EmView::List));         // the page opens on the list
    m.set_count(3);
    CHECK_EQ(m.on_event(S(Button::Back), c) ? 1 : 0, 0);   // BACK in the list leaves the page
    m.on_event(S(Button::Plus), c); CHECK_EQ(m.idx, 1);
    m.on_event(S(Button::Minus), c); m.on_event(S(Button::Minus), c); CHECK_EQ(m.idx, 2);   // wraps
    m.on_event(S(Button::Ok), c); CHECK_EQ(int(m.view), int(EmView::Detail));
    m.on_event(S(Button::Plus), c); CHECK_EQ(m.idx, 0);                  // next cable in the detail
    m.on_event(S(Button::Ok), c);                     // delete asks first
    CHECK_EQ(int(c), int(EmCmd::None)); CHECK_EQ(int(m.view), int(EmView::Confirm));
    m.on_event(S(Button::Back), c);                   // keep it
    CHECK_EQ(int(c), int(EmCmd::None)); CHECK_EQ(int(m.view), int(EmView::Detail));
    m.on_event(S(Button::Back), c); CHECK_EQ(int(m.view), int(EmView::List));
    m.on_event(S(Button::Ok), c);
    m.on_event(S(Button::Ok), c);
    m.on_event(S(Button::Ok), c);                     // confirm
    CHECK_EQ(int(c), int(EmCmd::Delete)); CHECK_EQ(int(m.view), int(EmView::List));
    CHECK_EQ(m.on_event({Button::Ok, Press::Long}, c) ? 1 : 0, 0);
}

static void test_count_changes() {
    EmMenu m; EmCmd c;
    m.set_count(2);
    m.on_event(S(Button::Plus), c);                   // idx 1 (last)
    m.on_event(S(Button::Ok), c);                     // detail
    m.set_count(1);                                   // the shown cable was deleted elsewhere
    CHECK_EQ(m.idx, 0); CHECK_EQ(int(m.view), int(EmView::Detail));
    m.set_count(0);
    CHECK_EQ(int(m.view), int(EmView::List));
    m.on_event(S(Button::Ok), c);                     // empty list: OK does nothing
    CHECK_EQ(int(m.view), int(EmView::List));
    m.on_event(S(Button::Plus), c); CHECK_EQ(m.idx, 0);
}

int main() {
    test_flow();
    test_count_changes();
    return test_report("test_ui_emmenu");
}
