// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// ROADMAP "Home screen fonts stretched or squeezed in landscape": glyphs must render at the same
// pixel size in every rotation. The host panel (g_panel) holds the SCREEN frame of the current
// rotation (fake_lcd's set_rotation is a no-op; the rotation enters through the content layout and
// the ContentViewport stretch), so "the same string in rotation 0 and 1, glyph bitmaps equal after
// rotating by 90 degrees" is checked here as: the tight glyph bitmaps are pixel-identical in every
// rotation (hence also equal after rotating either by 90 degrees, which is what the physical panel
// shows between two orientations 90 degrees apart).
//
// Two renderers are compared:
//  - the Home cell glyph path (home::paint with a text item -- the hero digits' path), and
//  - gfx_text (the already native-size path) as a control,
// plus the real Home page: its hero digits must land on the same native pixels in every rotation.
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_layout.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_viewport.hpp"
#include "k2/ui/gfx.hpp"
#include "k2/ui/home_cell.hpp"
#include "k2/ui/home_model.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/text.hpp"
#include "k2/pd/pd_source.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace k2;

namespace {

constexpr int kHeroX = 38, kHeroTop = 25;            // Home hero digits, logical content coords (ui_home.cpp)
constexpr int kHeroChars = 5;                        // DigitField of 5 chars
constexpr home::CellGeom kHeroCell = home::cell_geom(home::kHeroY0, home::kHeroY1, true);   // the Home hero cell

struct Bitmap { int w = 0, h = 0; std::vector<uint16_t> px; };

// Tight bounding box of everything that is not `bg` on the panel.
Bitmap tight(uint16_t bg) {
    int minx = 240, miny = 240, maxx = -1, maxy = -1;
    for (int y = 0; y < LCD_HEIGHT; y++)
        for (int x = 0; x < LCD_WIDTH; x++)
            if (g_panel[y * LCD_WIDTH + x] != bg) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
    Bitmap b;
    if (maxx < 0) return b;
    b.w = maxx - minx + 1; b.h = maxy - miny + 1;
    b.px.resize(size_t(b.w) * b.h);
    for (int y = 0; y < b.h; y++)
        for (int x = 0; x < b.w; x++)
            b.px[size_t(y) * b.w + x] = g_panel[(miny + y) * LCD_WIDTH + minx + x];
    return b;
}

Bitmap rot90cw(const Bitmap &b) {
    Bitmap r;
    r.w = b.h; r.h = b.w;
    r.px.assign(size_t(r.w) * r.h, 0);
    for (int y = 0; y < b.h; y++)
        for (int x = 0; x < b.w; x++)
            r.px[size_t(x) * r.w + (r.w - 1 - y)] = b.px[size_t(y) * b.w + x];
    return r;
}

bool bitmap_eq(const Bitmap &a, const Bitmap &b, const char *what, int rotation) {
    bool ok = a.w == b.w && a.h == b.h && a.px == b.px;
    if (!ok)
        std::printf("  rotation %d: %s bitmap %dx%d vs rotation 0 %dx%d%s\n", rotation, what,
                    a.w, a.h, b.w, b.h, a.px == b.px ? " (same size, pixels differ)" : "");
    return ok;
}

Layout layout_for(uint8_t rotation) {
    return compute_layout(orientation_for(rotation).edge, kSlotCenters, kSlotLens, kPanelThickness);
}

// The same string through the Home cell glyph path (home::paint, a text item) in one rotation.
void render_home_cell_string(uint8_t rotation, std::string_view s, const Font &f) {
    std::fill(std::begin(g_panel), std::end(g_panel), kBg);
    home::Cell c;
    c.geom = kHeroCell;
    c.bg = kBg;
    c.sweep = false;
    const home::Item items[1] = {home::text(f, kHeroX, kHeroTop, s, kText)};
    c.items = items;
    Layout l = layout_for(rotation);
    {
        ContentViewport viewport(l.content);
        home::paint(c, 0, kHeroCell.top, home::kW - 1, kHeroCell.bottom);
    }
}

// The same string through gfx_text (the native-size path) in one rotation.
void render_gfx_string(uint8_t rotation, std::string_view s, const Font &f) {
    std::fill(std::begin(g_panel), std::end(g_panel), kBg);
    Layout l = layout_for(rotation);
    const Surface logical{0, 0, 224, 240};
    {
        ContentViewport viewport(l.content);
        gfx_text(logical, f, kHeroX, kHeroTop, s, kText, kBg);
    }
}

// The real Home page in one rotation, with the mockup fixture's live 20 V source.
void render_home_page(uint8_t rotation) {
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20040; g_stub.pm.current_ma = 1834; g_stub.pm.power_mw = 36753;
    g_stub.pm.flow = power::Flow::Forward;
    g_stub.caps.valid = true; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2; g_stub.caps.raw_header = 0x1181;
    g_stub.caps.num_pdos = 7;
    const pd::Pdo mix[7] = {
        {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000},   {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000},
        {pd::PdoType::Fixed, 0, 12000, 12000, 3000, 36000}, {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000},
        {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000},{pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000},
        {pd::PdoType::Pps, 0, 3300, 20000, 5000, 100000}};
    for (int i = 0; i < 7; i++) g_stub.caps.pdos[i] = mix[i];
    g_stub.cc = 1;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 5;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000;
    Ui ui;
    ui.init(0);
    ui.configure_rotation(false, rotation);
    uint32_t t = 1000;
    for (int i = 0; i < 8; ++i) { ui.tick(t); t += 200; }   // the display throttle lapses: hero digits repaint
    CHECK_EQ(ui.rotation(), rotation);
}

// ---- the glyph-bitmap comparison (the Validate item) ---------------------------------------

static void test_same_string_glyph_bitmaps() {
    const std::string_view s = "20.04";   // five hero chars: digits and '.'
    Bitmap home_bmp[4], gfx_bmp[4];
    for (uint8_t r = 0; r < 4; ++r) {
        render_home_cell_string(r, s, kHero);
        home_bmp[r] = tight(kBg);
        render_gfx_string(r, s, kHero);
        gfx_bmp[r] = tight(kBg);
        CHECK_EQ(home_bmp[r].w > 0 && home_bmp[r].h > 0, true);
    }
    for (uint8_t r = 1; r < 4; ++r) {
        // Same pixel size and shape in every rotation (the host panel is the screen frame, so the
        // bitmaps must be identical; equal after rotating by 90 degrees follows and is checked too).
        CHECK_EQ(bitmap_eq(home_bmp[r], home_bmp[0], "home cell", r), true);
        CHECK_EQ(bitmap_eq(rot90cw(home_bmp[r]), rot90cw(home_bmp[0]), "home cell rot90", r), true);
        // Control: gfx_text already keeps native size; it must agree with itself in all rotations.
        CHECK_EQ(bitmap_eq(gfx_bmp[r], gfx_bmp[0], "gfx_text", r), true);
    }
    // The control and the Home path must paint the same glyph (same bitmap) in the 1:1 rotation.
    CHECK_EQ(bitmap_eq(home_bmp[0], gfx_bmp[0], "home==gfx rot0", 0), true);
}

// ---- the real Home page: hero digits land on the same native pixels in every rotation -------

static void test_home_page_hero_glyphs() {
    const int box_w = kHeroChars * kHero.adv, box_h = kHero.height;   // the hero line box, logical coords
    std::array<std::vector<uint8_t>, 4> mask{};                       // full-coverage ink (== kWatt) mask
    std::array<Rect, 4> nat{};
    for (uint8_t r = 0; r < 4; ++r) {
        render_home_page(r);
        Layout l = layout_for(r);
        ContentViewport viewport(l.content);          // maps like the page's own draw
        nat[r] = ContentViewport::native_rect({int16_t(kHeroX), int16_t(kHeroTop),
                                               int16_t(box_w), int16_t(box_h)});
        CHECK_EQ(nat[r].w == box_w && nat[r].h == box_h, true);   // the box itself never scales
        mask[r].assign(size_t(box_w) * box_h, 0);
        for (int dy = 0; dy < box_h; dy++)
            for (int dx = 0; dx < box_w; dx++) {
                const uint16_t px = g_panel[(nat[r].y + dy) * LCD_WIDTH + nat[r].x + dx];
                mask[r][size_t(dy) * box_w + dx] = px == kWatt ? 1 : 0;
            }
        // Some hero ink must actually be there (the fixture reads 36.75 W, the default W hero).
        unsigned ink = 0;
        for (uint8_t v : mask[r]) ink += v;
        CHECK_EQ(ink > 500u, true);
    }
    for (uint8_t r = 1; r < 4; ++r) {
        bool ok = mask[r] == mask[0];
        if (!ok) {
            int diffs = 0;
            for (size_t i = 0; i < mask[0].size(); i++) diffs += mask[r][i] != mask[0][i];
            std::printf("  rotation %d: hero ink mask differs from rotation 0 in %d of %zu pixels\n",
                        r, diffs, mask[0].size());
        }
        CHECK_EQ(ok, true);
    }
}

}  // namespace

