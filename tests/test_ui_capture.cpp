// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Drives the real Ui page-draw code and the real capture_screen() through host stand-ins for the
// drivers, and checks that the streamed screenshot equals a direct render to the panel.
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "ui_nav.h"
#include "k2/ui/capture.hpp"
#include "k2/ui/text.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include "card_fixtures.h"
#include "k2/core/remote_proto.hpp"
#include "k2/ui/rle565.hpp"
#include <memory>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

using namespace k2;

static void setup_stubs() {
    k2::usb::stats.log_sent = 1007;                    // counters wider than a digit boundary away from a carry
    g_stub.pm = {};
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20030; g_stub.pm.current_ma = 1520; g_stub.pm.power_mw = 30456;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 3; g_stub.caps.spec_rev = 3; g_stub.caps.active_cc = 1;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000};
    g_stub.em.valid = 1; g_stub.em.max_voltage_v = 50; g_stub.em.max_current_a = 5; g_stub.em.max_power_w = 240;
    g_stub.em.speed_str = "USB4 (40G)"; g_stub.em.vendor_str = "Apple"; g_stub.em.vid = 0x05AC;
}

struct Shot { std::vector<uint16_t> px; int strips_done = 0; size_t rle = 0; bool ok = false; };

// Decodes the stream frames queued by capture_screen().
static Shot decode_reports(uint8_t seq) {
    Shot s; s.px.assign(240 * 240, 0xDEAD);
    std::vector<uint8_t> buf[60];
    for (auto &r : g_stub.reports) {
        if (r[0] != 0xA6 || r[1] != 0x4B || r[2] != remote::kCmdScreenData || r[3] != seq) continue;
        const int len = r[5], strip = r[6], flags = r[7];
        if (flags & remote::kShotFlagAbort) return s;
        buf[strip].insert(buf[strip].end(), r.begin() + 8, r.begin() + 6 + len);
        s.rle += len - 2;
        if (flags & remote::kShotFlagLast) {
            size_t n = 0;
            if (!rle565_decode(buf[strip].data(), buf[strip].size(), &s.px[strip * 960], 960, n) || n != 960) return s;
            s.strips_done++;
        }
    }
    s.ok = s.strips_done == 60;
    return s;
}

static int distinct(const uint16_t *p, size_t n) {
    std::vector<uint16_t> v(p, p + n);
    std::sort(v.begin(), v.end());
    return static_cast<int>(std::unique(v.begin(), v.end()) - v.begin());
}

static void dump_ppm(const char *path, const uint16_t *px) {
    const char *dir = std::getenv("K2_DUMP");
    if (!dir) return;
    const char *base = std::strrchr(path, '/');
    char output[256];
    std::snprintf(output, sizeof output, "%s/%s", dir, base ? base + 1 : path);
    FILE *f = fopen(output, "wb");
    if (!f) return;
    fprintf(f, "P6\n240 240\n255\n");
    for (int i = 0; i < 240 * 240; i++) {
        const uint16_t v = px[i];
        const uint8_t c[3] = {static_cast<uint8_t>(((v >> 11) & 31) << 3), static_cast<uint8_t>(((v >> 5) & 63) << 2),
                              static_cast<uint8_t>((v & 31) << 3)};
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}


// ---- Black Box scenarios: real pdtrace recorder fed with synthetic traffic -----------------------
namespace bb {
static bool g_first = true;
constexpr uint16_t kSrcRole = 1u << 8;
static void msg(uint16_t hdr, std::initializer_list<uint32_t> objs = {}, uint8_t token = 0xE0) {
    std::vector<uint8_t> p;
    for (uint32_t o : objs) for (int i = 0; i < 4; i++) p.push_back(uint8_t(o >> (8 * i)));
    p.insert(p.end(), 4, 0);                                   // CRC
    pdtrace::rx(token, hdr, p);
}
static void src_caps67() {          // 5V 9V 12V 15V 20V3.35A, PPS 11V 3A and 21V 3A
    msg(0x7001 | kSrcRole, {0x0001912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x0006414F,
                            0xC0000000u | 110u << 17 | 33u << 8 | 60u, 0xC0000000u | 210u << 17 | 33u << 8 | 60u});
}
static void src_caps15() { msg(0x1001 | kSrcRole, {0x0001912C}); }
static void identity(unsigned vid, unsigned pid) {
    msg(0x4F | kSrcRole | 4u << 12, {0xFF00A041, vid, 0, pid << 16});
}
static bool g_link = false;         // capture() also sniffs a USB4 + TBT3 + DP link and an e-marker
static void link_traffic() {
    constexpr uint32_t kSt = 1u << 15 | 1u << 8, kAck = 1u << 6;
    msg(0x1008 | kSrcRole, {2u << 28 | 3u << 21 | 1u << 19 | 3u << 17});               // Enter_USB: USB4 Gen3
    msg(0x200F | kSrcRole, {0x8087u << 16 | kSt | 4u, 3u << 16 | 1u << 24});            // TBT3 Enter Mode REQ
    msg(0x100F, {0x8087u << 16 | kSt | kAck | 4u});
    msg(0x200F | kSrcRole, {0xFF01u << 16 | kSt | 4u});                                // DP Enter Mode
    msg(0x100F, {0xFF01u << 16 | kSt | kAck | 4u});
    msg(0x200F | kSrcRole, {0xFF01u << 16 | kSt | 17u, 1u << 10 | 2u});                // DP Configure, pin C
    msg(0x100F, {0xFF01u << 16 | kSt | kAck | 17u});
    msg(0x500F, {0xFF00u << 16 | kSt | kAck | 1u, 3u << 27 | 0x2109, 0, 0, 3u | 2u << 5}, 0xC0);   // e-marker
    msg(0x400F, {0xFF00u << 16 | kSt | kAck | 1u, 1u << 31 | 1u << 30 | 0x05AC, 0, 0x1234u << 16}); // partner
}
// One capture: some traffic, then the trigger, then the post-trigger tail. `at` is the trigger time.
static void capture(pdtrace::Trigger t, uint32_t at, bool caps67, int vid, int pid) {
    g_stub.ms = at - 300;
    if (vid >= 0) identity(unsigned(vid), unsigned(pid));
    if (g_link) link_traffic();
    if (caps67) src_caps67(); else src_caps15();
    pdtrace::vbus_sample(g_first ? 5000 : 9000, 0);     // later captures start at 9 V: no VBUS-dip trigger
    g_first = false;
    g_stub.ms += 100;
    msg(0x1282, {0x13042BC8});          // Request
    msg(0x0003, {});                    // Accept
    g_stub.ms += 100;
    pdtrace::vbus_sample(9000, 500);
    msg(0x0006, {});                    // PS_RDY
    g_stub.ms = at;
    if (t == pdtrace::Trigger::Contract) pdtrace::sink(4, 0, false, false, uint8_t(1 + (at / 1000) % 5));
    else if (t == pdtrace::Trigger::HardReset) pdtrace::reset(pdtrace::ResetKind::HardRx);
    else pdtrace::cc(pdtrace::CcEvent::Detach, 1, 0);
    g_stub.ms = at + 600;
    msg(0x0006, {});                    // closes the post-trigger window
    pdtrace::vbus_sample(9000, 500);
}
static void fresh() { pdtrace::init(); pdtrace::clear_captures(); g_first = true; }
}  // namespace bb

static void open_blackbox(Ui &ui, uint32_t t) {
    ui.goto_page(static_cast<uint8_t>(Page::Tools), t);
    ui.push_menu(Menu::BlackBox, t + 10);
    ui.tick(t + 20);
    ui.tick(t + 400);                                  // the readout settles over two samples
}
static void close_blackbox(Ui &ui, uint32_t t) {
    while (ui.menu_depth()) ui.on_button(Button::Back, ButtonEvent::Short, t);
}

static void test_screen(Ui &ui, const char *name, uint32_t now_ms);
// A screen that shows the same content must not touch the LCD: ticks seconds apart add no windows.
static void check_steady(Ui &ui, const char *what, uint32_t t) {
    ui.tick(t);
    const unsigned w0 = g_lcd_windows;
    for (int i = 1; i <= 4; i++) { g_stub.ms += 1500; ui.tick(t + 1500u * i); }
    printf("%s: %u LCD windows over 6 s\n", what, g_lcd_windows - w0);
    CHECK_EQ(g_lcd_windows - w0, 0u);
}
static Ui *g_ui_for_hook = nullptr;   // the scan hook of the stub captures through it

static k2::pd::Contract pd_request(int pos, int mv, int ma) {
    k2::pd::Contract c;
    c.valid = true; c.ready = true; c.pos = uint8_t(pos); c.mv = uint16_t(mv); c.op_ma = uint16_t(ma); c.max_ma = uint16_t(ma);
    return c;
}

// Counts pixels of exactly the given colours inside a rectangle.
static int count_colours(int x0, int y0, int x1, int y1, std::initializer_list<uint16_t> cols) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            for (uint16_t c : cols) n += g_panel[y * 240 + x] == c;
    return n;
}
// Stale Home is grey end to end: no measurement colour in the readouts, the strips or the energy row;
// the status bar below y 18 is untouched by this check.
static void check_home_all_grey(const char *what) {
    const int n = count_colours(0, 20, 224, 222, {k2::kVolt, k2::kAmp, k2::kWatt, k2::kVoltT, k2::kAmpT, k2::kWattT,
                                                  k2::kEnergy, k2::kCharge});
    printf("%s: %d measurement-colour pixels on the stale Home\n", what, n);
    CHECK_EQ(n, 0);
}


// A status-bar title of `chars` characters (12 px font, advance 6, from x 20) must show its last glyph:
// the readout used to wipe the bar from x 80 on, so "PD trigger" came out as "PD trigge".
static void check_title_intact(const char *what, int chars, int x_first = 20) {
    const int x0 = x_first + 6 * (chars - 1);
    int ink = 0;
    for (int y = 3; y < 15; y++)
        for (int x = x0; x < x0 + 6; x++) ink += g_panel[y * 240 + x] != k2::kBar;
    printf("%s: %d ink pixels in the last of %d title characters\n", what, ink, chars);
    CHECK_EQ(ink > 4, 1);
}

static void test_page(Ui &ui, uint8_t page, const char *name) {
    ui.goto_page(page, 5000);
    test_screen(ui, name, 5260);   // 5260 and the capture's +100 share one 250 ms chevron step
}

static void test_menu(Ui &ui, Menu m, const char *name) {
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 5000);
    ui.push_menu(m, 5100);
    test_screen(ui, name, 5200);
}

