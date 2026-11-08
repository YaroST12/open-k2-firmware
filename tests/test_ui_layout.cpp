// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the label-panel / content-area geometry.
#include <initializer_list>
#include "test_util.h"
#include "k2/ui/ui_layout.hpp"
#include "k2/ui/ui_style.hpp"

using namespace k2;

static const int16_t kCenters[4] = {30, 90, 150, 210};

static void check_rect(Rect r, int x, int y, int w, int h) {
    CHECK_EQ(r.x, x); CHECK_EQ(r.y, y); CHECK_EQ(r.w, w); CHECK_EQ(r.h, h);
}

static void test_right() {
    Layout l = compute_layout(PanelEdge::Right, kCenters, 60, 30);
    check_rect(l.panel, 210, 0, 30, 240);
    check_rect(l.content, 0, 0, 210, 240);
    check_rect(l.slot[0], 210, 0, 30, 60);
    check_rect(l.slot[1], 210, 60, 30, 60);
    check_rect(l.slot[2], 210, 120, 30, 60);
    check_rect(l.slot[3], 210, 180, 30, 60);
}

static void test_left() {
    Layout l = compute_layout(PanelEdge::Left, kCenters, 60, 30);
    check_rect(l.panel, 0, 0, 30, 240);
    check_rect(l.content, 30, 0, 210, 240);
    check_rect(l.slot[2], 0, 120, 30, 60);
}

static void test_top_bottom() {
    Layout t = compute_layout(PanelEdge::Top, kCenters, 60, 30);
    check_rect(t.panel, 0, 0, 240, 30);
    check_rect(t.content, 0, 30, 240, 210);
    check_rect(t.slot[1], 60, 0, 60, 30);
    Layout b = compute_layout(PanelEdge::Bottom, kCenters, 60, 30);
    check_rect(b.panel, 0, 210, 240, 30);
    check_rect(b.content, 0, 0, 240, 210);
    check_rect(b.slot[3], 180, 210, 60, 30);
}

static void test_centres_are_tunable_and_clamped() {
    // Slots follow the centres; a slot that would leave the screen is cut at the edge.
    const int16_t c[4] = {10, 100, 160, 235};
    Layout l = compute_layout(PanelEdge::Right, c, 60, 30);
    check_rect(l.slot[0], 210, 0, 30, 40);      // -20..40 cut to 0..40
    check_rect(l.slot[1], 210, 70, 30, 60);
    check_rect(l.slot[3], 210, 205, 30, 35);    // 205..265 cut to 205..240
}

static void test_unequal_slots_tile_the_edge() {
    // The shipped slot table: wide outer slots, narrow inner ones, no gaps or overlaps.
    const int16_t c[4] = {33, 93, 147, 207}, n[4] = {66, 54, 54, 66};
    Layout l = compute_layout(PanelEdge::Right, c, n, 16);
    check_rect(l.slot[0], 224, 0, 16, 66);
    check_rect(l.slot[1], 224, 66, 16, 54);
    check_rect(l.slot[2], 224, 120, 16, 54);
    check_rect(l.slot[3], 224, 174, 16, 66);
}

static void test_shipped_style_tiles() {
    // kSlotCenters are derived from kSlotLens, so the shipped table tiles 0..240 on every edge.
    for (PanelEdge e : {PanelEdge::Right, PanelEdge::Left, PanelEdge::Top, PanelEdge::Bottom}) {
        Layout l = compute_layout(e, kSlotCenters, kSlotLens, kPanelThickness);
        const bool vertical = e == PanelEdge::Right || e == PanelEdge::Left;
        int pos = 0;
        for (int i = 0; i < 4; i++) {
            const Rect &r = l.slot[i];
            CHECK_EQ(vertical ? r.y : r.x, pos);
            pos += vertical ? r.h : r.w;
        }
        CHECK_EQ(pos, 240);
    }
}

static void test_rotation_table() {
    // The boot default must keep the button strip on the physical button edge: the RIGHT edge of
    // the board at MADCTL 0x00 (verified on hardware), i.e. consistent with orientation_for().
    CHECK_EQ(static_cast<int>(kDefaultOrientation.edge), static_cast<int>(PanelEdge::Right));
    CHECK_EQ(kDefaultOrientation.rotation, 0);
    CHECK_EQ(static_cast<int>(kDefaultOrientation.edge),
             static_cast<int>(orientation_for(kDefaultOrientation.rotation).edge));
    // Slot order along the right edge, top to bottom: BACK, MINUS, OK, PLUS.
    CHECK_EQ(static_cast<int>(slot_button(kDefaultOrientation, 0)), static_cast<int>(Button::Back));
    CHECK_EQ(static_cast<int>(slot_button(kDefaultOrientation, 1)), static_cast<int>(Button::Minus));
    CHECK_EQ(static_cast<int>(slot_button(kDefaultOrientation, 2)), static_cast<int>(Button::Ok));
    CHECK_EQ(static_cast<int>(slot_button(kDefaultOrientation, 3)), static_cast<int>(Button::Plus));
    const PanelEdge edges[] = {PanelEdge::Right, PanelEdge::Top, PanelEdge::Left, PanelEdge::Bottom};
    const Button buttons[] = {Button::Back, Button::Minus, Button::Ok, Button::Plus};
    for (uint8_t r = 0; r < 4; ++r) {
        const Orientation o = orientation_for(r);
        CHECK_EQ(static_cast<int>(o.edge), static_cast<int>(edges[r]));
        CHECK_EQ(o.rotation, r);
        CHECK_EQ(static_cast<int>(top_port_direction(r)), r);
        for (unsigned slot = 0; slot < 4; ++slot)
            CHECK_EQ(static_cast<int>(slot_button(o, slot)),
                     static_cast<int>(buttons[r < 2 ? slot : 3 - slot]));
    }
    CHECK_EQ(orientation_for(4).rotation, 0);
}

static void test_thin_panel_gives_wider_content() {
    Layout l = compute_layout(PanelEdge::Right, kCenters, 60, 20);
    check_rect(l.panel, 220, 0, 20, 240);
    check_rect(l.content, 0, 0, 220, 240);
    check_rect(l.slot[3], 220, 180, 20, 60);
}

static void test_text_direction_follows_the_edge() {
    CHECK_EQ(static_cast<int>(panel_text_dir(PanelEdge::Right)), static_cast<int>(TextDir::Ccw));
    CHECK_EQ(static_cast<int>(panel_text_dir(PanelEdge::Left)), static_cast<int>(TextDir::Cw));
    CHECK_EQ(panel_is_column(PanelEdge::Right) && panel_is_column(PanelEdge::Left), 1);
    CHECK_EQ(panel_is_column(PanelEdge::Top) || panel_is_column(PanelEdge::Bottom), 0);
}

int main() {
    test_unequal_slots_tile_the_edge();
    test_shipped_style_tiles();
    test_thin_panel_gives_wider_content();
    test_text_direction_follows_the_edge();
    test_right();
    test_left();
    test_top_bottom();
    test_centres_are_tunable_and_clamped();
    test_rotation_table();
    return test_report("test_ui_layout");
}
