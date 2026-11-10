// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// ROADMAP 2026-11-10 Row D (handoff 2c): the Connection page must not flicker on a steady link.
//  - A steady inline link with a flapping fast-charge verdict, a flapping USB-link family and ADC /
//    VBUS jitter produces 0 body paint passes over 10 s on every tab (counted via g_conn_body_passes,
//    and 0 pixels written into the body band). A value that HOLDS for ~1 s still reaches the rows once.
//  - A row-count change (6 -> 5 -> 6) repaints only the vacated slot and the moved closing hairline,
//    never a whole-body wipe, and the result is pixel-identical to a forced full redraw (resync).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/pixel_sink.hpp"
#include "k2/drivers/lcd.hpp"
#include "ui_nav.h"

using namespace k2;

#define CHECK(c) CHECK_EQ(int(!!(c)), 1)

// Body-band write counter: diverts the LCD stream into g_panel (so frames stay comparable) and counts
// every pixel written with y in [kit::kBodyY, kFooterY) -- one paint_body pass touches that band.
namespace bh {
constexpr int kTop = kit::kBodyY, kBot = 222;   // kFooterY
unsigned total = 0;
unsigned cx, cy, x0, x1, y0, y1;
PixelSink panel;
void begin(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    static bool attached = false;
    if (!attached) { panel.attach(g_panel, 240, 0, 240); attached = true; }
    panel.window(a, b, c, d);
    x0 = cx = a; y0 = cy = b; x1 = c; y1 = d;
}
void put(uint32_t k) {
    for (; k; --k) {
        if (cy >= kTop && cy < kBot) total++;
        if (++cx > x1) { cx = x0; if (++cy > y1) cy = y0; }
    }
}
void pixels(const uint16_t *px, uint32_t count) { panel.push(px, count); put(count); }
void fill(uint16_t color, uint32_t count) { panel.fill(color, count); put(count); }
const lcd::Sink sink{begin, pixels, fill};
void reset() { total = 0; }
}  // namespace bh

// A new Ui every time: init() keeps the submenu stack and the link state of the previous case.
static Ui &fresh_ui() {
    static std::unique_ptr<Ui> ui;
    ui = std::make_unique<Ui>();
    g_stub = StubState();
    g_stub.ms = 10000;
    g_stub.cc = 0;
    g_stub.pm.vbus_mv = 0;
    ui->init(g_stub.ms);
    ui->set_rotation(0, g_stub.ms);
    for (uint32_t t = 10100; t <= 11500; t += 100) ui->tick(t);
    g_stub.ms = 12000;
    return *ui;
}

static void make_inline(Ui &ui, uint32_t t) {
    g_stub.cc = 1;
    g_stub.pm.vbus_mv = 19700;
    g_stub.pm.current_ma = 2000;
    for (int i = 0; i < 3; i++) ui.tick(t + 100u * i);
    CHECK_EQ(int(ui.link().inline_mode()), 1);
}

static void set_adc(int32_t dp, int32_t dm, int32_t cc1, int32_t cc2) {
    adc::Reading r;
    r.dp_mv = dp; r.dm_mv = dm; r.cc1_mv = cc1; r.cc2_mv = cc2;
    g_stub.adc = r;
}

