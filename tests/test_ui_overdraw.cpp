// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Overdraw metric and budgets. The panel has no read-back and no framebuffer, so every pixel a paint sends costs SPI time
// (1.07 us/px at 15 MHz), and a long paint interrupts the charger test. The fake LCD counts, per paint, the pixels sent
// and the distinct panel pixels they touched (tests/fake_lcd.h, the full-screen clear is not counted); this test prints
// the table for every page (one full paint after a clear) and every page-to-page switch, and holds the budgets.
//   sent / unique   how many times the average touched pixel is written (1.00 = each pixel once)
//   sent / 53760    against the logical content (224 x 240)
//   sent / 57600    against the whole panel
// K2_OVERDRAW_ROWS=1 also prints, per page, which rows were written more than once.
#include "fake_lcd.h"
#include "test_util.h"
#include "k2/ui/ui.hpp"
#include "ui_nav.h"
#include "ui_stubs.h"
#include "ui_walk.h"
#include "k2/pd/pd_source.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace k2;

namespace {

constexpr unsigned kContentPx = 224u * 240u, kScreenPx = 240u * 240u;

void fixture() {
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20030;
    g_stub.pm.current_ma = 1520;
    g_stub.pm.power_mw = 30456;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 7; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1;
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;
    const pd::Pdo mix[7] = {
        {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000},     {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000},
        {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000},   {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000},
        {pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000}, {pd::PdoType::Pps, 0, 3300, 21000, 5000, 100000},
        {pd::PdoType::Avs, 0, 15000, 28000, 5000, 140000}};
    for (int i = 0; i < 7; i++) g_stub.caps.pdos[i] = mix[i];
}

// full_max: the budget for one full paint after a clear (Ui::repaint), px sent. A ratchet: it only goes down (each phase of the
// overdraw work tightens it to what the page now costs); a page that gets dearer fails here.
// An opaque page (WalkDest::opaque) has no row here (0): its repaint skips the clear and paints over the screen, every pixel,
// so it is held to the opaque switch bound (kOpaqueMaxX / kScaledOpaqueMaxX) instead. Its blank paint after a clear only
// remains on the screenshot path (Ui::render_for_capture), which costs no wire time.
// The walk is tests/ui_walk.h (shared with test_ui_capture); the budgets sit in the same order.
constexpr unsigned kN = kWalkN;
constexpr unsigned kFullMax[kN] = {0, 0, 57100, 0, 0, 0, 52800, 0, 57400, 57000, 19900, 43800, 47700, 0, 57700};
constexpr const WalkDest *kDests = kWalkDests;
// The worst page switch, as a multiple of the screen (ratchet, see full_max). A menu reached from a menu is two switches
// (goto_page pops to the base page, then pushes), which is what the worst cell is.
constexpr double kSwitchMaxX = 3.50;
// A switch into an opaque page paints once, with no erase pass: the page's own paint plus the ground it lays under it
// (measured 1.00 to 1.03x the screen; the brief's bound is 1.15x).
constexpr double kOpaqueMaxX = 1.05;
// The landscape rotations (1, 3) scale 224 x 240 into 240 x 224: a glyph keeps its native size there, so no ground can leave a
// hole for it and every fill goes whole under the glyphs (ContentViewport::scaled()). Their bounds are looser, but an opaque
// switch is still the cheaper one (hardware-free host walk: Home -> Detail 1.34x opaque against 1.44x with the erase pass,
// Detail -> Home 1.00x against 2.26x).
constexpr double kScaledFullSlack = 1.25;
constexpr double kScaledSwitchMaxX = 3.85;
constexpr double kScaledOpaqueMaxX = 1.36;

struct Row { unsigned sent, unique; };

Row g_full[kN];
Row g_sw[kN][kN];   // [from][to]
int g_diff[kN][kN]; // pixels that differ from a clean paint of the destination

uint32_t g_now = 1000;
void settle(Ui &ui) { for (int i = 0; i < 40 && ui.painting(); ++i) ui.tick(g_now); }

uint8_t g_rotation = 0;

// Live data for a few seconds with Home open, so its sweep trend has history (a bench tester shows a drifting load).
void warm_home(Ui &ui) {
    for (int i = 0; i < 160; ++i) {
        g_stub.pm.vbus_mv = 20030 + (i * 7) % 90;
        g_stub.pm.current_ma = 1200 + (i * 37) % 700;
        g_stub.pm.power_mw = g_stub.pm.vbus_mv * g_stub.pm.current_ma / 1000;
        g_now += 100;
        ui.tick(g_now);
    }
}

Row meter() { return {g_meter_sent, g_meter_unique}; }

// K2_DUMP=dir writes dir/k2_<tag>_<name>.panel (tools/panel2png.py makes the PNGs).
void dump_panel(const char *tag, const char *name) {
    const char *dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char path[200];
    std::snprintf(path, sizeof path, "%s/k2_%s_%s.panel", dir, tag, name);
    if (FILE *f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, size_t(LCD_WIDTH) * LCD_HEIGHT, f); std::fclose(f); }
}

