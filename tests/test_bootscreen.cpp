// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Boot-info LCD screen (k2/ui/bootscreen.hpp): the layout rule, the show/hold timeline, the readable lines, and the whole
// path text -> QR -> panel pixels -> an independent reader -> the same text.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "fake_lcd.h"
#include "k2/core/bootinfo.hpp"
#include "k2/ui/bootscreen.hpp"
#include "k2/ui/qr.hpp"
#include "k2/ui/qrshare.hpp"
#include "qr_decode.hpp"
#include "test_util.h"
#include "k2/ui/ui.hpp"
#include "ui_stubs.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;

static bootinfo::Snapshot plus_boot() {
    bootinfo::Snapshot s;
    std::strcpy(s.fw, "1.0");
    std::strcpy(s.git, "fcb3128-dirty");
    s.proto = 1;
    s.mode = bootmode::Mode::PlusRdCc2;
    s.keys = bootmode::kPlus;
    s.pa = bootinfo::kPaOk | bootinfo::kPaBack | bootinfo::kPaMinus;
    s.reset_flags = 0x24000000;
    s.pin_initial = 2; s.pin_now = 1; s.pin_boot = 1;
    s.move = {true, 650, 2, 1, 0, 3300, 150};
    s.notes[0] = {3, 0, 2};
    s.notes[1] = {4, 1, 3};
    s.notes[2] = {652, 0, 1};
    s.notes[3] = {50, 2, 2};
    s.notes[4] = {600, 2, 2};
    s.notes[5] = {3001, 2, 1};
    s.note_n = 6;
    s.t_ms = 12345; s.vbus_mv = 5012; s.cc1_mv = 3290; s.cc2_mv = 0; s.plus_now = true;
    return s;
}

static void test_plan() {
    for (int n = 21; n <= 4 * bootscreen::kMaxVersion + 17; n += 4) {   // up to the largest symbol the screen draws (v10, 57)
        const bootscreen::Plan p = bootscreen::plan(n);
        CHECK(p.px >= bootscreen::kMinModulePx);                       // never below 3 px per module
        CHECK_EQ(p.box, (n + 2 * bootscreen::kQuietModules) * p.px);   // 4-module quiet zone on every side
        CHECK(p.box + bootscreen::kTextH <= 240);                      // the text lines fit under it
        CHECK(p.x0 >= 0 && p.x0 + p.box <= 240);
        CHECK_EQ(p.x0, (240 - p.box) / 2);
        CHECK_EQ(p.y0, (240 - bootscreen::kTextH - p.box) / 2);       // box + text lines centred as one block
    }
    CHECK_EQ(bootscreen::plan(57).px, 3);
    CHECK(bootscreen::plan(21).px > 3);                                // a small symbol gets bigger modules
    CHECK_EQ(bootscreen::plan(0).px, 0);                               // nothing to draw
    CHECK_EQ(bootscreen::plan(80).px, 0);                              // too big for the panel at 3 px
}

static void test_timeline() {
    using A = bootscreen::Controller::Action;
    bootscreen::Controller c;
    CHECK(!c.active());
    CHECK(c.tick(0, true) == A::None);                    // not started: nothing
    c.start(1000);
    CHECK(c.active());
    CHECK(c.tick(1000, true) == A::Redraw);               // the first frame at once
    CHECK_EQ(c.frame(), 1u);
    CHECK(c.tick(1200, true) == A::None);
    CHECK(c.tick(1499, true) == A::None);
    CHECK(c.tick(1500, true) == A::Redraw);               // every 500 ms while the key is held
    CHECK_EQ(c.frame(), 2u);
    CHECK(c.tick(2000, true) == A::Redraw);
    CHECK_EQ(c.frame(), 3u);
    // Released at 2300: the last frame stays 30 s, no more redraws, then Exit.
    CHECK(c.tick(2300, false) == A::None);
    CHECK(c.tick(2800, false) == A::None);
    CHECK(c.tick(2300 + 29999, false) == A::None);
    CHECK(c.active());
    CHECK(c.tick(2300 + 30000, false) == A::Exit);
    CHECK(!c.active());
    CHECK(c.tick(2300 + 31000, false) == A::None);
    // Pressed again during the hold: refresh resumes and the 30 s restart from the next release.
    c.start(0);
    CHECK(c.tick(0, true) == A::Redraw);
    CHECK(c.tick(100, false) == A::None);
    CHECK(c.tick(20000, true) == A::Redraw);
    CHECK(c.tick(20100, false) == A::None);
    CHECK(c.tick(20100 + 29000, false) == A::None);
    CHECK(c.tick(20100 + 30000, false) == A::Exit);
    // The key was already up when the first tick came: one frame is drawn anyway, then the hold runs from there.
    c.start(5000);
    CHECK(c.tick(5000, false) == A::Redraw);
    CHECK_EQ(c.frame(), 1u);
    CHECK(c.tick(5000 + 29999, false) == A::None);
    CHECK(c.tick(5000 + 30000, false) == A::Exit);
    // dismiss() ends it at once.
    c.start(0);
    CHECK(c.tick(0, true) == A::Redraw);
    c.dismiss();
    CHECK(!c.active());
    CHECK(c.tick(10, true) == A::None);
    // The millisecond clock wrapping does not freeze it.
    c.start(0xFFFFFF00u);
    CHECK(c.tick(0xFFFFFF00u, true) == A::Redraw);
    CHECK(c.tick(0xFFFFFF00u + 500, true) == A::Redraw);
}

