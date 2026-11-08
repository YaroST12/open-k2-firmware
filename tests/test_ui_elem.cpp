// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the generic element backend (ui_elem.hpp): the Elem signature cache and the
// IconCell. The point of the backend: a paint with an unchanged signature must stream zero
// pixels, and invalidate() must force a repaint.
#include "test_util.h"
#include "fake_lcd.h"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_widgets.hpp"
#include "k2/ui/ui_style.hpp"

using namespace k2;

static void test_elem_sig_cache() {
    Elem e;
    int calls = 0;
    e.paint(42, [&] { calls++; });
    CHECK_EQ(calls, 1);                    // first paint always runs
    e.paint(42, [&] { calls++; });
    CHECK_EQ(calls, 1);                    // same signature: skipped
    e.paint(43, [&] { calls++; });
    CHECK_EQ(calls, 2);                    // changed signature: repaints
    e.invalidate();
    e.paint(43, [&] { calls++; });
    CHECK_EQ(calls, 3);                    // invalidate forces even an unchanged signature
}

static void test_elem_zero_sig() {
    Elem e;                                // a fresh Elem must paint even with sig 0
    int calls = 0;
    e.paint(0, [&] { calls++; });
    CHECK_EQ(calls, 1);
    e.paint(0, [&] { calls++; });
    CHECK_EQ(calls, 1);
}

static void test_hash_helpers() {
    CHECK_EQ(mix_u32(0u, 0u), 0u);
    CHECK_EQ(mix(mix_init(), "abc") == mix(mix_init(), "abd"), 0);   // different text, different hash
    CHECK_EQ(mix(mix_init(), "abc"), mix(mix_init(), "abc"));         // deterministic
}

static void test_icon_cell() {
    Surface s{0, 0, 240, 240};
    IconCell cell;
    cell.init(s, 8, 30);
    g_lcd_pixels = 0;
    cell.draw(Icon::Bolt, kOk, kBg);
    const unsigned first = g_lcd_pixels;
    CHECK_EQ(first > 0u, 1);
    g_lcd_pixels = 0;
    cell.draw(Icon::Bolt, kOk, kBg);       // unchanged: no pixels
    CHECK_EQ(g_lcd_pixels, 0u);
    cell.draw(Icon::Bolt, kOk, kFocusT);   // bg changed: repaints
    CHECK_EQ(g_lcd_pixels > 0u, 1);
    g_lcd_pixels = 0;
    cell.draw(Icon::Warn, kOk, kFocusT);   // icon changed: repaints
    CHECK_EQ(g_lcd_pixels > 0u, 1);
    g_lcd_pixels = 0;
    cell.invalidate();
    cell.draw(Icon::Warn, kOk, kFocusT);   // invalidated: repaints despite same sig
    CHECK_EQ(g_lcd_pixels > 0u, 1);
}

static void test_footer_cached_elem() {
    Surface s{0, 0, 240, 240};
    Elem e;
    g_lcd_pixels = 0;
    draw_footer_cached(s, FooterTone::Ok, "hello", e);
    const unsigned first = g_lcd_pixels;
    CHECK_EQ(first > 0u, 1);
    g_lcd_pixels = 0;
    draw_footer_cached(s, FooterTone::Ok, "hello", e);
    CHECK_EQ(g_lcd_pixels, 0u);
    draw_footer_cached(s, FooterTone::Ok, "hello!", e);
    CHECK_EQ(g_lcd_pixels, first);         // text changed: full band repaint
    g_lcd_pixels = 0;
    draw_footer_cached(s, FooterTone::Ok, "hello!", e);
    CHECK_EQ(g_lcd_pixels, 0u);
    draw_footer_cached(s, FooterTone::Caution, "hello!", e);
    CHECK_EQ(g_lcd_pixels, first);         // tone changed: the fill changed, so repaint
    g_lcd_pixels = 0;
    e.invalidate();
    draw_footer_cached(s, FooterTone::Caution, "hello!", e);
    CHECK_EQ(g_lcd_pixels, first);
}

int main() {
    test_elem_sig_cache();
    test_elem_zero_sig();
    test_hash_helpers();
    test_icon_cell();
    test_footer_cached_elem();
    return test_report("test_ui_elem");
}