void print_rows(const char *name) {
    if (!std::getenv("K2_OVERDRAW_ROWS")) return;
    const uint8_t *w = lcd_meter_writes();
    int run0 = -1;
    unsigned run_px = 0;
    for (int y = 0; y <= 240; ++y) {
        unsigned px = 0;
        if (y < 240) for (int x = 0; x < 240; ++x) px += w[y * 240 + x] > 1 ? w[y * 240 + x] - 1u : 0;
        if (px && run0 < 0) { run0 = y; run_px = 0; }
        if (px) run_px += px;
        if (!px && run0 >= 0) { std::printf("    %-10s rows %3d..%3d rewrote %u px\n", name, run0, y - 1, run_px); run0 = -1; }
    }
}

void walk() {
    fixture();
    Ui ui;
    ui.init(g_now);
    ui.set_rotation(g_rotation, g_now);
    settle(ui);
    warm_home(ui);
    // One full paint per destination: reach the page, settle it, then clear and repaint (Ui::repaint).
    for (unsigned i = 0; i < kN; ++i) {
        ui.goto_page(0, g_now += 10);
        if (!ui.goto_page(kDests[i].index, g_now += 10)) { std::printf("cannot open %s\n", kDests[i].name); CHECK_EQ(0, 1); continue; }
        settle(ui);
        lcd::fill(0);   // kBg: the repaint of an opaque page alone does not clear
        lcd_meter_reset();
        ui.repaint(g_now += 10);
        settle(ui);
        g_full[i] = meter();
        dump_panel("full", kDests[i].name);
        print_rows(kDests[i].name);
        lcd_meter_trace_dump(kDests[i].name);
    }
    // Every ordered pair of pages: settle on A, switch to B (the erase pass and the incoming paint are both in the count).
    for (unsigned a = 0; a < kN; ++a)
        for (unsigned b = 0; b < kN; ++b) {
            if (a == b) continue;
            ui.goto_page(0, g_now += 10);
            ui.goto_page(kDests[a].index, g_now += 10);
            settle(ui);
            lcd_meter_reset();
            ui.goto_page(kDests[b].index, g_now += 10);
            settle(ui);
            g_sw[a][b] = meter();
            // Pixel-exact: the switch must leave what a clean paint of B leaves (same instant, same data).
            const std::vector<uint16_t> got(g_panel, g_panel + size_t(LCD_WIDTH) * LCD_HEIGHT);
            lcd::fill(0);   // kBg, as above
            ui.repaint(g_now);
            settle(ui);
            int diff = 0, first = -1;
            for (size_t i = 0; i < got.size(); ++i)
                if (got[i] != g_panel[i]) { ++diff; if (first < 0) first = int(i); }
            if (diff && std::getenv("K2_OVERDRAW_EXACT"))
                std::printf("  DIFF %s -> %s: %d px, first x=%d y=%d got %04x clean %04x\n", kDests[a].name, kDests[b].name, diff,
                            first % LCD_WIDTH, first / LCD_WIDTH, got[first], g_panel[first]);
            if (diff && std::getenv("K2_OVERDRAW_EXACT") && std::atoi(std::getenv("K2_OVERDRAW_EXACT")) > 1) {
                // The rows that differ, as runs: "y0..y1 x0..x1 n px" (x range over the run).
                int y0 = -1, mnx = 999, mxx = -1, n = 0;
                for (int y = 0; y <= LCD_HEIGHT; ++y) {
                    int rn = 0, rmn = 999, rmx = -1;
                    if (y < LCD_HEIGHT)
                        for (int x = 0; x < LCD_WIDTH; ++x)
                            if (got[size_t(y) * LCD_WIDTH + x] != g_panel[size_t(y) * LCD_WIDTH + x]) { ++rn; if (x < rmn) rmn = x; rmx = x; }
                    if (rn && y0 < 0) { y0 = y; mnx = 999; mxx = -1; n = 0; }
                    if (rn) { n += rn; if (rmn < mnx) mnx = rmn; if (rmx > mxx) mxx = rmx; }
                    if (!rn && y0 >= 0) { std::printf("    rows %3d..%3d x %3d..%3d  %d px\n", y0, y - 1, mnx, mxx, n); y0 = -1; }
                }
            }
            g_diff[a][b] = diff;
        }
}

