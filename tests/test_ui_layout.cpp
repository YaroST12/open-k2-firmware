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
    // Asymmetric hardware-aligned slot table: {70, 58, 58, 54} in Button order {Back, Minus, Ok, Plus}.
    // Left and Bottom (rotation >= 2) mirror both centres and slot lengths across 240 px so slots tile 0..240 cleanly.
    const int16_t c[4] = {35, 99, 157, 213}, n[4] = {70, 58, 58, 54};
    const Layout r = compute_layout(PanelEdge::Right, c, n, 16);
    check_rect(r.slot[0], 224, 0, 16, 70);
    check_rect(r.slot[1], 224, 70, 16, 58);
    check_rect(r.slot[2], 224, 128, 16, 58);
    check_rect(r.slot[3], 224, 186, 16, 54);
    const Layout l = compute_layout(PanelEdge::Left, c, n, 16);
    check_rect(l.slot[0], 0, 0, 16, 54);
    check_rect(l.slot[1], 0, 54, 16, 58);
    check_rect(l.slot[2], 0, 112, 16, 58);
    check_rect(l.slot[3], 0, 170, 16, 70);
    const Layout b = compute_layout(PanelEdge::Bottom, c, n, 16);
    check_rect(b.slot[0], 0, 224, 54, 16);
    check_rect(b.slot[1], 54, 224, 58, 16);
    check_rect(b.slot[2], 112, 224, 58, 16);
    check_rect(b.slot[3], 170, 224, 70, 16);
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

// The telemetry rail reads the same in every rotation: the page title first, then the readouts, at fixed lengths. A column
// rail whose text reads bottom-to-top (rotation 0) lays its slots out from the bottom up, so the reading order never flips.
static void test_opp_slots_keep_reading_order() {
    static const int16_t kWant[4] = {0, 54, 116, 178};
    for (uint8_t rot = 0; rot < 4; ++rot) {
        const Orientation o = orientation_for(rot);
        const Layout l = compute_layout(o.edge, kCenters, 60, kOppRailThickness);
        const PanelEdge opp = opposite_edge(o.edge);
        const TextDir dir = panel_text_dir(o.edge);
        const bool column = panel_is_column(opp);
        for (unsigned s = 0; s < 4; ++s) {
            const Rect r = opp_slot_rect(l.opp_rail, opp, dir, s);
            const int along = column ? l.opp_rail.h : l.opp_rail.w;
            const int len = column ? r.h : r.w;
            const int lo = column ? r.y - l.opp_rail.y : r.x - l.opp_rail.x;
            const int u = (column && dir == TextDir::Ccw) ? along - lo - len : lo;   // distance from where reading starts
            CHECK_EQ(len, kOppSlotLen[s]);
            CHECK_EQ(u, kWant[s]);
            CHECK_EQ(column ? r.w : r.h, kOppRailThickness);
            CHECK_EQ(column ? r.x : r.y, column ? l.opp_rail.x : l.opp_rail.y);
        }
    }
}

int main() {
    test_opp_slots_keep_reading_order();
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