static void test_lines() {
    bootscreen::Lines l;
    bootscreen::format_lines(plus_boot(), 7, l);
    CHECK(std::string(l.id) == "fw 1.0 fcb3128-dirty p1");
    CHECK(std::string(l.up) == "up 12.3 s  frame 7");
    CHECK(std::string(l.mode) == "plus  Rd CC2>CC1 at 650 ms");
    bootinfo::Snapshot p = plus_boot();
    p.move.moved = false; p.mode = bootmode::Mode::OkRdCc1; p.t_ms = 9; p.pin_initial = 1;
    bootscreen::format_lines(p, 1, l);
    CHECK(std::string(l.up) == "up 0.0 s  frame 1");
    CHECK(std::string(l.mode) == "ok  Rd CC1, no move");
    p.mode = bootmode::Mode::Plain;
    bootscreen::format_lines(p, 1, l);
    CHECK(std::string(l.mode).starts_with("plain"));
    p.t_ms = 3600u * 1000 * 5 + 61 * 1000;
    bootscreen::format_lines(p, 4000000000u, l);
    CHECK(std::string(l.up) == "up 18061.0 s  frame 4000000000");
    for (const char* s : {l.id, l.up, l.mode}) CHECK(std::strlen(s) <= 40);     // 40 cells of the 6 px font fill the panel width
}

// Reads the symbol back out of the panel pixels: sample the middle of each module cell.
static bool module_at(const bootscreen::Plan& p, int mx, int my) {
    const int x = p.x0 + (mx + bootscreen::kQuietModules) * p.px + p.px / 2;
    const int y = p.y0 + (my + bootscreen::kQuietModules) * p.px + p.px / 2;
    return g_panel[y * 240 + x] == 0x0000;
}

// The frame text the screen puts in the QR code for `snapshot` (what the host tool decodes), and the symbol size it makes.
static int frame_text(const bootinfo::Snapshot& s, std::string& text, std::string& record, qr::Workspace& w) {
    char rec[bootinfo::kQrMax + 1];
    const std::size_t len = bootinfo::format(s, rec);
    record.assign(rec, len);
    char out[qrshare::kTextBuf];
    const std::size_t tl = qrshare::encode_part(qrshare::Type::BootInfo, qrshare::kSchemaBootInfo,
                                                {reinterpret_cast<const std::uint8_t*>(rec), len}, 0, out);
    text.assign(out, tl);
    return qr::encode_text(out, w, bootscreen::kMaxVersion);
}

// Panel pixels -> reader -> frame text -> qrshare parser -> the K2B1 record.
static bool read_panel(const bootscreen::Plan& p, int n, std::string& record_out, std::string& text_out) {
    const qrdec::Result r = qrdec::decode(n, [&](int x, int y) { return module_at(p, x, y); });
    if (!r.ok) return false;
    text_out = r.text;
    std::uint8_t buf[qrshare::kMaxFrame];
    qrshare::Part part;
    if (!qrshare::parse_part(r.text, part, buf)) return false;
    if (part.type != qrshare::Type::BootInfo || part.schema != qrshare::kSchemaBootInfo || part.count != 1) return false;
    record_out.assign(reinterpret_cast<const char*>(part.payload.data()), part.payload.size());
    return true;
}