// source_x / source_y are the inverse of mapped_rect: the panel pixels of every non-empty mapped logical column / row
// map back to it, and every pixel of the content box has exactly one owner (the single-write compositor relies on it).
static void test_source_inverts_mapped_rect() {
    for (uint8_t rot = 0; rot < 4; rot++) {
        const Layout l = layout_for(rot);
        ContentViewport viewport(l.content);
        const bool stretch = ContentViewport::stretched();
        int owned_x = 0, owned_y = 0, bad = 0;
        for (int x = 0; x < 224; x++) {
            const Rect r = ContentViewport::mapped_rect({int16_t(x), 0, 1, 1});
            for (int px = r.x; stretch && px < r.x + r.w; px++) {
                owned_x++;
                if (ContentViewport::source_x(px) != x) bad++;
            }
        }
        for (int y = 0; y < 240; y++) {
            const Rect r = ContentViewport::mapped_rect({0, int16_t(y), 1, 1});
            for (int py = r.y; stretch && py < r.y + r.h; py++) {
                owned_y++;
                if (ContentViewport::source_y(py) != y) bad++;
            }
        }
        CHECK_EQ(bad, 0);
        if (stretch) {
            CHECK_EQ(owned_x, l.content.w);               // the mapped columns tile the content box exactly
            CHECK_EQ(owned_y, l.content.h);
        } else {
            CHECK_EQ(ContentViewport::source_x(37), 37);  // no viewport work: identity
            CHECK_EQ(ContentViewport::source_y(41), 41);
        }
    }
}