// Captures whatever is showing (page or submenu) and compares the stream with the LCD.
static void test_screen(Ui &ui, const char *name, uint32_t now_ms) {
    ui.tick(now_ms);                                 // fills the dynamic fields from the stubs
    std::vector<uint16_t> direct(g_panel, g_panel + 240 * 240);
    g_stub.reports.clear();
    capture_screen(ui, 7, now_ms);   // same instant: the flow-rail phase is time based, so it must not step
    if (const char *d = std::getenv("K2_DUMP")) {
        char rp[96]; snprintf(rp, sizeof rp, "%s/k2_%s.raw", d, name);
        if (FILE *f = fopen(rp, "wb")) {
            for (auto &r : g_stub.reports) fwrite(r.data(), 1, 64, f);
            fclose(f);
        }
    }
    if (const char *d = std::getenv("K2_DUMP")) {       // expected pixels, for tests/test_k2_remote.py
        char pp[96]; snprintf(pp, sizeof pp, "%s/k2_%s.panel", d, name);
        if (FILE *f = fopen(pp, "wb")) { fwrite(direct.data(), 2, direct.size(), f); fclose(f); }
    }
    Shot s = decode_reports(7);
    CHECK_EQ(s.ok, 1);
    int diff = 0, first = -1;
    for (int i = 0; i < 240 * 240; i++)
        if (s.px[i] != direct[i]) { diff++; if (first < 0) first = i; }
    printf("%s: %d strips, %zu RLE bytes, %d distinct colours, %d pixels differ", name, s.strips_done, s.rle,
           distinct(direct.data(), direct.size()), diff);
    if (first >= 0) printf(" (first at x=%d y=%d: capture %04x direct %04x)", first % 240, first / 240, s.px[first], direct[first]);
    printf("\n");
    CHECK_EQ(diff, 0);
    CHECK_EQ(distinct(direct.data(), direct.size()) > 6, 1);          // the page really has content
    // The LCD must be unchanged by the capture.
    int touched = 0;
    int tfirst = -1;
    for (int i = 0; i < 240 * 240; i++) if (g_panel[i] != direct[i]) { touched++; if (tfirst < 0) tfirst = i; }
    if (touched) printf("%s: LCD changed by the capture at %d pixels (first x=%d y=%d)\n", name, touched, tfirst % 240, tfirst / 240);
    CHECK_EQ(touched, 0);
    char p[64];
    snprintf(p, sizeof p, "host-build/k2_%s_direct.ppm", name); dump_ppm(p, direct.data());
    snprintf(p, sizeof p, "host-build/k2_%s_shot.ppm", name);   dump_ppm(p, s.px.data());
}

static void fill_seven() {
    g_stub.caps.num_pdos = 7;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000};
    g_stub.caps.pdos[3] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    g_stub.caps.pdos[4] = {pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000};
    g_stub.caps.pdos[5] = {pd::PdoType::Pps, 0, 3300, 21000, 5000, 100000};
    g_stub.caps.pdos[6] = {pd::PdoType::Avs, 0, 15000, 28000, 5000, 140000};
}

// 11 positions: the seven above plus EPR fixed 28/36/48 V and EPR AVS 15-48 V.
static void fill_epr() {
    fill_seven();
    g_stub.caps.num_pdos = 11;
    g_stub.caps.epr = true;
    g_stub.caps.pdos[7] = {pd::PdoType::Fixed, 0, 28000, 28000, 5000, 140000};
    g_stub.caps.pdos[8] = {pd::PdoType::Fixed, 0, 36000, 36000, 5000, 180000};
    g_stub.caps.pdos[9] = {pd::PdoType::Fixed, 0, 48000, 48000, 5000, 240000};
    g_stub.caps.pdos[10] = {pd::PdoType::Avs, 0, 15000, 48000, 5000, 240000};
}

static void test_charger_wizard_scenes() {
    const auto old = g_stub;
    g_stub = {};
    g_stub.pm.vbus_mv = 5000;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1;
    g_stub.caps.raw_header = 0x1181; g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = pd::decode_pdo(0x0001912C);
    g_stub.source_revision = {.valid = true, .rev_major = 3, .rev_minor = 2, .ver_major = 1, .ver_minor = 7};
    const uint32_t vdos[] = {0xFF00A041, (3u << 27) | 0x05ACu, 0, 0x12340005, (2u << 21) | (3u << 9) | (2u << 5)};
    emlib::Entry entry; entry.seq = 1; entry.version = 1; entry.rec = emrec::make(vdos, 2);
    std::snprintf(entry.name.data(), entry.name.size(), "Host passive marker");
    emlib::Entry longer = entry;   // a 24-cell name, the store's longest: the list must show it whole beside its "saved" word
    longer.seq = 2;
    std::snprintf(longer.name.data(), longer.name.size(), "Anker 240W USB4 e-marker");
    g_stub.lib = {entry, longer};
    static Ui wizard;
    wizard.init(1000); wizard.tick(1000);
    wizard.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100);
    uint32_t now = 1200;
    const auto press = [&](Button b) { wizard.on_button(b, ButtonEvent::Short, now); };
    const auto move = [&](unsigned n) { for (unsigned i = 0; i < n; ++i) press(Button::Plus); };
    // Every scene names the ChargerTestPage::Stage it must show; a change of the flow fails here instead of
    // silently retitling a scene (the Cable identity dialog once turned "ct_results" into Review).
    enum : int { kTests, kGroup, kCable, kReview = 4, kRunning, kResults, kSteps, kDetail, kCustom = 14 };
    const auto shot = [&](const char* name, int stage) {
        CHECK_EQ(wizard.charger_test_stage(), stage);
        test_screen(wizard, name, now); now += 300;
    };
    shot("ct_tests", kTests);
    const auto top = [&] { for (unsigned i = 0; i < charger_test::kMaxSteps; ++i) press(Button::Minus); };   // rows are 0-based: there is no Next row
    const auto long_ok = [&] { wizard.on_button(Button::Ok, ButtonEvent::Long, now); };
    move(4); press(Button::Ok); shot("ct_custom", kCustom);          // "Custom..." opens the per-family list
    move(4); long_ok(); shot("ct_pd_algorithms", kGroup);           // the USB PD family row, long OK: PD fixed + PPS
    move(1); shot("ct_pd_pps", kGroup);
    press(Button::Back); top(); move(6); press(Button::Ok); shot("ct_cable", kCable);   // Done (row 6) goes on to the cable
    top(); move(5); shot("ct_cable_identity", kCable);               // the saved clone is the sixth row; a pick selects and advances
    wizard.set_rotation(1, now); shot("ct_cable_identity_rot90", kCable); wizard.set_rotation(0, now);
    press(Button::Plus); shot("ct_cable_long_name", kCable); press(Button::Minus);   // the 24-cell clone under the cursor, then back
    press(Button::Ok); shot("ct_review", kReview);                    // Review shows the chosen identity; nothing is armed yet
    CHECK_EQ(g_stub.spoof_arms, 0);
    g_stub.spoof_err = emk_spoof::Err::Inline; wizard.tick(now); press(Button::Ok); press(Button::Ok);   // the second START arms the clone: refused
    shot("ct_spoof_refused", kReview); CHECK_EQ(g_stub.spoof_on, false);
    g_stub.spoof_err = emk_spoof::Err::Ok; wizard.tick(now);
    press(Button::Back); top(); press(Button::Ok); wizard.tick(now);   // back on the list: pick OFF (the top row), which advances to Review
    press(Button::Ok); shot("ct_confirmation", kReview);   // OFF: START opens the confirm window on Review (no dialog)
    press(Button::Ok); shot("ct_running", kRunning);
    press(Button::Back);
    for (unsigned i = 0; i < 10; ++i) { now += 100; wizard.tick(now); }
    shot("ct_results", kResults);
    move(1); press(Button::Ok); shot("ct_result_steps", kSteps);   // OK on the second unit line opens its steps
    press(Button::Ok); shot("ct_result_detail", kDetail);
    CHECK_EQ(g_stub.requests + g_stub.fc_starts + g_stub.scans + g_stub.lib_saves, 0);
    CHECK_EQ(g_stub.spoof_on, false);
    wizard.set_rotation(1, now); shot("ct_detail_rot90", kDetail);
    wizard.set_rotation(0, now);
    g_stub = old;
}

// The review gallery of the three-step wizard and Tools -> Spoof, on the same shared cable list, at both landscape rotations
// (0 and 3): K2_DUMP=dir host-build/test_ui_capture, then tools/panel2png.py dir out wiz3_*. The Spoof page arms a built-in chip,
// so the Cable step that follows shows the same ARMED marker (one list, one marker).
// The Connection review gallery: the three tabs filled and empty (the port-aware hints), the action pop-up per tab and Tools,
// at both landscape rotations. K2_DUMP=dir host-build/test_ui_capture, then the k2_conn_gal_*_direct.ppm files.
static void test_connection_gallery() {
    const auto old = g_stub;
    for (const uint8_t rot : {uint8_t{0}, uint8_t{3}}) {
        uint32_t now = 20000;
        char name[64];
        std::unique_ptr<Ui> ui;
        const auto fresh = [&] {
            ui = std::make_unique<Ui>();
            g_stub = {};
            g_stub.ms = now;
            g_stub.pm_online = true;
            g_stub.cc = 0;                                             // nothing attached: the link detaches after the debounce
            ui->init(now);
            ui->set_rotation(rot, now);
            ui->configure_rotation(false, rot);                        // locked: the stub's gravity must not rotate it back
            for (int i = 0; i < 16; i++) ui->tick(now += 100);
        };
        const auto cc = [&](int a, int b) { adc::Reading r; r.cc1_mv = uint16_t(a); r.cc2_mv = uint16_t(b); g_stub.adc = r; };
        const auto shot = [&](const char *scene) {
            std::snprintf(name, sizeof name, "conn_gal_rot%u_%s", rot == 0 ? 0u : 3u, scene);
            for (int i = 0; i < 3; i++) ui->tick(now += 100);          // a rotated page repaints over a few ticks
            test_screen(*ui, name, now += 100);
        };
        const auto open = [&](unsigned tab) {
            ui->goto_page(uint8_t(kConnTabAlias + tab), now += 100);
            for (int i = 0; i < 12; i++) ui->tick(now += 100);
        };
        const auto popup = [&](const char *scene) {
            ui->on_button(Button::Ok, ButtonEvent::Short, now += 100);
            shot(scene);
            ui->on_button(Button::Back, ButtonEvent::Short, now += 100);
        };
        // Empty states: each combination of tab and port topology
        fresh(); cc(0, 0);        open(0); shot("source_empty");
        fresh(); cc(3300, 3300);  open(2); shot("sink_empty_top_plug");
        open(1); shot("cable_empty_top_plug");
        fresh(); cc(3300, 0);     open(2); shot("sink_empty_via_cable");
        open(1); shot("cable_empty_via_cable");
        // showing an empty state never drove the lines (the stub was reset by the last fresh())
        CHECK_EQ(g_stub.ra_ons + g_stub.ra_offs + g_stub.rd_pauses + g_stub.scans + g_stub.hard_resets + g_stub.getcaps, 0);
        // Filled: a 140 W source on the line, a read cable, a device identified inline
        fresh();
        fill_seven();
        g_stub.pm.vbus_mv = 20030; g_stub.pm.current_ma = 1520; g_stub.pm.power_mw = 30456;
        g_stub.caps.valid = true; g_stub.caps.spec_rev = 3; g_stub.caps.active_cc = 1;
        g_stub.source_revision = {.valid = true, .rev_major = 3, .rev_minor = 2, .ver_major = 1, .ver_minor = 7};
        g_stub.em.valid = 1; g_stub.em.max_voltage_v = 50; g_stub.em.max_current_a = 5; g_stub.em.max_power_w = 240;
        g_stub.em.speed_str = "USB4 (40G)"; g_stub.em.vendor_str = "Apple"; g_stub.em.vid = 0x05AC;
        g_stub.em.is_active = false; g_stub.em.epr_capable = true; g_stub.em.sop = 1; g_stub.em.spec_rev = 2;
        ui->debug_link(true, now += 100);
        for (int i = 0; i < 4; i++) ui->tick(now += 100);
        open(0); shot("source_filled"); popup("source_popup");
        open(1); shot("cable_filled"); popup("cable_popup");
        g_stub.sink_vdm.identity.valid = true; g_stub.sink_vdm.identity.vid = 0x05AC; g_stub.sink_vdm.identity.pid = 0x1A2B;
        g_stub.sink_vdm.identity.vendor = "Apple"; g_stub.sink_vdm.identity.model = "iPad Air 5";
        g_stub.sink_vdm.num_svids = 2; g_stub.sink_vdm.svids[0] = 0x05AC; g_stub.sink_vdm.svids[1] = 0x8087;
        g_stub.sink_vdm.identity.id_header = 0xC0000000u | (2u << 27);
        g_stub.ident.state = k2::pd::IdState::Done; g_stub.ident.ok_mask = 0x1F; g_stub.ident.revision = 0x30000000;
        open(2); shot("sink_filled"); popup("sink_popup");
        // Tools: the new row order
        ui->goto_page(static_cast<uint8_t>(Page::Tools), now += 100);
        ui->tick(now += 100);
        shot("tools");
    }
    g_stub = old;
}

