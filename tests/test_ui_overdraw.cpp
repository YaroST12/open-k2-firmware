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
#include "k2/pd/pd_source.hpp"
#include <cstdio>
#include <cstdlib>

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

// full_max: the budget for one full paint after a clear, px sent. A ratchet: it only goes down (each phase of the overdraw work
// tightens it to what the page now costs); a page that gets dearer fails here.
struct Dest { const char *name; uint8_t index; unsigned full_max; };
// The walk: the six rotation pages, then the Tools / Settings submenus that open without a record or parameters.
constexpr Dest kDests[] = {
    {"Home", 0, 59200}, {"Detail", 1, 32600}, {"Charger", 2, 70900}, {"Lines", 3, 38400}, {"Tools", 4, 71900}, {"Settings", 5, 73500},
    {"Connection", kPdPage, 78500}, {"Diag", kDiagPage, 34700},
    {"Trigger", kMenuPageBase + static_cast<uint8_t>(Menu::Trigger), 160500},
    {"PdTrigger", kMenuPageBase + static_cast<uint8_t>(Menu::PdTrigger), 162200},
    {"CableR", kMenuPageBase + static_cast<uint8_t>(Menu::CableR), 67900},
    {"BlackBox", kMenuPageBase + static_cast<uint8_t>(Menu::BlackBox), 139100},
    {"Spoof", kMenuPageBase + static_cast<uint8_t>(Menu::Spoof), 57200},
    {"UsbLink", kMenuPageBase + static_cast<uint8_t>(Menu::UsbLink), 64200},
    {"Cards", kMenuPageBase + static_cast<uint8_t>(Menu::Cards), 68800},
};
// The worst page switch, as a multiple of the screen (ratchet, see full_max).
constexpr double kSwitchMaxX = 7.50;
constexpr unsigned kN = sizeof kDests / sizeof kDests[0];

struct Row { unsigned sent, unique; };

Row g_full[kN];
Row g_sw[kN][kN];   // [from][to]

uint32_t g_now = 1000;
void settle(Ui &ui) { for (int i = 0; i < 4; ++i) { g_now += 120; ui.tick(g_now); } }

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
        lcd_meter_reset();
        ui.repaint(g_now += 10);
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
            g_sw[a][b] = meter();
        }
}

}  // namespace

int main() {
    if (const char *r = std::getenv("K2_OVERDRAW_ROT")) g_rotation = static_cast<uint8_t>(std::atoi(r));
    walk();
    std::printf("Full paint after a clear (px sent / unique px touched / ratios):\n");
    std::printf("  %-11s %8s %8s %7s %9s %9s\n", "page", "sent", "unique", "s/u", "s/53760", "s/57600");
    for (unsigned i = 0; i < kN; ++i)
        std::printf("  %-11s %8u %8u %7.2f %9.2f %9.2f\n", kDests[i].name, g_full[i].sent, g_full[i].unique,
                    double(g_full[i].sent) / g_full[i].unique, double(g_full[i].sent) / kContentPx, double(g_full[i].sent) / kScreenPx);
    std::printf("Page switch, px sent (erase pass + incoming paint), as multiples of the 57600 px screen; rows = from, columns = to:\n");
    std::printf("  %-11s", "");
    for (unsigned b = 0; b < kN; ++b) std::printf(" %6.6s", kDests[b].name);
    std::printf("\n");
    double worst = 0;
    for (unsigned a = 0; a < kN; ++a) {
        std::printf("  %-11s", kDests[a].name);
        for (unsigned b = 0; b < kN; ++b) {
            if (a == b) { std::printf(" %6s", "-"); continue; }
            const double x = double(g_sw[a][b].sent) / kScreenPx;
            if (x > worst) worst = x;
            std::printf(" %6.2f", x);
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
    for (unsigned i = 0; i < kN; ++i) {
        CHECK_EQ(g_full[i].unique > 0, true);
        if (g_full[i].sent > kDests[i].full_max)
            std::printf("  OVER BUDGET %s: %u px sent, budget %u\n", kDests[i].name, g_full[i].sent, kDests[i].full_max);
        CHECK_EQ(g_full[i].sent <= kDests[i].full_max, true);
    }
    CHECK_EQ(worst <= kSwitchMaxX, true);
    return test_report("test_ui_overdraw");
}