void print_tables(double worst) {
    std::printf("Full repaint, Ui::repaint (an opaque page over the screen, the rest after a clear; px sent / unique px touched / ratios):\n");
    std::printf("  %-11s %8s %8s %7s %9s %9s\n", "page", "sent", "unique", "s/u", "s/53760", "s/57600");
    for (unsigned i = 0; i < kN; ++i)
        std::printf("  %-11s %8u %8u %7.2f %9.2f %9.2f\n", kDests[i].name, g_full[i].sent, g_full[i].unique,
                    double(g_full[i].sent) / g_full[i].unique, double(g_full[i].sent) / kContentPx, double(g_full[i].sent) / kScreenPx);
    std::printf("Page switch, px sent (erase pass + incoming paint), as multiples of the 57600 px screen; rows = from, columns = to:\n");
    std::printf("  %-11s", "");
    for (unsigned b = 0; b < kN; ++b) std::printf(" %6.6s", kDests[b].name);
    std::printf("\n");
    for (unsigned a = 0; a < kN; ++a) {
        std::printf("  %-11s", kDests[a].name);
        for (unsigned b = 0; b < kN; ++b) {
            if (a == b) { std::printf(" %6s", "-"); continue; }
            std::printf(" %6.2f", double(g_sw[a][b].sent) / kScreenPx);
        }
        std::printf("\n");
    }
    std::printf("  worst switch %.2fx the screen\n", worst);
    std::printf("Page switch, sent / unique:\n");
    for (unsigned a = 0; a < kN; ++a) {
        std::printf("  %-11s", kDests[a].name);
        for (unsigned b = 0; b < kN; ++b) {
            if (a == b) { std::printf(" %6s", "-"); continue; }
            std::printf(" %6.2f", double(g_sw[a][b].sent) / g_sw[a][b].unique);
        }
        std::printf("\n");
    }
}

void check_rotation(uint8_t rotation, bool print) {
    g_rotation = rotation;
    std::printf("-- rotation %u\n", unsigned(rotation));
    walk();
    double worst = 0;
    for (unsigned a = 0; a < kN; ++a)
        for (unsigned b = 0; b < kN; ++b)
            if (a != b) worst = std::max(worst, double(g_sw[a][b].sent) / kScreenPx);
    if (print) print_tables(worst);
    const bool scaled = rotation & 1;
    const double opaque_max = scaled ? kScaledOpaqueMaxX : kOpaqueMaxX;
    for (unsigned i = 0; i < kN; ++i) {
        CHECK_EQ(g_full[i].unique > 0, true);
        const unsigned budget = kDests[i].opaque ? static_cast<unsigned>(opaque_max * kScreenPx)
                                : scaled         ? static_cast<unsigned>(kFullMax[i] * kScaledFullSlack)
                                                 : kFullMax[i];
        if (g_full[i].sent > budget)
            std::printf("  OVER BUDGET %s: %u px sent, budget %u\n", kDests[i].name, g_full[i].sent, budget);
        CHECK_EQ(g_full[i].sent <= budget, true);
    }
    CHECK_EQ(worst <= (scaled ? kScaledSwitchMaxX : kSwitchMaxX), true);
    // Every switch leaves what a clean paint leaves; a switch into an opaque page paints once.
    for (unsigned a = 0; a < kN; ++a)
        for (unsigned b = 0; b < kN; ++b) {
            if (a == b) continue;
            if (g_diff[a][b]) std::printf("  NOT EXACT %s -> %s: %d px differ from a clean paint\n", kDests[a].name, kDests[b].name, g_diff[a][b]);
            CHECK_EQ(g_diff[a][b], 0);
            if (kDests[b].opaque && walk_one_switch(a, b)) {
                const double x = double(g_sw[a][b].sent) / kScreenPx;
                if (x > opaque_max) std::printf("  OPAQUE TOO DEAR %s -> %s: %.2fx the screen\n", kDests[a].name, kDests[b].name, x);
                CHECK_EQ(x <= opaque_max, true);
            }
        }
}

}  // namespace

// K2_OVERDRAW_ROT=n walks one rotation and prints the tables; without it all four rotations are checked.
int main() {
    if (const char *r = std::getenv("K2_OVERDRAW_ROT")) {
        check_rotation(static_cast<uint8_t>(std::atoi(r)), true);
    } else {
        for (uint8_t r = 0; r < 4; ++r) check_rotation(r, r == 0);
    }
    return test_report("test_ui_overdraw");
}