static void test_wizard_gallery() {
    const auto old = g_stub;
    for (const uint8_t rot : {uint8_t{0}, uint8_t{3}}) {
        g_stub = {};
        g_stub.pm_online = true;
        g_stub.pm.vbus_mv = 5000;
        g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1;
        g_stub.caps.raw_header = 0x1181; g_stub.caps.num_pdos = 1;
        g_stub.caps.pdos[0] = pd::decode_pdo(0x0001912C);
        g_stub.source_revision = {.valid = true, .rev_major = 3, .rev_minor = 2, .ver_major = 1, .ver_minor = 7};
        const uint32_t vdos[] = {0xFF00A041, (3u << 27) | 0x05ACu, 0, 0x12340005, (2u << 21) | (3u << 9) | (2u << 5)};
        emlib::Entry entry; entry.seq = 1; entry.version = 1; entry.rec = emrec::make(vdos, 2);
        std::snprintf(entry.name.data(), entry.name.size(), "Host passive marker");
        g_stub.lib = {entry};
        static Ui ui;
        uint32_t now = 1000;
        ui.init(now); ui.set_rotation(rot, now); ui.tick(now);
        ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), now += 100);
        char name[48];
        const auto shot = [&](const char *step) {
            std::snprintf(name, sizeof name, "wiz3_rot%u_%s", rot == 0 ? 0u : 3u, step);
            test_screen(ui, name, now);
            now += 300;
        };
        const auto press = [&](Button b) { ui.on_button(b, ButtonEvent::Short, now); now += 10; };
        const auto move = [&](unsigned n) { for (unsigned i = 0; i < n; ++i) press(Button::Plus); };
        const auto top = [&] { for (unsigned i = 0; i < charger_test::kMaxSteps; ++i) press(Button::Minus); };
        ui.tick(now += 100);
        shot("1_tests");
        move(2); press(Button::Ok);                                   // PD only: the USE pill moves
        move(3); press(Button::Ok); shot("1_custom");                 // Custom...
        press(Button::Back); top(); press(Button::Ok); shot("2_cable");
        move(1); shot("2_cable_cursor");
        move(5); press(Button::Ok); shot("2_cable_clone");            // the saved clone is picked: USE
        top(); press(Button::Ok); shot("3_review");
        press(Button::Ok); shot("3_review_confirm");                  // the first START OK: the confirm window, nothing touched
        CHECK_EQ(g_stub.spoof_arms, 0);
        press(Button::Back);                                          // closes the window only
        // Tools -> Spoof: arm the first built-in chip at once, then the wizard's Cable step shows the same marker.
        ui.goto_page(static_cast<uint8_t>(Page::Tools), now += 100);
        ui.tick(now += 100);
        CHECK_EQ(ui.push_menu(Menu::Spoof, now += 100), true);
        ui.tick(now += 100);
        shot("spoof_idle");
        move(1); press(Button::Ok); ui.tick(now += 100);
        CHECK_EQ(g_stub.spoof_arms, 1);
        shot("spoof_armed");
        ui.pop_menu(now += 100);
        ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), now += 100);
        ui.tick(now += 100);
        top(); press(Button::Ok);                                     // Tests -> Cable (the wizard resumes on the Tests step)
        shot("2_cable_armed");
    }
    g_stub = old;
}

static void test_spoof_focus_and_active() {
    const auto old = g_stub;
    g_stub = {};
    const uint32_t v1[] = {0xFF00A041, 0x1800203C, 123, 0x10000, 0x00182052};
    const uint32_t v2[] = {0xFF00A041, 0x1800203C, 123, 0x20000, 0x00182052};
    emlib::Entry a, b;
    a.seq = 1; a.rec = emrec::make(v1, 2); std::snprintf(a.name.data(), a.name.size(), "First");
    b.seq = 2; b.rec = emrec::make(v2, 2); std::snprintf(b.name.data(), b.name.size(), "Second");
    g_stub.lib = {a, b};
    for (unsigned n = 3; n <= 6; ++n) {
        emlib::Entry e = b; e.seq = n;
        std::snprintf(e.name.data(), e.name.size(), "Cable %u", n);
        g_stub.lib.push_back(e);
    }
    static Ui ui;
    ui.init(1000); ui.set_rotation(0, 1000); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::Tools), 1100);
    ui.push_menu(Menu::Spoof, 1200);
    test_screen(ui, "spoof_off_instructions", 1250);
    constexpr int kList = SpoofPage::kListY;      // the identity list starts under the two key/value rows
    g_stub.spoof_on = true; g_stub.spoof_record = a.rec; ++g_stub.spoof_generation;
    test_screen(ui, "spoof_active_focus", 1300);   // armed from outside the list ("external identity"): no row is marked
    CHECK_EQ(g_panel[(kList + 10) * 240 + 1], kFocus);       // the cursor rail of row 0 (Off)
    CHECK_EQ(g_panel[(kList + 10) * 240 + 160], kFocusT);
    ui.on_button(Button::Plus, ButtonEvent::Short, 1400);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1410);   // row 1 (the first built-in chip): select and arm
    ui.on_button(Button::Plus, ButtonEvent::Short, 1420);
    test_screen(ui, "spoof_armed_unfocused", 1500);
    CHECK_EQ(g_panel[(kList + 2 * 26 + 10) * 240 + 1], kFocus);   // the cursor moved on to row 2, the armed row 1 is not focused
    unsigned amber = 0;                                   // the armed row's name is amber (kCaution), as on the wizard's Cable step
    for (int y = kList + 26 + 5; y < kList + 26 + 5 + kSmall.height; ++y)
        for (int x = 28; x < 110; ++x) amber += g_panel[y * 240 + x] == kCaution;
    CHECK_EQ(amber > 0, true);
    for (int i = 0; i < 8; ++i) ui.on_button(Button::Plus, ButtonEvent::Short, 1600);   // row 2 -> row 10, the last of 11
    test_screen(ui, "spoof_scrolled", 1700);
    ui.tick(1800);   // let the 160 ms thumb easing settle before the pixel checks
    CHECK_EQ(g_panel[(kList + 4 * 26 + 10) * 240 + 1], kFocus);   // the window followed the cursor: it sits on the last visible row
    const char *spoof_rot[] = {"", "spoof_scrolled_rot90", "spoof_scrolled_rot180", "spoof_scrolled_rot270"};
    for (uint8_t r = 1; r < 4; ++r) {
        const uint32_t t = 1800 + r * 300;
        ui.set_rotation(r, t); test_screen(ui, spoof_rot[r], t + 100);
    }
    ui.set_rotation(0, 3100);
    CHECK_EQ(g_stub.spoof_arms, 1);              // the one OK press armed the chip; nothing else did
    CHECK_EQ(g_stub.spoof_disarms, 0);
    g_stub = old;
}

// Tools > Report cards: empty state, list, one card, a worst-case card, the delete confirm, rotated.
static void test_cards_scenes() {
    namespace rc = report_card;
    namespace ct = charger_test;
    const auto old = g_stub;
    g_stub = {};
    stub_cards_reset();
    static Ui ui;
    ui.init(1000); ui.set_rotation(0, 1000); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::Tools), 1100);
    ui.push_menu(Menu::Cards, 1200);
    test_screen(ui, "cards_empty", 1250);
    ui.on_button(Button::Back, ButtonEvent::Short, 1300);

    rc::Card a;
    a.id = {0x1A2B3C4D, 0x0B05, 0x1234};
    a.num_pdos = 5; a.spec_rev = 2; a.boots = 4; a.runs = 3; a.fw = 0x0FF2C610;
    a.group[0] = {rc::GV::Pass, 20000, 0, 0};
    a.group[1] = {rc::GV::Pass, 12000, 0, 0};
    a.group[2] = {rc::GV::Fail, 0, 0, uint8_t(ct::Reason::Timeout)};
    a.group[4] = {rc::GV::Skip, 0, 0, uint8_t(ct::Reason::NoOffer)};
    a.n_ranges = 2;
    a.range[0] = {uint8_t(ct::Algorithm::Pps), 0, 33, 210, 200, 130};
    a.range[1] = {uint8_t(ct::Algorithm::SprAvs), 0, 150, 280, 280, 0};
    a.handover = ct::HandoverOutcome::FellBack; a.handover_ds = 32;
    rc::set_name(a, "Anker 735");
    rc::hw::put(a);

    rc::Card w = a;                                   // worst case: every group, four ranges, long name
    w.id = {0xDEADBEEF, 0x2CA3, 0x4321};
    w.boots = 987; w.runs = 12;
    for (int i = 0; i < 12; ++i) {
        const rc::GV v = i % 3 == 0 ? rc::GV::Pass : i % 3 == 1 ? rc::GV::Fail : rc::GV::Skip;
        w.group[i] = {v, uint16_t(v == rc::GV::Pass ? 20000 : 0), 0, uint8_t(v == rc::GV::Pass ? 0 : int(ct::Reason::Timeout))};
    }
    w.group[9] = {rc::GV::Fail, 12000, 0, uint8_t(ct::Reason::Timeout), 20000};   // SVOOC65: the widest cell, "12V" cross "20V"
    w.n_ranges = 4;
    w.range[2] = {uint8_t(ct::Algorithm::Pps), 0, 33, 110, 110, 90};
    w.range[3] = {uint8_t(ct::Algorithm::EprAvs), 0, 150, 480, 480, 0};
    w.handover = ct::HandoverOutcome::StayedPd; w.handover_ds = 125;
    rc::set_name(w, "Very long charger nm");
    rc::hw::put(w);
    rc::Card f;
    f.id = {0x55667788, 0, 0}; f.boots = 2; f.runs = 1;
    f.group[0] = {rc::GV::Fail, 0, 0, uint8_t(ct::Reason::Timeout)};
    rc::hw::put(f);

    ui.push_menu(Menu::Cards, 1400);
    test_screen(ui, "cards_list", 1450);
    ui.on_button(Button::Plus, ButtonEvent::Short, 1500);
    test_screen(ui, "cards_list_second", 1550);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1600);
    test_screen(ui, "cards_detail_worst", 1650);
    ui.on_button(Button::Plus, ButtonEvent::Short, 1700);
    test_screen(ui, "cards_detail", 1750);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1760);       // the focused row's sub-screen (the PPS range)
    test_screen(ui, "cards_group_range", 1860);               // page ticks are 100 ms apart: keep every scene that far from the last
    ui.on_button(Button::Plus, ButtonEvent::Short, 1870);     // next row: a refresh of the whole sub-screen
    test_screen(ui, "cards_group_range2", 1970);
    ui.on_button(Button::Back, ButtonEvent::Short, 1980);
    ui.on_button(Button::Back, ButtonEvent::Short, 1985);     // detail -> list
    ui.on_button(Button::Minus, ButtonEvent::Short, 1990);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1995);       // newest card: one failed group, no name
    test_screen(ui, "cards_detail_fail", 2100);
    ui.on_button(Button::Ok, ButtonEvent::Short, 2110);
    test_screen(ui, "cards_group_pd", 2210);
    ui.on_button(Button::Back, ButtonEvent::Short, 2220);
    ui.on_button(Button::Back, ButtonEvent::Long, 2230);
    test_screen(ui, "cards_confirm", 2330);
    const char *rot[] = {"", "cards_confirm_rot90", "cards_confirm_rot180", "cards_confirm_rot270"};
    for (uint8_t r = 1; r < 4; ++r) {
        const uint32_t t = 2400 + r * 300;
        ui.set_rotation(r, t); test_screen(ui, rot[r], t + 100);
    }
    ui.set_rotation(0, 3400);
    ui.on_button(Button::Back, ButtonEvent::Short, 3500);   // cancel
    ui.on_button(Button::Back, ButtonEvent::Short, 3550);   // detail -> list
    test_screen(ui, "cards_list_rot0_again", 3600);
    stub_cards_reset();
    g_stub = old;
}