// A steady inline link with a live contract (Source tab shows the offers, Sink tab the sniffed
// request, Cable tab the e-marker), then 10 s of flapping fc / usb-link / ADC / VBUS per tab.
static void test_steady_link_zero_body_passes() {
    Ui &ui = fresh_ui();
    make_inline(ui, 20000);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20030;
    g_stub.pm_fast_set = true; g_stub.pm_fast_mv = 20030;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 3;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Pps, 0, 5000, 11000, 5000, 55000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 2;
    g_stub.contract.mv = 9000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000;
    g_stub.em.valid = true; g_stub.em.vid = 0x1234; g_stub.em.pid = 0x5678; g_stub.em.vendor_str = "Cable";
    set_adc(300, 80, 1678, 3300);
    static const fastcharge::Protocol flap[] = {fastcharge::Protocol::Pending, fastcharge::Protocol::Qc2_5V,
                                                fastcharge::Protocol::NoQuick, fastcharge::Protocol::Dcp5V,
                                                fastcharge::Protocol::Apple10, fastcharge::Protocol::Scp};
    for (uint8_t tab = 0; tab < 3; tab++) {
        uint32_t t = 22000 + tab * 30000;
        ui.goto_page(kConnTabAlias + tab, t);
        for (int i = 0; i < 15; i++) ui.tick(t += 100);   // settle: page painted, debounce primed
        lcd::set_sink(&bh::sink);
        bh::reset();
        const uint32_t passes0 = g_conn_body_passes, ml0 = g_match_live_calls;
        for (int i = 0; i < 100; i++) {   // 10 s of jitter, nothing holds
            t += 100;
            g_stub.fc = flap[(i / 3) % 6];                       // a new fc candidate every 300 ms
            g_stub.ul.mbps = (i & 1) ? 10000u : 480u;            // the usb link flaps
            g_stub.ul.family = (i & 1) ? usblink::Family::Usb32 : usblink::Family::Usb2;
            g_stub.ul.conf = (i & 1) ? usblink::Confidence::Negotiated : usblink::Confidence::Advertised;
            set_adc(300 + (i % 7) * 13, 80 + (i % 4) * 9, 1678 + (i % 5) * 7, 3300 - (i % 3) * 11);
            g_stub.pm_fast_mv = (i & 1) ? 20290 : 20310;         // the Type-C VBUS step flaps 20.2/20.3 V
            ui.tick(t);
        }
        const unsigned px = bh::total;
        const uint32_t passes = g_conn_body_passes - passes0, ml = g_match_live_calls - ml0;
        lcd::set_sink(nullptr);
        std::printf("tab %u: %u body passes, %u body px, %u match_live calls over 10 s of flapping\n",
                    unsigned(tab), passes, px, ml);
        CHECK_EQ(passes, 0u);   // Row D budget: a steady link repaints nothing
        CHECK_EQ(px, 0u);
        CHECK_EQ(ml, 0u);       // the debounced inputs do not even reach the signatures
        // Positive control: values that HOLD for ~1 s reach the rows exactly once, then quiet.
        // (The usb-link row renders on Source and Sink; the Cable tab renders the e-marker current.)
        g_stub.fc = fastcharge::Protocol::Qc2_5V;
        g_stub.ul.mbps = 5000; g_stub.ul.family = usblink::Family::Usb32;
        g_stub.ul.conf = usblink::Confidence::Hint;   // the flap ended on 10000/Negotiated
        if (tab == 1) g_stub.em.max_current_a = 5;    // Cable tab: the Current row reads the e-marker
        set_adc(300, 80, 1678, 3300);
        g_stub.pm_fast_mv = 20030;
        lcd::set_sink(&bh::sink);
        bh::reset();
        const uint32_t p0 = g_conn_body_passes;
        for (int i = 0; i < 15; i++) { t += 100; ui.tick(t); }   // 1.5 s holding the new values
        const unsigned held_px = bh::total;
        const uint32_t held_passes = g_conn_body_passes - p0;
        bh::reset();
        const uint32_t p1 = g_conn_body_passes;
        for (int i = 0; i < 30; i++) { t += 100; ui.tick(t); }   // 3 s more, still holding
        const unsigned quiet_px = bh::total;
        const uint32_t quiet_passes = g_conn_body_passes - p1;
        lcd::set_sink(nullptr);
        std::printf("tab %u: held change: %u passes / %u px, then %u passes / %u px\n",
                    unsigned(tab), held_passes, held_px, quiet_passes, quiet_px);
        CHECK(held_passes >= 1u);        // the held value did reach the page
        CHECK(held_px > 0u);
        CHECK_EQ(quiet_passes, 0u);      // and then it went quiet
        CHECK_EQ(quiet_px, 0u);
    }
}

