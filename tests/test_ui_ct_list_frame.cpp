// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Render test for the ROADMAP row "Charger-test spoof list loses selection borders on scroll":
// the Cable step's identity list is scrolled one row at a time to the end and back, in portrait
// and in the landscape rotations, and after every step each visible row's selection square
// (the 12x12 radio marker at kit::kMarkX) must have all four border edges in the panel pixels.
//
// The frame is drawn at its native size clipped to the viewport (like a glyph), so in a landscape
// rotation the row mapping cannot squeeze a 1 px border line away to zero physical rows — which
// rows lose their top/bottom line moved as the list scrolled, exactly the reported defect.
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_layout.hpp"
#include "k2/ui/orientation.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/kit.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/pd/emlib.hpp"

using namespace k2;

static void fixture() {
    g_stub = {}; pd::set_fake(0);
    g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
    g_stub.fc_answers = true;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 1; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2;
    g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300);
    g_stub.caps.raw_header = 0x1181;
}
static void press(Ui& ui, Button b, uint32_t now) { ui.on_button(b, ButtonEvent::Short, now); }
static emlib::Entry saved(unsigned seq) {
    const uint32_t v[5] = {0xFF00A041, 0x1800203C, 123, seq << 16, 0x00182052};
    emlib::Entry e;
    e.seq = seq; e.version = 1; e.rec = emrec::make(v, 2);
    std::snprintf(e.name.data(), e.name.size(), "Host identity %u", seq);
    return e;
}
static void dump(const char* name) {
    if (const char* d = std::getenv("K2_DUMP")) {
        char p[256]; std::snprintf(p, sizeof p, "%s/k2_ctframe_%s.panel", d, name);
        if (FILE* f = fopen(p, "wb")) { fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); fclose(f); }
    }
}
static int slot_top(int slot, int open_slot, int y0) {
    return y0 + slot * kit::kRowH + (open_slot >= 0 && slot > open_slot ? kit::kFocusExtra : 0);
}
// The marker's physical rect: origin mapped through the rotation's content viewport, size native
// (12x12), clipped to the viewport's destination — exactly what ContentViewport::native_rect computes.
static Rect marker_phys(Ui& ui, int lx, int ly) {
    const Layout lay = compute_layout(orientation_for(ui.rotation()).edge, kSlotCenters, kSlotLens, kPanelThickness);
    const Rect &d = lay.content;
    const int px = d.x + lx * d.w / 224;
    const int py = d.y + ly * d.h / 240;
    return {static_cast<int16_t>(px), static_cast<int16_t>(py),
            static_cast<int16_t>(std::min<int>(kit::kMarkSize, d.x + d.w - px)),
            static_cast<int16_t>(std::min<int>(kit::kMarkSize, d.y + d.h - py))};
}
// Every visible row's selection square must show all four edges in the marker colour
// (kFocus on the picked row's solid square, kDim on the others' hollow frame).
static int check_frames(Ui& ui, int pick, int cursor, int y0, const char* when) {
    const ListState st = ui.list_state();
    const int open_slot = cursor >= st.offset && cursor < st.end() ? cursor - st.offset : -1;
    int bad = 0;
    for (int s = 0; s < st.visible && st.offset + s < st.count; ++s) {
        const int idx = st.offset + s;
        const uint16_t col = idx == pick ? kFocus : kDim;
        const int rt = slot_top(s, open_slot, y0);
        const Rect m = marker_phys(ui, kit::kMarkX, rt + 7);
        if (m.w < kit::kMarkSize || m.h < kit::kMarkSize) {
            std::printf("  %s: row %d slot %d marker clipped by the viewport: %dx%d at (%d,%d)\n",
                        when, idx, s, m.w, m.h, m.x, m.y);
            bad++;
            continue;
        }
        for (int k = 0; k < kit::kMarkSize; ++k) {
            struct { int x, y; } pts[4] = {{m.x + k, m.y}, {m.x + k, m.y + 11}, {m.x, m.y + k}, {m.x + 11, m.y + k}};
            for (auto& p : pts)
                if (g_panel[p.y * LCD_WIDTH + p.x] != col) {
                    if (bad < 24) std::printf("  %s: row %d slot %d edge (%d,%d) is %04x want %04x\n",
                                              when, idx, s, p.x, p.y, g_panel[p.y * LCD_WIDTH + p.x], col);
                    bad++;
                }
        }
    }
    return bad;
}
// Scroll the Cable-step list one row at a time to the end and back, checking frames after each step.
static void scroll_end_and_back(Ui& ui, uint32_t& now, int rot, int& bad, bool shots) {
    char w[64];
    const int count = ui.list_state().count;
    for (int step = 0; step < count; ++step) {
        press(ui, Button::Plus, now); ui.tick(now); now += 20;
        std::snprintf(w, sizeof w, "rot%d down %d (cursor %d off %d)", rot, step, ui.charger_test_cursor(), ui.list_state().offset);
        bad += check_frames(ui, 8, ui.charger_test_cursor(), kit::kBodyY, w);
        if (shots && step == 3) { char n[32]; std::snprintf(n, sizeof n, "rot%d_down3", rot); dump(n); }
        if (shots && step == count - 1) { char n[32]; std::snprintf(n, sizeof n, "rot%d_end", rot); dump(n); }
    }
    for (int step = 0; step < count; ++step) {
        press(ui, Button::Minus, now); ui.tick(now); now += 20;
        std::snprintf(w, sizeof w, "rot%d up %d (cursor %d off %d)", rot, step, ui.charger_test_cursor(), ui.list_state().offset);
        bad += check_frames(ui, 8, ui.charger_test_cursor(), kit::kBodyY, w);
        if (shots && step == 2) { char n[32]; std::snprintf(n, sizeof n, "rot%d_up2", rot); dump(n); }
    }
}
int main() {
    fixture();
    for (unsigned n = 1; n <= 10; ++n) g_stub.lib.push_back(saved(n));   // 14 rows, 7 visible: the list scrolls
    Ui ui; ui.init(0);
    uint32_t now = 1000;
    int bad = 0;
    for (int rot = 0; rot < 4; ++rot) {
        ui.set_rotation(static_cast<uint8_t>(rot), now); now += 20;
        ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), now); now += 20;
        ui.tick(now); now += 20;
        for (unsigned i = 0; i < 40; ++i) press(ui, Button::Minus, now);   // the Tests step to the top
        press(ui, Button::Ok, now);                                        // a preset pick advances to the Cable step
        ui.tick(now); now += 20;
        if (ui.charger_test_stage() != 2) { std::printf("rot%d: stage %d != 2\n", rot, ui.charger_test_stage()); continue; }
        const int count = ui.list_state().count;
        std::printf("rot%d: Cable step, count=%d visible=%d\n", rot, count, ui.list_state().visible);
        if (rot == 0) {
            // pick a saved clone (row 8) so the picked row's solid square is scrolled too:
            // OK advances to Review, BACK returns with the cursor on the pick
            for (int i = 0; i < 8; ++i) press(ui, Button::Plus, now);
            press(ui, Button::Ok, now); ui.tick(now); now += 20;
            press(ui, Button::Back, now); ui.tick(now); now += 20;
            std::printf("  picked row 8: stage=%d cursor=%d\n", ui.charger_test_stage(), ui.charger_test_cursor());
        }
        scroll_end_and_back(ui, now, rot, bad, true);
        // back to the Tests step for the next rotation
        press(ui, Button::Back, now); ui.tick(now); now += 20;
    }
    std::printf("total bad=%d\n", bad);
    CHECK_EQ(bad, 0);
    return test_report("test_ui_ct_list_frame");
}