// FH22 (card #18) on the K2: the same tones as the Results grid. PD fixed and AVS green, PPS amber "caps 19.4V", the protocols the
// charger never answered grey. No red anywhere: nothing answered and then failed.
static void test_cards_fh22_scenes() {
    const auto old = g_stub;
    g_stub = {};
    stub_cards_reset();
    static Ui ui;
    ui.init(1000); ui.set_rotation(0, 1000); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::Tools), 1100);
    report_card::Card fh = cardfx::fh22_full();
    report_card::hw::put(fh);
    ui.push_menu(Menu::Cards, 1200);
    test_screen(ui, "cards_fh22_list", 1300);
    CHECK_EQ(count_colours(0, 30, 240, 240, {k2::kError}), 0);      // the row summary reads "2P 1~", not "2P 9F"
    CHECK_EQ(count_colours(0, 30, 240, 240, {k2::kCaution}) > 0, 1);   // the row's marker: amber, the Results grid's roll-up
    ui.on_button(Button::Ok, ButtonEvent::Short, 1400);
    test_screen(ui, "cards_fh22_detail", 1500);
    CHECK_EQ(count_colours(0, 30, 240, 240, {k2::kError}), 0);
    CHECK_EQ(count_colours(0, 30, 240, 240, {k2::kOkT}) > 0, 1);          // the AVS row's green cell (the PD row sits on the focus bar)
    CHECK_EQ(count_colours(0, 30, 240, 240, {k2::kCautionT}) > 0, 1);     // the PPS row's amber cell, "caps 19.4V"
    ui.on_button(Button::Plus, ButtonEvent::Short, 1600);          // PPS row
    ui.on_button(Button::Ok, ButtonEvent::Short, 1610);
    test_screen(ui, "cards_fh22_pps", 1720);
    ui.on_button(Button::Back, ButtonEvent::Short, 1730);
    ui.on_button(Button::Back, ButtonEvent::Short, 1740);
    ui.on_button(Button::Back, ButtonEvent::Short, 1750);
    stub_cards_reset();
    g_stub = old;
}

static void test_emarker_menu_scenes() {
    const auto old = g_stub;
    g_stub = {}; g_stub.cc = 0; g_stub.pm.vbus_mv = 0;
    g_stub.em.valid = true; g_stub.em.num_vdos = 5; g_stub.em.spec_rev = 2;
    g_stub.em.raw_vdos = {0xFF00A041, (3u << 27) | 0x05ACu, 0, 0x12340005, (2u << 21) | (3u << 9) | (2u << 5)};
    for (unsigned n = 1; n <= 6; ++n) {
        emlib::Entry e; e.seq = n; e.rec = emrec::make(g_stub.em.raw_vdos, 2);
        std::snprintf(e.name.data(), e.name.size(), "Long cable identity %u", n);
        g_stub.lib.push_back(e);
    }
    static Ui ui;
    ui.init(1000); ui.set_rotation(0, 1000); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::Tools), 1100);
    ui.goto_page(kConnTabAlias + 1, 1200);                        // Connection, Cable tab
    ui.run_action(Action::EmarkerScan, 1300); ui.tick(1350);
    test_screen(ui, "conn_cable", 1380);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1400);           // the action pop-up
    test_screen(ui, "conn_cable_popup", 1450);
    CHECK_EQ(g_panel[50 * 240 + 1], kFocus);                      // the first action row carries the focus bar
    for (int i = 0; i < 3; ++i) ui.on_button(Button::Plus, ButtonEvent::Short, 1500 + i);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1550);           // Saved cables
    test_screen(ui, "em_saved", 1600);
    CHECK_EQ(g_panel[30 * 240 + 1], kFocus);                      // the kit RowList: cursor bar on the first cable
    CHECK_EQ(g_panel[30 * 240 + 160], kFocusT);
    CHECK_EQ(g_panel[60 * 240 + 160], kBg);                       // edge to edge, no card behind the other rows
    for (int i = 0; i < 5; ++i) ui.on_button(Button::Plus, ButtonEvent::Short, 1700);
    test_screen(ui, "em_saved_scrolled", 1800);
    CHECK_EQ(g_panel[(24 + 5 * 26 + 6) * 240 + 1], kFocus);       // six cables fit the window: the cursor moved down to row 5
    CHECK_EQ(g_panel[30 * 240 + 1], kBg);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1900); test_screen(ui, "em_saved_detail", 1950);
    ui.on_button(Button::Ok, ButtonEvent::Short, 2000); test_screen(ui, "em_saved_confirm", 2050);
    ui.on_button(Button::Back, ButtonEvent::Short, 2100);
    ui.on_button(Button::Back, ButtonEvent::Short, 2150);
    const char *em_rot[] = {"", "em_saved_rot90", "em_saved_rot180", "em_saved_rot270"};
    for (uint8_t r = 1; r < 4; ++r) {
        const uint32_t t = 2200 + r * 300;
        ui.set_rotation(r, t); test_screen(ui, em_rot[r], t + 100);
    }
    ui.set_rotation(0, 3400);
    CHECK_EQ(g_stub.lib_saves + g_stub.lib_removes + g_stub.spoof_arms, 0);
    g_stub = old;
}