// Count the full-width hairlines of the Connection list area (one per row top plus the closing rule).
static int body_hairlines() {
    const int y0 = kit::kBodyY + 18, pitch = 20;
    int n = 0;
    for (int y = y0; y < 222; y += pitch) {
        int rule = 0;
        for (int x = 0; x < LCD_WIDTH; x++) rule += g_panel[y * LCD_WIDTH + x] == kRule;
        if (rule >= LCD_WIDTH - 20) n++;
    }
    return n;
}

// A 4 -> 3 -> 4 row-count change on the Source tab paints only the vacated slot plus the moved closing
// hairline (never the 44 352 px whole-body wipe), and the frame ends pixel-identical to a forced resync.
static void test_row_count_change_paints_slots_only() {
    Ui &ui = fresh_ui();
    g_stub.session = 77;   // a new attach session: the static offer book of the previous test must clear
    g_stub.pm.vbus_mv = 5000;
    g_stub.pm_online = true;
    g_stub.cc = 1;
    uint32_t t = 20000;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().attached()), 1);
    for (int i = 0; i < 55; i++) ui.tick(t += 100);   // past the waiting-for-caps window
    CHECK_EQ(int(ui.link().waiting()), 0);
    set_adc(0, 0, 408, 1);                            // a default-USB Rp on the active line
    g_stub.fc = fastcharge::Protocol::Pending;
    ui.goto_page(kConnTabAlias + 0, t += 100);
    for (int i = 0; i < 15; i++) ui.tick(t += 100);
    const int rows6 = body_hairlines();               // ID, PD "none", Type-C, USB + the closing rule
    CHECK(rows6 >= 4);
    // Drop one row (a PD-speaking source whose caps were missed: the Type-C row goes).
    lcd::set_sink(&bh::sink);
    bh::reset();
    const uint32_t p0 = g_conn_body_passes;
    g_stub.pd_traffic = true;
    ui.tick(t += 100);
    const unsigned drop_px = bh::total;
    const uint32_t drop_passes = g_conn_body_passes - p0;
    lcd::set_sink(nullptr);
    const int rows5 = body_hairlines();
    std::printf("6->5 rows: %u body passes, %u body px (a whole-body wipe is 44352 px), hairlines %d -> %d\n",
                drop_passes, drop_px, rows6, rows5);
    CHECK_EQ(rows5, rows6 - 1);                        // the list and its closing rule moved up one slot
    CHECK(drop_passes <= 2u);                          // at most the pass that notices + the slot clear
    CHECK(drop_px <= 8000u);                           // one vacated slot (4480 px) + hairline, not a wipe
    // And back: the freed slot is repainted, the rest untouched.
    lcd::set_sink(&bh::sink);
    bh::reset();
    g_stub.pd_traffic = false;
    ui.tick(t += 100);
    const unsigned back_px = bh::total;
    lcd::set_sink(nullptr);
    const int rows6b = body_hairlines();
    std::printf("5->6 rows: %u body px, hairlines %d -> %d\n", back_px, rows5, rows6b);
    CHECK_EQ(rows6b, rows6);
    CHECK(back_px <= 8000u);
    for (int i = 0; i < 5; i++) ui.tick(t += 100);
    // The incremental result equals a forced full redraw (resync): flip the rotation twice.
    std::vector<uint16_t> before(g_panel, g_panel + LCD_WIDTH * LCD_HEIGHT);
    ui.set_rotation(1, t);
    ui.set_rotation(0, t);
    unsigned diff = 0;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) diff += before[i] != g_panel[i];
    std::printf("after 6->5->6: incremental vs forced resync diff=%u\n", diff);
    CHECK_EQ(diff, 0u);
    g_stub.pd_traffic = false;
    g_stub.pm.vbus_mv = 5000;
}

int main() {
    test_steady_link_zero_body_passes();
    test_row_count_change_paints_slots_only();
    return test_report("test_ui_conn_flicker");
}