static void test_render() {
    const bootinfo::Snapshot s = plus_boot();
    static qr::Workspace w;
    std::string text, record;
    const int n = frame_text(s, text, record, w);
    bootscreen::draw(s, 1);
    CHECK(n >= 21 && n <= 57);
    const bootscreen::Plan p = bootscreen::plan(n);
    CHECK(p.px >= 3);
    // The independent reader gets the frame back, and the frame carries the K2B1 text.
    std::string got_record, got_text;
    CHECK(read_panel(p, n, got_record, got_text));
    CHECK(got_text == text);
    CHECK(got_record == record);
    CHECK(text.starts_with("K2:BI1:"));
    CHECK(got_record.starts_with("K2B1 fw=1.0 g=fcb3128-dirty p=1 "));         // the firmware identity first
    // Black on white only inside the QR box, white all around it (the first frame clears the panel), a clean quiet zone.
    int stray = 0, quiet_bad = 0;
    for (int y = 0; y < p.box; y++)
        for (int x = 0; x < p.box; x++) {
            const std::uint16_t v = g_panel[(p.y0 + y) * 240 + p.x0 + x];
            if (v != 0x0000 && v != 0xFFFF) stray++;
            const int q = bootscreen::kQuietModules * p.px;
            if ((x < q || y < q || x >= p.box - q || y >= p.box - q) && v != 0xFFFF) quiet_bad++;
        }
    CHECK_EQ(stray, 0);
    CHECK_EQ(quiet_bad, 0);
    int outside_bad = 0;
    for (int y = 0; y < p.box; y++)
        for (int x = 0; x < 240; x++)
            if ((x < p.x0 || x >= p.x0 + p.box) && g_panel[y * 240 + x] != 0xFFFF) outside_bad++;
    CHECK_EQ(outside_bad, 0);
    // The readable lines are black on white under the code.
    int ink = 0;
    for (int y = p.box; y < 240; y++)
        for (int x = 0; x < 240; x++)
            if (g_panel[y * 240 + x] != 0xFFFF) ink++;
    CHECK(ink > 200);

    // A later frame redraws the code and the text without a clear: still decodes, new uptime text.
    bootinfo::Snapshot s2 = s;
    s2.t_ms = 99999;
    bootscreen::draw(s2, 2);
    std::string text2, record2;
    static qr::Workspace w2;
    const int n2 = frame_text(s2, text2, record2, w2);
    const bootscreen::Plan p2 = bootscreen::plan(n2);
    CHECK(read_panel(p2, n2, got_record, got_text));
    CHECK(got_record == record2);
    CHECK(got_record.find("t=99999") != std::string::npos);

    // The largest boot text (kQrMax characters of K2B1) still fits version 10 and the three-line layout.
    bootinfo::Snapshot big = s;
    big.note_n = bootinfo::kMaxNotes;
    for (unsigned i = 0; i < bootinfo::kMaxNotes; i++) big.notes[i] = {std::uint16_t(3000 + i), std::uint8_t(i % 3), std::uint8_t(i)};
    big.t_ms = 4000000000u; big.vbus_mv = 20000; big.cc1_mv = 3300; big.cc2_mv = 3300;
    static qr::Workspace w3;
    std::string t3, r3;
    const int n3 = frame_text(big, t3, r3, w3);
    CHECK(n3 >= 21 && n3 <= 57);
    CHECK(r3.size() <= bootinfo::kQrMax);

    if (const char* d = std::getenv("K2_DUMP")) {
        bootscreen::draw(s, 7);
        char path[256];
        std::snprintf(path, sizeof path, "%s/k2_bootscreen.panel", d);
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, 240 * 240, f); std::fclose(f); }
        std::printf("record: %s\nframe:  %s\n", record.c_str(), text.c_str());
    }
}

// Handing the panel back: Ui::repaint() puts the page up exactly as init() drew it, whatever was on the screen.
static void test_repaint() {
    g_stub = {};
    static Ui ui;
    ui.init(1000);
    ui.tick(1100);
    ui.repaint(1200);
    static std::uint16_t before[240 * 240];
    std::memcpy(before, g_panel, sizeof before);
    bootscreen::draw(plus_boot(), 1);
    CHECK(std::memcmp(before, g_panel, sizeof before) != 0);
    ui.repaint(1300);
    CHECK(std::memcmp(before, g_panel, sizeof before) == 0);
}

int main() {
    test_plan();
    test_timeline();
    test_lines();
    test_render();
    test_repaint();
    return test_report("test_bootscreen");
}