// home::paint streams a row in windows of a few dozen pixels. A full-width paint (several windows per row, fills and
// glyphs crossing the window edges) and the same cell painted as narrow strips (one window per row, edges elsewhere)
// must leave identical panels, 1:1 and stretched alike.
static void test_paint_windows_are_invisible() {
    const int16_t top = kHeroCell.top;
    const home::Item items[4] = {
        home::fill(3, static_cast<int16_t>(top + 2), 217, 6, 0x1234),
        home::text(font_24, 60, static_cast<int16_t>(top + 12), "Window edge 123", kText),
        home::text(font_16, 5, static_cast<int16_t>(top + 44), "A long line of text across the cell", 0x07E0),
        home::text(font_12, 150, static_cast<int16_t>(top + 70), "tail text here", 0xF800),
    };
    for (uint8_t r = 0; r < 4; ++r) {
        home::Cell c;
        c.geom = kHeroCell;
        c.bg = kBg;
        c.sweep = false;
        c.items = items;
        const Layout l = layout_for(r);
        std::fill(std::begin(g_panel), std::end(g_panel), kBg);
        {
            ContentViewport viewport(l.content);
            home::paint(c, 0, top, home::kW - 1, kHeroCell.bottom);
        }
        const std::vector<uint16_t> whole(std::begin(g_panel), std::end(g_panel));
        std::fill(std::begin(g_panel), std::end(g_panel), kBg);
        {
            ContentViewport viewport(l.content);
            for (int x = 0; x < home::kW; x += 37)
                home::paint(c, x, top, x + 36 < home::kW - 1 ? x + 36 : home::kW - 1, kHeroCell.bottom);
        }
        int differ = 0, inked = 0;
        for (size_t i = 0; i < whole.size(); i++) {
            differ += whole[i] != g_panel[i];
            inked += whole[i] != kBg;
        }
        if (differ) std::printf("  rotation %d: %d pixels differ between the full paint and the strips\n", r, differ);
        CHECK_EQ(differ, 0);
        CHECK_EQ(inked > 2000, true);
    }
}

// gfx_text streams a row in windows too: a line several windows wide equals the rows render_row gives whole.
static void test_gfx_text_wide_line() {
    const std::string_view s = "A line of three windows..";   // 25 chars of font_16: 200 px
    const int w = text_width(font_16, s);
    CHECK_EQ(w > 160, true);
    std::fill(std::begin(g_panel), std::end(g_panel), kBg);
    gfx_text(Surface{0, 0, 240, 240}, font_16, 10, 20, s, kText, kBg);
    uint16_t lut[16];
    make_blend_lut(kText, kBg, lut);
    uint16_t row[240];
    int differ = 0;
    for (int r = 0; r < font_16.height; r++) {
        CHECK_EQ(render_row(font_16, s, r, lut, row, 240), w);
        for (int x = 0; x < w; x++) differ += g_panel[(20 + r) * LCD_WIDTH + 10 + x] != row[x];
    }
    CHECK_EQ(differ, 0);
}

int main() {
    test_gfx_text_wide_line();
    test_paint_windows_are_invisible();
    test_same_string_glyph_bitmaps();
    test_home_page_hero_glyphs();
    test_source_inverts_mapped_rect();
    return test_report("test_ui_glyph_rotation");
}