int main() {
    setup_stubs();
    k2::g_missing_glyphs = 0;
    g_stub.pm.vbus_mv = 20000;                       // a live charger, so the link stays attached
    static Ui ui;
    ui.init(1000); ui.set_rotation(0, 1000);
    test_page(ui, 0, "home");
    test_page(ui, static_cast<uint8_t>(Page::ChargerTest), "chargertest");
    test_page(ui, kPdPage, "pd");
    test_page(ui, kDiagPage, "diag");
    test_page(ui, static_cast<uint8_t>(Page::Lines), "lines");
    test_page(ui, static_cast<uint8_t>(Page::Tools), "tools");
    // Power-flow arrow glyphs (UTF-8 in the 24 px font) survive the strip-wise capture.
    g_stub.pm.current_ma = -1520; g_stub.pm.power_mw = -30456; g_stub.pm.flow = k2::power::Flow::Reverse;
    test_page(ui, 0, "home_reverse");
    g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0; g_stub.pm.flow = k2::power::Flow::Idle;
    test_page(ui, 0, "home_idle");
    g_stub.pm.current_ma = 1520; g_stub.pm.power_mw = 30456; g_stub.pm.flow = k2::power::Flow::Forward;
    test_page(ui, 0, "home_forward");
    ui.goto_page(0, 5000);
    ui.set_pressed(k2::Button::Back, true, 5000);
    ui.tick(5100);
    ui.tick(7000);                                                  // BACK held 2 s: "Reboot in 3"
    CHECK_EQ(ui.hold_card_visible() ? 1 : 0, 1);
    {   // The page beneath is frozen while the card shows, so only the card itself is checked here.
        std::vector<uint16_t> direct(g_panel, g_panel + 240 * 240);
        int red = 0;
        for (int y = 70; y < 154; y++)
            for (int x = 12; x < 212; x++) red += direct[y * 240 + x] == k2::kError;
        CHECK_EQ(red > 300, 1);   // frame + warning icon + progress fill
        dump_ppm("host-build/k2_home_reboot_hold_direct.ppm", direct.data());
    }
    ui.set_pressed(k2::Button::Back, false, 7100);
    ui.tick(7200);
    test_page(ui, 0, "home_novbus");
    g_stub.pm_online = false;
    test_page(ui, 0, "home_error");
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20000; g_stub.pm.current_ma = 1520; g_stub.pm.power_mw = 30456; g_stub.pm.flow = k2::power::Flow::Forward;
    g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0; g_stub.pm.flow = k2::power::Flow::Idle;   // not inline from here on
    ui.tick(5200);
    // Large counters that carry into new digits (and wrap) while the capture bumps TX: still one value.
    k2::usb::stats.log_sent = 99990; k2::usb::stats.log_dropped = 4294967290u; k2::usb::stats.resets = 12345;
    test_page(ui, kDiagPage, "diag_big");
    k2::usb::stats.log_sent = 4294967200u;
    test_page(ui, kDiagPage, "diag_huge");
    // PD page with 7 PDOs: two-line rows scroll through the source order.
    fill_seven();
    test_page(ui, kPdPage, "pd7");
    g_stub.contract = pd_request(4, 20000, 5000);                    // asks 5 A of a 5 A PDO: matches
    test_page(ui, kPdPage, "match7");
    g_stub.contract = pd_request(2, 9000, 4000);                      // wants more than PDO2 gives
    test_page(ui, kPdPage, "match_limited");
    g_stub.contract = pd_request(9, 20000, 3000);                     // position beyond the offer
    test_page(ui, kPdPage, "match_outside");
    // Sink caps seen (9 V 3 A and 15 V 4 A): the sink column judges every offer, the request row is linked.
    g_stub.contract = pd_request(3, 15000, 3000);
    g_stub.sink_seen.valid = true;
    g_stub.sink_seen.n = 2;
    g_stub.sink_seen.raw[0] = (uint32_t(9000 / 50) << 10) | uint32_t(3000 / 10);
    g_stub.sink_seen.raw[1] = (uint32_t(15000 / 50) << 10) | uint32_t(4000 / 10);
    test_page(ui, kPdPage, "match_sink_caps");
    g_stub.sink_seen = {};
    g_stub.contract = {};
    {   // The sink's wants list: the Pixel's three PDOs (hardware) against a 67 W charger, then the no-list case.
        const auto saved_caps = g_stub.caps;
        g_stub.session++;                                            // a new charger: the offers book starts over
        g_stub.caps.num_pdos = 5;
        g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
        g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
        g_stub.caps.pdos[2] = {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000};
        g_stub.caps.pdos[3] = {pd::PdoType::Fixed, 0, 20000, 20000, 3350, 67000};
        g_stub.caps.pdos[4] = {pd::PdoType::Pps, 0, 3300, 11000, 5500, 60500};
        g_stub.contract = pd_request(2, 9000, 3000);
        g_stub.sink_seen.valid = true;
        g_stub.sink_seen.n = 3;
        g_stub.sink_seen.raw = {0x3601912C, 0x0002D12C, 0xC0DC323C, 0, 0, 0, 0};
        test_page(ui, kPdPage, "wants_pixel_sniffed");
        g_stub.sink_seen = {};                                       // the same list, asked for by Identify in this attach
        g_stub.ident.ok_mask = uint8_t(1u << int(pd::IdStep::SinkCap));
        g_stub.ident.num_sink_pdos = 3;
        g_stub.ident.sink_pdos = {0x3601912C, 0x0002D12C, 0xC0DC323C, 0, 0, 0, 0};
        g_stub.ident_current = true;
        test_page(ui, kPdPage, "wants_pixel_identify");
        g_stub.ident_current = false;                                // an earlier attach: not this device's list
        test_page(ui, kPdPage, "wants_none");
        const int ids = g_stub.identifies;
        ui.on_button(Button::Ok, ButtonEvent::Short, 6500);          // OK opens the Source pop-up (Identify lives on the Sink tab)
        CHECK_EQ(g_stub.identifies, ids);
        ui.on_button(Button::Back, ButtonEvent::Short, 6510);        // BACK closes the pop-up
        g_stub.ident = {};
        g_stub.session++;
        g_stub.caps = saved_caps;
        g_stub.contract = {};
    }
    {   // Caps missed: PD traffic (or an inline sink) means a PD source, not "Type-C only".
        const auto saved_caps = g_stub.caps;
        g_stub.session++;
        g_stub.caps.num_pdos = 0; g_stub.caps.valid = false;
        g_stub.pd_traffic = true;
        test_page(ui, kPdPage, "pd_active_nocaps");
        CHECK_EQ(match_live(ui.link()).typec_ma, 0u);
        g_stub.pd_traffic = false;
        g_stub.session++;
        g_stub.caps = saved_caps;
    }
    CHECK_EQ(ui.list_state().overflows(), 0);
    test_page(ui, kDiagPage, "diag_top");
    // EPR source: 11 rows, scrolled to the end so the two-digit positions are on screen.
    fill_epr();
    test_page(ui, kPdPage, "pd_epr_0");                     // the draw picks up the new row count
    test_page(ui, kPdPage, "pd_epr_max");
    g_stub.caps.num_pdos = 3; g_stub.caps.epr = false;
    {   // Connection > Cable with nothing read: the empty state, a charger on the cable (listen only), the listen window, the scan.
        const auto saved = g_stub.em;
        const auto saved_vbus = g_stub.pm.vbus_mv;
        g_stub.pm.vbus_mv = 0;
        g_stub.em = {};
        ui.goto_page(kPdPage + 1, 32100);
        ui.tick(32105);
        test_screen(ui, "conn_cable_empty", 32200);
        g_stub.pm.vbus_mv = 5000;                                      // a charger supplies the cable: listen only
        ui.tick(32300);
        test_screen(ui, "conn_cable_empty_sniff", 32400);
        const bool saved_result = g_stub.scan_result;
        g_stub.scan_result = false;                                    // nothing heard: the listen window opens
        ui.run_action(Action::EmarkerScan, 32500);
        ui.tick(32520);
        test_screen(ui, "conn_cable_listen", 32600);
        ui.tick(32500 + 5200);                                         // the window ends without a reply
        test_screen(ui, "conn_cable_quiet", 37800);
        g_stub.scan_result = saved_result;
        g_stub.pm.vbus_mv = 0;
        ui.tick(33015);
        g_ui_for_hook = &ui;
        g_stub.on_scan = [] { test_screen(*g_ui_for_hook, "conn_cable_scan", 33100); };   // the scan's own screen
        ui.run_action(Action::EmarkerScan, 33020);
        g_stub.on_scan = nullptr;
        g_stub.em = saved;
        g_stub.pm.vbus_mv = saved_vbus;
    }
    // E-marker page with the full cable decode (active, Thunderbolt, VDO 2).
    g_stub.em.is_active = true; g_stub.em.epr_capable = true; g_stub.em.sop = 1; g_stub.em.spec_rev = 2;
    g_stub.em.vdo_version = 3; g_stub.em.latency_str = "10-20 ns (~2 m)"; g_stub.em.termination_str = "Active, one end";
    g_stub.em.vdo2_valid = true; g_stub.em.max_temp_c = 60; g_stub.em.shutdown_temp_c = 70; g_stub.em.u3_power = 2;
    g_stub.em.usb4_supported = true; g_stub.em.usb_lanes = 2; g_stub.cable_tb = true; g_stub.em.num_vdos = 6;
    // The earlier 1.5 A reading made the link inline (sticky); a replug clears it.
    g_stub.pm.vbus_mv = 5000;                         // 5 V, nothing inline above that
    ui.debug_link(false, 8800); ui.debug_link(true, 8850);
    ui.run_action(Action::PdRescan, 8860);            // drops the debug hold
    g_stub.caps.rx_count++;
    ui.tick(8900);
    CHECK_EQ(ui.link().inline_mode(), 0);
    g_stub.em.valid = true;
    ui.goto_page(kPdPage + 1, 9000);                       // Connection > Cable: the stub holds the cable above
    ui.tick(9050);
    test_screen(ui, "conn_cable_full", 9200);
    CHECK_EQ(g_panel[230 * 240 + 100] != k2::kBg, 1);      // the status bar is painted
    ui.on_button(Button::Ok, ButtonEvent::Short, 9300);    // the pop-up over the filled tab
    test_screen(ui, "conn_cable_popup", 9400);
    ui.on_button(Button::Back, ButtonEvent::Short, 9500);
    // Cable swap: CC gone for more than 300 ms, then back. The scan data stays, dimmed, with LAST.
    ui.tick(10000);
    g_stub.cc = 0;
    ui.tick(10100); ui.tick(10500);
    g_stub.cc = 1;
    ui.tick(10600);
    CHECK_EQ(ui.link().cable_stale(), 1);
    test_screen(ui, "conn_cable_stale", 10700);
    // Charger unplugged: the PD list empties (no LAST, no age); Home keeps its own LAST pill.
    ui.debug_link(false, 11000);
    CHECK_EQ(ui.link().charger_stale(), 1);
    test_page(ui, kPdPage, "pd_detached");
    test_page(ui, 0, "home_unplugged");
    ui.debug_link(true, 12000);
    ui.tick(12100);
    CHECK_EQ(ui.link().charger_stale(), 0);
    // PD trigger step wizard: OFFER, VOLTAGE (PPS only), CURRENT, CONFIRM, then the LIVE card.
    fill_seven();
    g_stub.sink = {};
    {
        uint32_t t = 13000;
        auto press = [&](Button b, ButtonEvent e = ButtonEvent::Short) { ui.on_button(b, e, t); t += 100; };
        const int req0 = g_stub.requests, rel0 = g_stub.releases;
        test_menu(ui, Menu::PdTrigger, "wiz_offer");
        check_title_intact("wiz_offer", 12, 4);   // "1/3 PD offer", the kit bar text starts at x 4
        press(Button::Plus);                                   // 9 V fixed
        test_screen(ui, "wiz_offer_fixed9", t); t += 200;
        press(Button::Ok);                                     // fixed: VOLTAGE is skipped
        test_screen(ui, "wiz_current_fixed", t); t += 200;
        press(Button::Minus);                                  // 50 mA down
        press(Button::Plus);
        press(Button::Ok);
        test_screen(ui, "wiz_confirm_fixed9", t); t += 200;
        press(Button::Ok);                                     // above 5 V: armed, nothing sent yet
        CHECK_EQ(g_stub.requests - req0, 0);
        test_screen(ui, "wiz_confirm_armed", t); t += 200;
        press(Button::Ok);                                     // second press sends
        CHECK_EQ(g_stub.requests - req0, 1);
        CHECK_EQ(int(g_stub.last_choice.pos), 2);
        {   // LIVE states from the sink engine
            using k2::pd::SinkState;
            g_stub.sink = {}; g_stub.sink.state = SinkState::Requesting;
            g_stub.sink.pos = 2; g_stub.sink.mv = 9000; g_stub.sink.ma = 3000;
            test_screen(ui, "wiz_live_requesting", t); t += 200;
            g_stub.sink.state = SinkState::Transition;
            test_screen(ui, "wiz_live_accepted", t); t += 200;
            g_stub.sink.state = SinkState::Ready; g_stub.sink.held = true;
            test_screen(ui, "wiz_live_psrdy", t); t += 200;
            g_stub.sink.hard_resets = 2; g_stub.sink.result = k2::pd::SinkResult::HardReset;
            test_screen(ui, "wiz_live_hardreset", t); t += 200;
            g_stub.sink.hard_resets = 0; g_stub.sink.held = false; g_stub.sink.state = SinkState::Failed;
            g_stub.sink.result = k2::pd::SinkResult::Rejected;
            test_screen(ui, "wiz_live_rejected", t); t += 200;
            g_stub.sink.held = true; g_stub.sink.state = SinkState::Ready; g_stub.sink.result = k2::pd::SinkResult::Ok;
        }
        press(Button::Back);                                   // BACK from LIVE releases to 5 V
        CHECK_EQ(g_stub.releases - rel0, 1);
        test_screen(ui, "wiz_back_from_live", t); t += 200;    // back at CONFIRM
        press(Button::Back);                                   // CURRENT
        press(Button::Back);                                   // OFFER (fixed: no VOLTAGE)
        press(Button::Back);                                   // exits
        CHECK_EQ(static_cast<int>(ui.menu()), static_cast<int>(Menu::Count));
        g_stub.sink = {};
    }
    {   // PPS path: OFFER on the 3.3-11 V PPS, VOLTAGE with coarse steps, CURRENT, CONFIRM
        uint32_t t = 15000;
        auto press = [&](Button b, ButtonEvent e = ButtonEvent::Short) { ui.on_button(b, e, t); t += 100; };
        const int req0 = g_stub.requests;
        ui.goto_page(static_cast<uint8_t>(Page::Tools), t); ui.push_menu(Menu::PdTrigger, t + 50); t += 100;
        for (int i = 0; i < 4; i++) press(Button::Plus);       // PDO 5: PPS 3.3-11 V
        test_screen(ui, "wiz_offer_pps", t); t += 200;
        press(Button::Ok);
        test_screen(ui, "wiz_voltage_pps", t); t += 200;
        press(Button::Minus, ButtonEvent::Long);               // coarse: 1 V
        test_screen(ui, "wiz_voltage_pps_coarse", t); t += 200;
        press(Button::Minus);                                  // 20 mV
        press(Button::Ok);
        test_screen(ui, "wiz_current_pps", t); t += 200;
        press(Button::Minus, ButtonEvent::Long);
        press(Button::Ok);
        test_screen(ui, "wiz_confirm_pps", t); t += 200;
        press(Button::Back);                                   // one step back: CURRENT
        test_screen(ui, "wiz_back_to_current", t); t += 200;
        press(Button::Ok);
        press(Button::Ok);                                     // armed (above 5 V) or sent
        press(Button::Ok);
        CHECK_EQ(g_stub.requests - req0, 1);
        CHECK_EQ(int(g_stub.last_choice.pos), 5);
        g_stub.sink = {};
        ui.goto_page(static_cast<uint8_t>(Page::Home), t + 100);
    }
    {   // A refused send, a dimmed offer (cap) and the scrolled EPR list
        uint32_t t = 17000;
        auto press = [&](Button b, ButtonEvent e = ButtonEvent::Short) { ui.on_button(b, e, t); t += 100; };
        g_stub.request_err = k2::pd::Err::Busy;
        ui.goto_page(static_cast<uint8_t>(Page::Tools), t); ui.push_menu(Menu::PdTrigger, t + 50); t += 100;
        press(Button::Ok); press(Button::Ok);                  // 5 V fixed: CONFIRM
        press(Button::Ok);
        test_screen(ui, "wiz_confirm_refused", t); t += 200;
        g_stub.request_err = k2::pd::Err::Ok;
        ui.goto_page(static_cast<uint8_t>(Page::Home), t);
        fill_epr();
        t += 100;
        ui.goto_page(static_cast<uint8_t>(Page::Tools), t); ui.push_menu(Menu::PdTrigger, t + 50); t += 100;
        for (int i = 0; i < 9; i++) press(Button::Plus);
        test_screen(ui, "wiz_offer_scroll", t); t += 200;
        ui.goto_page(static_cast<uint8_t>(Page::Home), t);
        fill_seven();
    }
    test_page(ui, kPdPage + 2, "conn_sink_idle");
    {   // Running: the status bar carries the progress
        g_stub.ident.state = k2::pd::IdState::Running; g_stub.ident.ok_mask = 0x03;
        test_screen(ui, "conn_sink_identify_running", 13900);
        CHECK_EQ(g_panel[230 * 240 + 100] != k2::kBg, 1);
        g_stub.ident = {};
    }
    {   // A live sink on the other port: Identify is refused on the first press, the status bar says so
        const auto saved = g_stub.pm;
        g_stub.pm.vbus_mv = 20000; g_stub.pm.current_ma = 1500; g_stub.pm.power_mw = 30000;
        g_stub.pm.flow = k2::power::Flow::Forward;
        ui.run_action(Action::PdRescan, 13940);   // drops the forced link: the readings decide
        for (uint32_t t = 13950; t <= 14350; t += 100) ui.tick(t);
        CHECK_EQ(ui.link().inline_mode(), 1);
        ui.on_button(Button::Ok, ButtonEvent::Short, 14355);   // pop-up: Identify
        ui.on_button(Button::Ok, ButtonEvent::Short, 14357);   // inline: the first press only arms the confirm
        test_screen(ui, "conn_sink_refused", 14360);
        ui.on_button(Button::Back, ButtonEvent::Short, 14365);
        g_stub.pm = saved;
        ui.debug_link(false, 14370); ui.debug_link(true, 14380);   // a replug clears the sticky inline state
        ui.tick(14390); ui.tick(14395);
    }
    {   // Identify result: an iPad, answered, with a sink PDO and two SVIDs, one step unanswered
        g_stub.sink_vdm.identity.valid = true; g_stub.sink_vdm.identity.vid = 0x05AC; g_stub.sink_vdm.identity.pid = 0x1A2B;
        g_stub.sink_vdm.identity.vendor = "Apple"; g_stub.sink_vdm.identity.model = "iPad Air 5";
        g_stub.sink_vdm.num_svids = 2; g_stub.sink_vdm.svids[0] = 0x05AC; g_stub.sink_vdm.svids[1] = 0x8087;
        g_stub.sink_vdm.identity.id_header = 0xC0000000u | (2u << 27);   // USB host+device, peripheral
        g_stub.ident.state = k2::pd::IdState::Done; g_stub.ident.ok_mask = 0x1F; g_stub.ident.skipped_mask = 0x24;
        g_stub.ident.revision = 0x30000000;
        g_stub.ident.num_sink_pdos = 1; g_stub.ident.sink_pdos[0] = (uint32_t(400) << 10) | 150;
        test_screen(ui, "conn_sink_result", 14500);
        CHECK_EQ(g_panel[230 * 240 + 100] != k2::kBg, 1);    // status bar
        g_stub.sink_vdm = {}; g_stub.ident = {};
    }
    {   // Aborted names its reason: "Aborted: unplugged" runs well past the 7 characters of a bare "Aborted"
        ui.on_button(Button::Ok, ButtonEvent::Short, 14740);   // pop-up
        ui.on_button(Button::Ok, ButtonEvent::Short, 14745);   // Identify: the page now follows its progress
        g_stub.ident.state = k2::pd::IdState::Aborted; g_stub.ident.why = k2::pd::IdWhy::Detached;
        ui.tick(14755);
        test_screen(ui, "conn_sink_aborted", 14760);
        int ink = 0;
        for (int y = 222; y < 240; y++) for (int x = 100; x < 150; x++) ink += g_panel[y * 240 + x] != k2::kBar;
        CHECK_EQ(ink > 0, 1);
        g_stub.ident = {};
    }
    {   // Identify with nobody attached: no Rp on either CC, VBUS 0. Refused before sending, the footer says so.
        const auto saved_pm = g_stub.pm;
        const auto saved_adc = g_stub.adc;
        g_stub.pm = {}; g_stub.pm.vbus_mv = 0;
        g_stub.adc = k2::adc::Reading{};
        g_stub.identifies = 0;
        ui.debug_link(false, 14770);
        ui.tick(14800);
        ui.tick(14805);
        CHECK_EQ(int(ui.identify(14810, false)), int(Ui::IdResult::NoDevice));
        CHECK_EQ(g_stub.identifies, 0);                              // nothing was transmitted
        ui.tick(14815);
        ui.on_button(Button::Ok, ButtonEvent::Short, 14817);     // pop-up: Identify is greyed, the status bar names why
        test_screen(ui, "conn_sink_nodevice", 14820);
        int amber = 0;
        for (int y = 222; y < 240; y++) for (int x = 20; x < 218; x++) amber += g_panel[y * 240 + x] == k2::kCaution;
        CHECK_EQ(amber > 0, 1);
        ui.on_button(Button::Back, ButtonEvent::Short, 14830);   // close the pop-up
        g_stub.pm = saved_pm; g_stub.adc = saved_adc;
    }
    {   // FC staged input: protocol list, draft/coarse, exact review, live retarget, cleanup.
        const auto saved = g_stub.pm;
        const auto saved_sink = g_stub.sink;
        const auto saved_caps = g_stub.caps;
        g_stub.caps = {};
        g_stub.sink = {};
        g_stub.pm.vbus_mv = 5010; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;
        g_stub.pm.flow = k2::power::Flow::Forward;
        ui.debug_link(false, 14900); ui.debug_link(true, 14910);
        test_menu(ui, Menu::Trigger, "fc_setup");
        check_title_intact("fc_setup", 10);
        CHECK_EQ(g_panel[68 * 240 + 210], k2::kFocusT); // selected QC2 row
        CHECK_EQ(g_panel[70 * 240 + 1], k2::kFocus); // the row's 3 px cyan focus bar
        uint32_t t = 15000;
        auto key = [&](Button b, ButtonEvent p = ButtonEvent::Short) { ui.on_button(b, p, t += 100); };
        key(Button::Plus);                            // QC3
        key(Button::Ok);
        test_screen(ui, "fc_edit", t += 100);
        CHECK_EQ(count_colours(6, 118, 150, 148, {k2::kVolt}) > 100, true);   // the big draft number
        key(Button::Plus, ButtonEvent::Long);           // one 1V draft change
        test_screen(ui, "fc_edit_coarse", t += 100);
        key(Button::Minus, ButtonEvent::Long);          // back to 9V
        key(Button::Ok);
        test_screen(ui, "fc_review", t += 100);
        CHECK_EQ(g_panel[190 * 240 + 1], k2::kFocus);   // the SEND cell: 2 px focus outline
        CHECK_EQ(g_panel[190 * 240 + 100], k2::kFocusT);
        key(Button::Ok);
        test_screen(ui, "fc_armed", t += 100);
        CHECK_EQ(g_panel[180 * 240 + 20], k2::kCaution);   // armed: the cell turns amber
        CHECK_EQ(count_colours(0, 212, 223, 215, {k2::kBg}) > 100, true);   // with the draining bar still nearly full
        key(Button::Ok);
        test_screen(ui, "fc_requesting", t += 100);
        g_stub.fc_state = static_cast<int>(k2::fc::State::Active); g_stub.fc_proto = static_cast<int>(k2::fc::Proto::Qc3);
        g_stub.fc_target_mv = 9000;
        const int vb[] = {5010, 5010, 5010, 6000, 7500, 9020, 9020, 9000, 8400, 9020, 9020, 9010};
        for (int mv : vb) { g_stub.pm.vbus_mv = mv; ui.tick(t += 300); }
        test_screen(ui, "fc_run", t += 100);
        CHECK_EQ(count_colours(6, 110, 216, 218, {k2::kVolt}) > 40, true);   // the VBUS trace
        key(Button::Plus);
        test_screen(ui, "fc_live_edit", t += 100);
        CHECK_EQ(count_colours(72, 84, 170, 104, {k2::kVolt}) > 10, true); // HELD
        CHECK_EQ(count_colours(72, 64, 170, 84, {k2::kVolt}) > 10, true);  // MEASURED
        key(Button::Ok);
        test_screen(ui, "fc_live_review", t += 100);
        key(Button::Ok); key(Button::Ok);
        key(Button::Minus);
        test_screen(ui, "fc_live_edit_down", t += 100);
        key(Button::Back, ButtonEvent::Long);
        test_screen(ui, "fc_releasing", t += 100);
        CHECK_EQ(g_panel[30 * 240 + 200], k2::kCaution);   // the release monitor: an amber AlertBand
        key(Button::Back);
        test_screen(ui, "fc_release_after_exit", t += 100);
        CHECK_EQ(ui.ports_busy(), true);
        g_stub.pm.vbus_mv = 5000;
        for (int n = 0; n < 6; ++n) ui.tick(t += 100);
        CHECK_EQ(ui.ports_busy(), false);
        ui.push_menu(Menu::Trigger, t += 100);
        test_screen(ui, "fc_safe", t += 100);
        key(Button::Minus);                            // QC3 -> QC2
        key(Button::Minus);                            // QC2 -> SCAN
        key(Button::Ok); key(Button::Ok); key(Button::Ok);
        g_stub.fc_scanning = true; g_stub.fc_scan_step = 2;
        g_stub.fc_scan_res = {{k2::fc::Proto::Fcp, k2::fc::Outcome::Rejected, 0}, {k2::fc::Proto::Scp, k2::fc::Outcome::Accepted, 5020}};
        test_screen(ui, "fc_scanning", t += 100);
        g_stub.fc_scanning = false; g_stub.fc_scan_step = 0;
        g_stub.fc_scan_res = {{k2::fc::Proto::Fcp, k2::fc::Outcome::Rejected, 0}, {k2::fc::Proto::Scp, k2::fc::Outcome::Rejected, 0},
                              {k2::fc::Proto::Afc, k2::fc::Outcome::Rejected, 0}, {k2::fc::Proto::Qc2, k2::fc::Outcome::Accepted, 9020},
                              {k2::fc::Proto::Qc3, k2::fc::Outcome::Accepted, 5960}, {k2::fc::Proto::Vooc, k2::fc::Outcome::Skipped, 0}};
        test_screen(ui, "fc_scan", t += 100);
        CHECK_EQ(g_panel[30 * 240 + 9], k2::kError);                   // row 0 (FCP): rejected, solid red cell
        CHECK_EQ(g_panel[96 * 240 + 9], k2::kOkT);                     // row 3 (QC2): accepted, green tint
        g_stub.fc_scan_res.clear();
        g_stub.pm.vbus_mv = 5000;
        for (int n = 0; n < 6; ++n) ui.tick(t += 100);
        key(Button::Plus);                             // leave result view; QC2
        g_stub.pm.vbus_mv = 0; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0; g_stub.pm.flow = k2::power::Flow::Idle;
        g_stub.fc_start_fail = static_cast<int>(k2::fc::Fail::NoBand);
        key(Button::Ok); key(Button::Ok); key(Button::Ok); key(Button::Ok);
        test_screen(ui, "fc_refused", t += 100);
        CHECK_EQ(g_panel[230 * 240 + 1], k2::kErrorT);                  // refusal: Error band fill, no icon
        CHECK_EQ(count_colours(4, 222, 220, 239, {k2::kError}) > 20, true);
        g_stub.fc_start_fail = 0;
        ui.goto_page(static_cast<uint8_t>(Page::Home), t += 100);
        g_stub.pm = saved; g_stub.sink = saved_sink; g_stub.caps = saved_caps;
    }
    {   // PD trigger refused: no charger on INPUT
        ui.debug_link(false, 20200);
        test_menu(ui, Menu::PdTrigger, "pdtrigger_refused");
        CHECK_EQ(g_panel[(22 + 4 + 2) * 240 + 69], k2::kError);          // checklist accent
        CHECK_EQ(g_panel[230 * 240 + 100] != k2::kBg, 1);                // footer band
        ui.debug_link(true, 20300);
        ui.goto_page(static_cast<uint8_t>(Page::Home), 20400);
    }
    // AI recognition: Home widget and the two Lines rows, for a PD source and for a legacy charger.
    fill_seven();
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2;
    g_stub.fc = k2::fastcharge::Protocol::Pending;
    test_page(ui, 0, "home_ai");
    g_stub.fc = k2::fastcharge::Protocol::Dcp5V;
    test_page(ui, static_cast<uint8_t>(Page::Lines), "lines_ai_pd");
    ui.on_button(Button::Plus, ButtonEvent::Short, 5400);
    ui.on_button(Button::Plus, ButtonEvent::Short, 5400);
    test_page(ui, static_cast<uint8_t>(Page::Lines), "lines_ai_pd_end");     // goto_page keeps the scroll offset: the AI rows are in view
    g_stub.caps.num_pdos = 0; g_stub.caps.valid = false;
    g_stub.fc = k2::fastcharge::Protocol::Dcp5V;
    test_page(ui, static_cast<uint8_t>(Page::Lines), "lines_ai_legacy");
    // USB link submenu: everything off, then HID switched on (reboot pending).
    g_stub.usb_stored = {false, false};
    k2::usbcfg::current = {false, false};
    ui.goto_page(static_cast<uint8_t>(k2::Page::Settings), 5400);
    ui.push_menu(k2::Menu::UsbLink, 5401);
    test_screen(ui, "usblink_off", 5300);
    ui.on_button(k2::Button::Ok, k2::ButtonEvent::Short, 5410);
    test_screen(ui, "usblink_hid_pending", 5300);
    CHECK_EQ(g_stub.usb_stored.hid, 1);
    ui.pop_menu(5420);
    k2::usbcfg::current = {true, true};
    g_stub.usb_stored = {};
    // Cable resistance submenu: waiting, idle reference taken, result shown.
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 5100; g_stub.pm.current_ma = 40;
    ui.goto_page(static_cast<uint8_t>(k2::Page::Tools), 5500);
    ui.push_menu(k2::Menu::CableR, 5501);
    test_screen(ui, "cable_wait", 5200);
    {   // Reset visibly does something, an instruction carries no success mark, the table names the load port.
        auto footer = [&] { uint32_t h = 2166136261u; for (int y = 222; y < 240; y++) for (int x = 0; x < 220; x++) h = (h ^ g_panel[y * 240 + x]) * 16777619u; return h; };
        const uint32_t before = footer();
        ui.on_button(Button::Ok, ButtonEvent::Short, 5210);
        ui.tick(5220);
        CHECK_EQ(footer() != before, 1);                                         // "Cleared" shows
        g_stub.pm.current_ma = 400;                                              // loaded already: "Unload, then add 1A"
        ui.on_button(Button::Ok, ButtonEvent::Short, 5230);
        ui.tick(5210 + 1700);
        int ok_px = 0;
        for (int y = 222; y < 240; y++) for (int x = 0; x < 20; x++) ok_px += g_panel[y * 240 + x] == k2::kOk;
        CHECK_EQ(ok_px, 0);
        int port_px = 0;   // third row: "load on top" in dim ink at the value column
        for (int y = 24 + 2 * 26 + 1; y < 24 + 3 * 26; y++) for (int x = 56; x < 120; x++) port_px += g_panel[y * 240 + x] == k2::kDim;
        CHECK_EQ(port_px > 0, 1);
        g_stub.pm.current_ma = 40;
    }
    ui.tick(5600);
    g_stub.pm.vbus_mv = 4900; g_stub.pm.current_ma = -2050;
    ui.tick(5700);
    test_screen(ui, "cable_result", 5200);
    {   // edge-to-edge: rules at the top of every row and under the table, no card fill anywhere in the body
        CHECK_EQ(g_panel[24 * 240 + 100], k2::kRule);
        CHECK_EQ(g_panel[(24 + 3 * 26) * 240 + 2], k2::kRule);
        int card = 0;
        for (int y = 24; y < 222; y++) for (int x = 0; x < 220; x++) card += g_panel[y * 240 + x] == k2::kSurface;
        CHECK_EQ(card, 0);
        int ok_cell = 0, scale = 0;   // the excellent grade: green verdict cell right of the figure, first scale segment lit
        for (int y = 103 + 12; y < 103 + 40; y++) for (int x = 150; x < 214; x++) ok_cell += g_panel[y * 240 + x] == k2::kOkT;
        for (int x = 8; x < 74; x++) scale += g_panel[(103 + 64 + 2) * 240 + x] == k2::kOk;
        CHECK_EQ(ok_cell > 1000, 1);
        CHECK_EQ(scale, 66);
        CHECK_EQ(g_panel[(103 + 64 + 2) * 240 + 100], k2::kRule);    // the other two segments stay unlit
    }
    CHECK_EQ(g_panel[230 * 240 + 100] != k2::kBg, 1);      // footer band
    g_stub.pm.vbus_mv = 4300; g_stub.pm.current_ma = -4050;
    ui.tick(5800);
    test_screen(ui, "cable_poor", 5200);
    {   // Fair has no verdict style: the badge is a solid caution cell and the middle scale segment is lit.
        ui.on_button(Button::Ok, ButtonEvent::Short, 5805);   // clear the latched point, retake idle, then 400 mohm at 2 A
        g_stub.pm.vbus_mv = 5100; g_stub.pm.current_ma = 40;
        ui.tick(5806);
        g_stub.pm.vbus_mv = 4300; g_stub.pm.current_ma = -2000;
        ui.tick(5810);
        test_screen(ui, "cable_fair", 5200);
        int badge = 0;
        for (int y = 103 + 12; y < 103 + 40; y++) for (int x = 150; x < 214; x++) badge += g_panel[y * 240 + x] == k2::kCaution;
        CHECK_EQ(badge > 1000, 1);
        CHECK_EQ(g_panel[(103 + 64 + 2) * 240 + 100], k2::kCaution);
        g_stub.pm.vbus_mv = 4300; g_stub.pm.current_ma = -4050;
        ui.tick(5820);
    }
    // Kelvin mode ([+]): no sense fixture (floating pins read 0 V), then the fixture reading, then back to passive.
    ui.on_button(Button::Plus, ButtonEvent::Short, 5850);
    CHECK_EQ(static_cast<int>(ui.cable_mode()), static_cast<int>(k2::cable_r::Mode::Kelvin));
    g_stub.pm.vbus_mv = 5100; g_stub.pm.current_ma = 1000;
    g_stub.adc = k2::adc::Reading{};
    ui.tick(5860);
    test_screen(ui, "cable_kelvin_nofix", 5200);
    g_stub.adc->cc1_mv = 5000; g_stub.adc->cc2_mv = 5120; g_stub.adc->dm_mv = 5070;   // 120 mohm = 50 + 70 at 1 A
    ui.tick(5870);
    test_screen(ui, "cable_kelvin", 5200);
    CHECK_EQ(ui.kelvin().has_value(), true);
    ui.on_button(Button::Plus, ButtonEvent::Short, 5890);
    CHECK_EQ(static_cast<int>(ui.cable_mode()), static_cast<int>(k2::cable_r::Mode::Passive));
    ui.tick(5895);
    test_screen(ui, "cable_passive_again", 5200);
    {   // No power monitor: a red alert band instead of the table, the reason in the footer.
        g_stub.pm_online = false;
        ui.tick(5896);
        test_screen(ui, "cable_meter_missing", 5200);
        CHECK_EQ(g_panel[40 * 240 + 100], k2::kError);
        CHECK_EQ(g_panel[(24 + 36 + 2) * 240 + 100], k2::kBg);
        g_stub.pm_online = true;
        ui.tick(5897);
    }
    g_stub.adc.reset();
    g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0;
    ui.goto_page(0, 5900);
    // Unplugged after a live reading: grey values, static LAST pill (no age), frozen footer.
    ui.goto_page(0, 20000);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20010; g_stub.pm.current_ma = 2990; g_stub.pm.power_mw = 59830; g_stub.pm.flow = k2::power::Flow::Forward;
    ui.tick(20300);
    g_stub.pm.vbus_mv = 0; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0; g_stub.pm.flow = k2::power::Flow::Idle;
    ui.tick(20400);
    test_screen(ui, "home_stale", 22600);
    check_home_all_grey("home_stale");
    {   // Only the LAST age ticks while stale: 75 s later the frame differs inside the pill box and nowhere else
        std::vector<uint16_t> before(g_panel, g_panel + 240 * 240);
        test_screen(ui, "home_stale_later", 22600 + 75000);
        int diff = 0, outside = 0;
        for (int y = 0; y < 240; y++)
            for (int x = 0; x < 240; x++) {
                if (g_panel[y * 240 + x] == before[y * 240 + x]) continue;
                diff++;
                if (!(y >= 1 && y <= 17 && x >= 130 && x <= 218)) outside++;
            }
        CHECK_EQ(diff > 0, 1);       // the age moved on
        CHECK_EQ(outside, 0);
    }
    {   // A 140 W charger chip ("PD3.1 140W") and the LAST pill must not collide.
        fill_seven();
        g_stub.caps.num_pdos = 7; g_stub.caps.spec_rev = 3; g_stub.caps.valid = true;
        test_screen(ui, "home_stale_pd", 22650 + 600000);
        check_home_all_grey("home_stale_pd");
        g_stub.caps.num_pdos = 0; g_stub.caps.valid = false;
    }
    ui.run_action(k2::Action::EnergyReset, 22700);                  // clears the stale state
    g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0;
    // V, A and W: same decimals, dots in one column; 2 decimals below 100 W, 1 from 100 W.
    g_stub.pm.vbus_mv = 20030; g_stub.pm.current_ma = 1520; g_stub.pm.power_mw = 30456; g_stub.pm.flow = k2::power::Flow::Forward;
    ui.goto_page(0, 23000);
    test_screen(ui, "home_dots_2dec", 23300);
    g_stub.pm.vbus_mv = 20030; g_stub.pm.current_ma = 5100; g_stub.pm.power_mw = 102153;
    test_screen(ui, "home_dots_1dec", 24000);
    g_stub.pm.vbus_mv = 5100; g_stub.pm.current_ma = 40; g_stub.pm.power_mw = 204;
    test_screen(ui, "home_dots_small", 25000);
    {   // Lines v2: bars against the 0.4 V / 2.0 V ticks and the verdict pill
        adc::Reading r;
        r.dp_mv = 600; r.dm_mv = 600; r.cc1_mv = 1680; r.cc2_mv = 0; r.mcu = {324, true, true};
        g_stub.adc = r;
        g_stub.fc = fastcharge::Protocol::Dcp5V;
        ui.goto_page(static_cast<uint8_t>(Page::Lines), 26000);
        test_screen(ui, "lines_dcp", 26100);
        // Lines list rows at 24 + i*26, bar track x96..208 (kLinesBarW 112), ticks at +13/+67.
        CHECK_EQ(g_panel[34 * 240 + 110], k2::kDPlus);        // D+ bar filled to 20 px (x96..116)
        CHECK_EQ(g_panel[34 * 240 + 140], k2::kBg);           // empty part of the track
        CHECK_EQ(g_panel[34 * 240 + 109], k2::kDim);          // 0.4 V tick
        CHECK_EQ(g_panel[60 * 240 + 110], k2::kDMinus);
        CHECK_EQ(g_panel[86 * 240 + 150], k2::kVolt);         // CC1 1.68 V reaches 57 px (x96..153)
        CHECK_EQ(g_panel[86 * 240 + 163], k2::kDim);          // 2.0 V tick over the track
        CHECK_EQ(g_panel[112 * 240 + 150], k2::kBg);          // CC2 0 V: empty
        CHECK_EQ(g_panel[152 * 240 + 12], k2::kOk);           // DCP short pill (solid ok, verdict row y140)
        g_stub.adc->dp_mv = 3300; g_stub.adc->dm_mv = 0;      // QC 9 V pattern: D+ full, D- empty
        g_stub.fc = fastcharge::Protocol::Qc2_9V;
        test_screen(ui, "lines_qc9", 27000);
        CHECK_EQ(g_panel[34 * 240 + 200], k2::kDPlus);        // D+ full: bar x96..208
        CHECK_EQ(g_panel[60 * 240 + 110], k2::kBg);
        g_stub.adc.reset();                                   // no reading: dashes, empty bars
        test_screen(ui, "lines_none", 28000);
        CHECK_EQ(g_panel[34 * 240 + 110], k2::kBg);
    }
    {   // Diag list: state dots at x6..13 of each group's first row (rows at 24 + i*22)
        g_stub.pm_online = true;
        ui.goto_page(kDiagPage, 29000);
        test_screen(ui, "diag_cards", 29100);
        CHECK_EQ(g_panel[31 * 240 + 8] != k2::kBg, 1);        // USB group dot (row 0) painted
        CHECK_EQ(g_panel[97 * 240 + 8] != k2::kBg, 1);        // PD group dot (row 3)
        CHECK_EQ(g_panel[163 * 240 + 8] != k2::kBg, 1);       // SYS group dot (row 6)
        CHECK_EQ(g_panel[205 * 240 + 150], k2::kBg);          // below the last row: plain background
        g_stub.pm_online = false;
        test_screen(ui, "diag_nometer", 30000);
        CHECK_EQ(g_panel[163 * 240 + 8], k2::kError);         // Meter dot red
        g_stub.pm_online = true;
    }
    {   // Tools hub v2: focus bar on the selected row, state dots on the others, tag pill on the selected one.
        const auto saved_pm = g_stub.pm;
        g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 1000;
        ui.debug_fake_pdos(0, 30900); // release forced-link fixtures so the live load guard updates
        ui.goto_page(static_cast<uint8_t>(Page::Tools), 31000);
        for (int i = 0; i < ToolsPage::kTools; i++) ui.on_button(Button::Minus, ButtonEvent::Short, 31010);
        test_screen(ui, "tools_hub", 31100);
        // Tools list (kit RowList): rows at 24 + i*26, selection bar x0..2, the 12 px icon at x8 coloured by readiness, the
        // readiness tag as text at the right edge.
        CHECK_EQ(g_panel[30 * 240 + 1], k2::kFocus);           // selection bar of row 0
        CHECK_EQ(g_panel[30 * 240 + 160], k2::kFocusT);        // selected row fill
        CHECK_EQ(count_colours(8, 57, 20, 69, {k2::kError}) > 0, true);    // PD trigger icon (row 1): blocked by the load
        CHECK_EQ(count_colours(160, 30, 214, 46, {k2::kError}) > 0, true); // selected FC row: its LOAD tag is blocked, in text
        CHECK_EQ(count_colours(8, 83, 20, 95, {k2::kError}) > 0, true);    // Spoof icon (row 2): a live sink is cut
        for (int i = 0; i < 3; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 31200);
        test_screen(ui, "tools_hub_pd", 31300);
        CHECK_EQ(g_panel[110 * 240 + 1], k2::kFocus);          // row 3 (24 + 3*26 + 8)
        CHECK_EQ(g_panel[30 * 240 + 1], k2::kBg);              // row 0 lost its bar
        // Eight tools in a seven row window: the scroll thumb is painted in the bar lane, and it moves with the window.
        const int thumb_top = count_colours(221, 24, 224, 100, {k2::kDim});
        CHECK_EQ(thumb_top > 0, true);
        for (int i = 0; i < 4; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 31400);
        test_screen(ui, "tools_hub_end", 31500);
        CHECK_EQ(count_colours(221, 24, 224, 100, {k2::kDim}) < thumb_top, true);   // the thumb left the top half
        CHECK_EQ(count_colours(221, 120, 224, 210, {k2::kDim}) > 0, true);          // and sits in the lower part
        g_stub.pm = saved_pm;
        ui.debug_link(false, 31510); ui.debug_link(true, 31520); // isolate the synthetic load fixture
    }
    {   // Cable tab with no cable data: the large "not read" message, no rows.
        const auto saved = g_stub.em;
        const auto saved_vbus = g_stub.pm.vbus_mv;
        g_stub.pm.vbus_mv = 0;                                 // no source on the cable: the VCONN scan
        g_stub.em = {};                                        // valid = false
        ui.goto_page(static_cast<uint8_t>(Page::Tools), 31900);
        ui.tick(31905);                                        // the page reads the new VBUS
        ui.goto_page(kConnTabAlias + 1, 31910);
        ui.tick(31915);
        ui.run_action(Action::EmarkerScan, 31920);
        ui.tick(31950);
        test_screen(ui, "emarker_none", 32000);
        int ink = 0;
        for (int y = 74; y < 90; y++) for (int x = 8; x < 200; x++) ink += g_panel[y * 240 + x] != k2::kBg;
        CHECK_EQ(ink > 0, 1);                                  // the message line
        CHECK_EQ(ui.list_state().count, 0);
        g_stub.em = saved;
        g_stub.pm.vbus_mv = saved_vbus;
    }
    {   // Black Box v2: empty, one capture, three captures in two groups, a long timeline
        bb::fresh();
        g_stub.ms = 200000;
        open_blackbox(ui, 40000);
        test_screen(ui, "bb_empty", 40700);
        check_steady(ui, "bb_empty", 40710);
        CHECK_EQ(g_panel[30 * 240 + 150], k2::kBg);            // the text, no card
        CHECK_EQ(g_panel[100 * 240 + 150], k2::kFocusT);       // the live row is the only row and is highlighted
        close_blackbox(ui, 40800);
        bb::fresh();
        bb::capture(pdtrace::Trigger::Contract, 100000, true, 0x05AC, 0x1234);
        g_stub.ms = 112000;
        open_blackbox(ui, 41000);
        g_stub.ms = 112300;
        test_screen(ui, "bb_one", 41700);
        check_steady(ui, "bb_one list", 41710);
        ui.on_button(Button::Ok, ButtonEvent::Short, 41800);
        g_stub.ms = 112300;
        test_screen(ui, "bb_one_timeline", 41900);
        check_steady(ui, "bb_one timeline", 41910);
        close_blackbox(ui, 42000);
        bb::fresh();
        bb::capture(pdtrace::Trigger::HardReset, 50000, false, -1, 0);
        bb::capture(pdtrace::Trigger::HardReset, 80000, true, 0x05AC, 0x1234);
        bb::capture(pdtrace::Trigger::Contract, 120000, true, 0x05AC, 0x1234);
        g_stub.ms = 200000;
        open_blackbox(ui, 42000);
        g_stub.ms = 200003;   // the capture pass spends ~0.5 s: no age crosses a second
        test_screen(ui, "bb_three", 42700);
        // Kit RowList rows from y24: header 26 px, then the focused capture opens to 40 px (its note line), the rest are 26 px.
        CHECK_EQ(g_panel[70 * 240 + 150], k2::kFocusT);        // row 1 (Apple group's capture) highlighted
        CHECK_EQ(g_panel[125 * 240 + 150], k2::kBg);           // the other group's rows are not
        ui.on_button(Button::Plus, ButtonEvent::Short, 42800);
        g_stub.ms = 200003;   // the capture pass spends ~0.5 s: no age crosses a second
        test_screen(ui, "bb_three_sel2", 42900);
        CHECK_EQ(g_panel[60 * 240 + 150], k2::kBg);
        CHECK_EQ(g_panel[125 * 240 + 150], k2::kFocusT);
        ui.on_button(Button::Plus, ButtonEvent::Short, 43000);
        g_stub.ms = 200003;   // the capture pass spends ~0.5 s: no age crosses a second
        test_screen(ui, "bb_three_sel3", 43100);
        CHECK_EQ(g_panel[160 * 240 + 150], k2::kFocusT);
        CHECK_EQ(g_panel[125 * 240 + 150], k2::kBg);
        close_blackbox(ui, 43200);
        bb::fresh();
        bb::g_link = true;
        bb::capture(pdtrace::Trigger::Contract, 100000, true, 0x05AC, 0x1234);
        bb::g_link = false;
        g_stub.ms = 112000;
        open_blackbox(ui, 43300);
        ui.on_button(Button::Ok, ButtonEvent::Short, 43310);      // timeline
        ui.on_button(Button::Ok, ButtonEvent::Short, 43320);      // its link page
        g_stub.ms = 112300;
        test_screen(ui, "bb_link", 43330);
        check_steady(ui, "bb_link", 43335);
        CHECK_EQ(g_panel[60 * 240 + 200], k2::kBg);                // key/value rows on the page, no cards
        CHECK_EQ(g_panel[44 * 240 + 150], k2::kRule);              // the hairline over the first row
        ui.on_button(Button::Back, ButtonEvent::Short, 43340);
        ui.on_button(Button::Back, ButtonEvent::Short, 43350);     // list
        ui.on_button(Button::Plus, ButtonEvent::Short, 43355);     // the visible "Live link" row below the captures
        test_screen(ui, "bb_list_live_sel", 43357);
        check_steady(ui, "bb_list_live_sel", 43358);
        ui.on_button(Button::Ok, ButtonEvent::Short, 43360);       // live link
        test_screen(ui, "bb_link_live", 43370);
        check_steady(ui, "bb_link_live", 43375);
        close_blackbox(ui, 43380);
        bb::fresh();
        bb::capture(pdtrace::Trigger::HardReset, 50000, false, -1, 0);
        g_stub.ms = 60000;
        open_blackbox(ui, 43400);
        ui.on_button(Button::Ok, ButtonEvent::Short, 43410);
        ui.on_button(Button::Ok, ButtonEvent::Short, 43420);
        test_screen(ui, "bb_link_none", 43430);
        close_blackbox(ui, 43440);
        bb::fresh();
        g_stub.ms = 9000;
        bb::src_caps67();
        for (int i = 0; i < 70; i++) { g_stub.ms += 7; bb::msg(i & 1 ? 0x0003 : 0x0001); if (i % 5 == 0) bb::src_caps15(); }
        g_stub.ms += 700;
        pdtrace::reset(pdtrace::ResetKind::HardRx);
        for (int i = 0; i < 20; i++) { g_stub.ms += 20; bb::msg(0x0006, {}); }
        g_stub.ms += 700;
        bb::msg(0x0006, {});
        open_blackbox(ui, 43000);
        ui.on_button(Button::Ok, ButtonEvent::Short, 43700);
        g_stub.ms += 1100 - pdtrace::capture_info(0).age_ms % 1000;        // the capture pass spends ~0.5 s: start at .1
        test_screen(ui, "bb_long", 43800);
        for (int i = 0; i < 4; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 43900 + i);
        g_stub.ms += 1100 - pdtrace::capture_info(0).age_ms % 1000;
        test_screen(ui, "bb_long_scrolled", 44000);
        close_blackbox(ui, 44100);
        bb::fresh();
    }
    test_charger_wizard_scenes();
    test_wizard_gallery();
    test_connection_gallery();
    test_spoof_focus_and_active();
    test_emarker_menu_scenes();
    test_cards_scenes();
    test_cards_fh22_scenes();
    // Every string any page or submenu draws must only use glyphs of the font it is drawn with.
    for (int m = 0; m < static_cast<int>(Menu::Count); m++) {
        ui.goto_page(static_cast<uint8_t>(Page::Tools), 9000);
        ui.push_menu(static_cast<Menu>(m), 9001);
        for (int st = 0; st < 4; st++) {                      // idle, negotiating, active, failed
            g_stub.fc_state = st;
            ui.tick(9100 + st * 300);
        }
        g_stub.fc_state = 0;
    }
    ui.goto_page(0, 9900);
    CHECK_EQ(k2::g_missing_glyphs, 0u);
    return test_report("test_ui_capture");
}
