// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The QR scan-test rig's pure model (k2/ui/ui_qrtest.hpp): the capacity table against tools/qr_capacity.c's output,
// the fit table over the 240 px panel, the show-argument validation with its reason strings, the list text, the
// max-length kind-'t' link (equal to qrshare::link::encode_kind on the padded record, and encodable at exactly that
// version), and the QrTestPage through the real Ui (full-screen draw, BACK pops).
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "fake_lcd.h"
#include "k2/ui/qr.hpp"
#include "k2/ui/qrshare_link.hpp"
#include "k2/ui/qrview.hpp"
#include "k2/ui/ui.hpp"
#include "k2/core/remote_proto.hpp"
#include "k2/ui/ui_qrtest.hpp"
#include "qr_decode.hpp"
#include "test_util.h"
#include "ui_stubs.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;
namespace qt = k2::qrtest;
namespace lk = k2::qrshare::link;

// tools/qr_capacity.c over the vendored generator (byte mode, level L), captured: v<NN> byte_L=...
static const int kCapacityTool[41] = {
    0, 17, 32, 53, 78, 106, 134, 154, 192, 230, 271, 321, 367, 425, 458, 520, 586, 644, 718, 792, 858,
    929, 1003, 1091, 1171, 1273, 1367, 1465, 1528, 1628, 1732, 1840, 1952, 2068, 2188, 2303, 2431, 2563, 2699, 2809, 2953};

// base64url (RFC 4648 section 5, no padding) decode, for checking the link's record.
static std::vector<std::uint8_t> b64url_decode(std::string_view s) {
    static const char* kA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::vector<std::uint8_t> out;
    unsigned acc = 0; int bits = 0;
    for (char c : s) {
        const char* p = std::strchr(kA, c);
        if (!p) return {};
        acc = acc << 6 | unsigned(p - kA);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<std::uint8_t>(acc >> bits)); }
    }
    return out;
}

static void test_capacity_table() {
    for (int v = 1; v <= 40; v++) CHECK_EQ(qt::byte_l(v), kCapacityTool[v]);
    CHECK_EQ(qt::byte_l(0), 0);
    CHECK_EQ(qt::byte_l(41), 0);
    CHECK_EQ(qt::byte_l(11), 321);
    CHECK_EQ(qt::byte_l(13), 425);
    // Monotonic, and the record cap follows the link head (50 chars) + 4 chars per 3 bytes.
    for (int v = 2; v <= 40; v++) CHECK(qt::byte_l(v) > qt::byte_l(v - 1));
    CHECK_EQ(qt::max_record(11), 203);
    CHECK_EQ(qt::max_record(13), 281);
    CHECK_EQ(qt::max_record(24), 840);
    CHECK_EQ(qt::max_record(25), 917);
    CHECK_EQ(lk::kHead, 50);
    CHECK_EQ(qt::max_record(1), 0);   // version 1's 17 chars cannot even hold the head
}

static void test_fit_row() {
    qt::FitRow r = qt::fit_row(11, 3, 4);
    CHECK_EQ(r.modules, 61); CHECK_EQ(r.box, 207); CHECK_EQ(r.fits, 1);
    CHECK_EQ(r.chars, 321); CHECK_EQ(int(r.record), 203);
    r = qt::fit_row(13, 3, 4);
    CHECK_EQ(r.box, 231); CHECK_EQ(r.fits, 1); CHECK_EQ(r.chars, 425); CHECK_EQ(int(r.record), 281);
    r = qt::fit_row(14, 3, 2);
    CHECK_EQ(r.box, 231); CHECK_EQ(r.fits, 1); CHECK_EQ(r.chars, 458);
    r = qt::fit_row(24, 2, 2);
    CHECK_EQ(r.box, 234); CHECK_EQ(r.fits, 1); CHECK_EQ(r.chars, 1171); CHECK_EQ(int(r.record), 840);
    r = qt::fit_row(25, 2, 2);
    CHECK_EQ(r.box, 242); CHECK_EQ(r.fits, 0); CHECK_EQ(r.chars, 1273); CHECK_EQ(int(r.record), 917);
    r = qt::fit_row(25, 2, 1);
    CHECK_EQ(r.box, 238); CHECK_EQ(r.fits, 1);
    r = qt::fit_row(26, 2, 1);
    CHECK_EQ(r.box, 246); CHECK_EQ(r.fits, 0);
    r = qt::fit_row(14, 3, 4);
    CHECK_EQ(r.box, 243); CHECK_EQ(r.fits, 0);
}

static void test_check_show() {
    char reason[qt::kReasonMax];
    CHECK_EQ(qt::check_show(11, 3, 4, reason), 0);
    CHECK_EQ(qt::check_show(13, 3, 4, reason), 0);
    CHECK_EQ(qt::check_show(25, 2, 1, reason), 0);
    CHECK_EQ(qt::check_show(1, 2, 1, reason), 0);
    // Bad ranges: refused with a reason naming the field and the range.
    int n = qt::check_show(0, 3, 4, reason);
    CHECK(n > 0); CHECK(std::string_view(reason, n).find("version 0") != std::string_view::npos);
    n = qt::check_show(41, 3, 4, reason);
    CHECK(n > 0); CHECK(std::string_view(reason, n).find("41") != std::string_view::npos);
    n = qt::check_show(13, 1, 4, reason);
    CHECK(n > 0); CHECK(std::string_view(reason, n).find("px 1") != std::string_view::npos);
    n = qt::check_show(13, 4, 4, reason);
    CHECK(n > 0); CHECK(std::string_view(reason, n).find("px 4") != std::string_view::npos);
    n = qt::check_show(13, 3, 0, reason);
    CHECK(n > 0); CHECK(std::string_view(reason, n).find("quiet 0") != std::string_view::npos);
    n = qt::check_show(13, 3, 5, reason);
    CHECK(n > 0); CHECK(std::string_view(reason, n).find("quiet 5") != std::string_view::npos);
    // The box does not fit the 240 px panel: refused with the box size in the reason.
    n = qt::check_show(25, 2, 2, reason);
    CHECK(n > 0); CHECK(std::string_view(reason, n).find("242") != std::string_view::npos);
    CHECK(std::string_view(reason, n).find("240") != std::string_view::npos);
    n = qt::check_show(26, 2, 1, reason);
    CHECK(n > 0); CHECK(std::string_view(reason, n).find("246") != std::string_view::npos);
    n = qt::check_show(14, 3, 4, reason);
    CHECK(n > 0); CHECK(std::string_view(reason, n).find("243") != std::string_view::npos);
    // Every accepted (px >= 2, quiet >= 1) combination shows at most version 25: the workspace size.
    for (int v = 1; v <= 40; v++)
        for (int px = 2; px <= 3; px++)
            for (int q = 1; q <= 4; q++)
                if (qt::check_show(v, px, q, reason) == 0) CHECK(v <= qt::kMaxShownVersion);
}

static void test_list() {
    // The header plus 8 (px, quiet) combos x 2 rows.
    char line[48];
    CHECK(qt::list_line(0, line) > 0);
    CHECK(std::string_view(line, 2) == "# ");
    CHECK_EQ(qt::list_line(qt::kListLines, line), 0);
    CHECK_EQ(qt::list_line(-1, line), 0);
    // The whole text is the concatenation of the lines, and list_total() says its length.
    std::string all;
    for (int i = 0; i < qt::kListLines; i++) {
        const int n = qt::list_line(i, line);
        CHECK(n > 0);
        all.append(line, static_cast<std::size_t>(n));
    }
    CHECK_EQ(int(qt::list_total()), int(all.size()));
    // The boundary rows the scan matrix is made of.
    CHECK(all.find("v13 m69 p3 q4 box231 fit c425 r281\n") != std::string::npos);
    CHECK(all.find("v24 m113 p2 q2 box234 fit c1171 r840\n") != std::string::npos);
    CHECK(all.find("v25 m117 p2 q2 box242 no c1273 r917\n") != std::string::npos);
    CHECK(all.find("v25 m117 p2 q1 box238 fit c1273 r917\n") != std::string::npos);
    CHECK(all.find("v14 m73 p3 q2 box231 fit c458 r306\n") != std::string::npos);
    // Chunking: any window of the text comes back with the off(2) total(2) n(1) header; off > total is refused.
    std::array<std::uint8_t, k2::remote::kMaxRspPayload> buf{};
    for (std::size_t off : {std::size_t(0), std::size_t(1), std::size_t(7), std::size_t(48), std::size_t(49),
                            all.size() - 1, all.size()}) {
        const std::size_t n = qt::list_chunk(static_cast<std::uint16_t>(off), buf);
        CHECK(n >= 5);
        CHECK_EQ(int(buf[0] | buf[1] << 8), int(off));
        CHECK_EQ(int(buf[2] | buf[3] << 8), int(all.size()));
        CHECK_EQ(int(buf[4]), int(n - 5));
        CHECK(n - 5 <= qt::kListChunk);
        CHECK(std::string_view(reinterpret_cast<const char*>(buf.data() + 5), n - 5) ==
              all.substr(off, n - 5));
    }
    CHECK_EQ(qt::list_chunk(static_cast<std::uint16_t>(all.size() + 1), buf), 0);
    // Walking the chunks from 0 reassembles the table.
    std::string got;
    for (std::size_t off = 0; off < all.size();) {
        const std::size_t n = qt::list_chunk(static_cast<std::uint16_t>(off), buf);
        CHECK(n >= 5);
        got.append(reinterpret_cast<const char*>(buf.data() + 5), n - 5);
        off += n - 5;
        if (n == 5) break;
    }
    CHECK(got == all);
}

static void test_build_link() {
    static qrtest::Workspace w;
    const std::size_t cap = qrtest::kWsBufLen;
    // v13 at 3 px: the link is exactly the version's byte-L capacity (425 chars) and carries the padded record.
    std::size_t n = qt::build_link(13, 3, {reinterpret_cast<char*>(w.temp), cap});
    CHECK_EQ(int(n), 425);
    CHECK_EQ(int(n), qt::byte_l(13));
    const std::string link(reinterpret_cast<const char*>(w.temp), n);
    CHECK(link.substr(0, lk::kBase.size()) == lk::kBase);
    CHECK(link.substr(lk::kBase.size(), 3) == "1t.");
    const auto rec = b64url_decode(std::string_view(link).substr(lk::kHead));
    CHECK_EQ(int(rec.size()), int(qt::max_record(13)));
    CHECK_EQ(int(rec.size()), 281);
    CHECK(std::string_view(reinterpret_cast<const char*>(rec.data()), 20) == "QR scan test v13 3px");
    for (std::size_t i = 20; i < rec.size(); i++) CHECK_EQ(rec[i], 0);
    // The link is exactly the encode_kind form: kBase + '1' + 't' + '.' + base64url(record) (the one-code fits()
    // gate of encode_kind applies to the v11 share links only, not to the bigger test codes).
    char payload[512];
    const std::size_t np = lk::base64url_encode(rec, payload);
    CHECK_EQ(int(np), int(n - lk::kHead));
    CHECK(std::string_view(payload, np) == std::string_view(link).substr(lk::kHead));
    // v11 at 3 px (today's share geometry): 321 chars, record 203 B.
    n = qt::build_link(11, 3, {reinterpret_cast<char*>(w.temp), cap});
    CHECK_EQ(int(n), 321);
    const std::string link11(reinterpret_cast<const char*>(w.temp), n);
    const auto rec11 = b64url_decode(std::string_view(link11).substr(lk::kHead));
    CHECK_EQ(int(rec11.size()), 203);
    CHECK(std::string_view(reinterpret_cast<const char*>(rec11.data()), 20) == "QR scan test v11 3px");
    // v25 at 2 px: 1273 chars, record 917 B, and the workspace still holds it.
    n = qt::build_link(25, 2, {reinterpret_cast<char*>(w.temp), cap});
    CHECK_EQ(int(n), 1273);
    CHECK(n <= cap);
    // Refusals: out of range, px out of range, buffer too small.
    CHECK_EQ(qt::build_link(0, 3, {reinterpret_cast<char*>(w.temp), cap}), 0);
    CHECK_EQ(qt::build_link(41, 3, {reinterpret_cast<char*>(w.temp), cap}), 0);
    CHECK_EQ(qt::build_link(13, 4, {reinterpret_cast<char*>(w.temp), cap}), 0);
    CHECK_EQ(qt::build_link(13, 3, {reinterpret_cast<char*>(w.temp), 424}), 0);
    CHECK_EQ(qt::build_link(13, 3, {reinterpret_cast<char*>(w.temp), 425}), 425);
}

static void test_encode_at_version() {
    static qrtest::Workspace w;
    // The max-length link encodes at exactly its version: it does not fit the one below.
    std::size_t n = qt::build_link(13, 3, {reinterpret_cast<char*>(w.temp), qrtest::kWsBufLen});
    CHECK_EQ(int(n), 425);
    CHECK_EQ(qr::encode_placed(w.code, w.temp, n, 12), 0);      // one version short: refused
    CHECK_EQ(qr::encode_placed(w.code, w.temp, n, 13), 69);      // modules of v13
    CHECK_EQ(qr::dark_code(w.code, 0, 0), true);                 // a finder corner is dark
    // v25: needs exactly version 25 (117 modules); v24 refuses.
    n = qt::build_link(25, 2, {reinterpret_cast<char*>(w.temp), qrtest::kWsBufLen});
    CHECK_EQ(int(n), 1273);
    CHECK_EQ(qr::encode_placed(w.code, w.temp, n, 24), 0);
    CHECK_EQ(qr::encode_placed(w.code, w.temp, n, 25), 117);
    // A version 11 link reads back through the independent reader as the link text itself (the copy is taken before
    // the encode: the generator uses the payload buffer as working storage).
    n = qt::build_link(11, 3, {reinterpret_cast<char*>(w.temp), qrtest::kWsBufLen});
    const std::string link_copy(reinterpret_cast<const char*>(w.temp), n);
    CHECK_EQ(qr::encode_placed(w.code, w.temp, n, 11), 61);
    const qrdec::Result r = qrdec::decode(61, [&](int x, int y) { return qr::dark_code(w.code, x, y); });
    CHECK(r.ok);
    CHECK_EQ(r.version, 11);
    CHECK(r.text == link_copy);
    // encode_version copies the payload in and checks the buffer size; encode() keeps its version-12 cap.
    CHECK_EQ(qr::encode_version("hello", w.code, w.temp, qrtest::kWsBufLen, 1), 21);
    CHECK_EQ(qr::encode_version("hello", w.code, w.temp, 100, 25), 0);   // buffers too small for v25
    qr::Workspace small;
    CHECK_EQ(qr::encode(std::string(400, 'x'), small), 0);
}

// ---- the page through the real Ui -------------------------------------------------------------------------------------------

static Ui &fresh_ui() {
    static Ui ui;
    g_stub.ms = 1000;
    ui.init(g_stub.ms);
    ui.set_rotation(0, g_stub.ms);
    return ui;
}

static void test_page() {
    Ui &ui = fresh_ui();
    ui.tick(1000);
    CHECK_EQ(int(ui.menu()), int(Menu::Count));
    // Open v13 at 3 px quiet 4 (the dispatcher validated it before calling the backend).
    ui.context().qrtest = {13, 3, 4};
    CHECK(ui.push_menu(Menu::QrTest, 1100));
    for (uint32_t t = 1102; ui.painting(); t += 2) ui.tick(t);
    CHECK_EQ(int(ui.menu()), int(Menu::QrTest));
    CHECK_EQ(ui.page_index(), kMenuPageBase + static_cast<int>(Menu::QrTest));
    const QrTestPage &pg = ui.qrtest_page();
    CHECK_EQ(pg.version(), 13);
    CHECK_EQ(pg.px(), 3);
    CHECK_EQ(pg.quiet(), 4);
    CHECK_EQ(pg.modules(), 69);                       // the code encoded at v13
    CHECK_EQ(int(pg.link_len()), 425);                // the max-length link of v13
    std::array<char, 1300> linkbuf{};
    CHECK_EQ(int(pg.link_text(linkbuf)), 425);
    const std::string link(linkbuf.data(), 425);
    CHECK(link.substr(0, lk::kBase.size()) == lk::kBase);
    CHECK(link.substr(lk::kBase.size(), 3) == "1t.");
    // The panel holds one full-screen box: the quiet zone's corners are white, the symbol inside matches the code.
    const qrview::Plan p = qrview::plan_in(qrview::Area{}, pg.modules(), 0, pg.px(), pg.quiet(), pg.px());
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 231); CHECK_EQ(p.x0, 4);
    CHECK_EQ(g_panel[0], 0xFFFF);                     // outside the box the fill left white
    CHECK_EQ(g_panel[p.y0 * 240 + p.x0], 0xFFFF);     // top-left quiet corner inside the box
    CHECK_EQ(g_panel[(p.box - 1) * 240 + p.x0 + p.box - 1], 0xFFFF);   // bottom-right quiet corner
    // Read the drawn code back (v13 is past the test reader's version 12, so re-encode into a local buffer and compare).
    static qrtest::Workspace w_chk;
    std::memcpy(w_chk.temp, link.data(), link.size());
    CHECK_EQ(qr::encode_placed(w_chk.code, w_chk.temp, link.size(), 13), 69);
    bool same = true;
    for (int my = 0; my < 69 && same; my++)
        for (int mx = 0; mx < 69 && same; mx++) {
            const int px = p.x0 + (mx + 4) * 3 + 1, py = p.y0 + (my + 4) * 3 + 1;
            same = (g_panel[py * 240 + px] == 0x0000) == qr::dark_code(w_chk.code, mx, my);
        }
    CHECK(same);
    // The chrome is hidden: the button strip's edge pixels are the box's quiet zone, not a label.
    CHECK_EQ(g_panel[239 * 240 + 239], 0xFFFF);       // bottom-right of the panel: quiet zone white
    // BACK pops the page (ordinary page stack).
    ui.on_button(Button::Back, ButtonEvent::Short, 1200);
    CHECK_EQ(int(ui.menu()), int(Menu::Count));
    CHECK_EQ(int(ui.page()), int(Page::Home));
    // Re-show at another geometry: v25 at 2 px quiet 1 (the largest the panel can show).
    ui.context().qrtest = {25, 2, 1};
    CHECK(ui.push_menu(Menu::QrTest, 1300));
    for (uint32_t t = 1302; ui.painting(); t += 2) ui.tick(t);
    const QrTestPage &pg2 = ui.qrtest_page();
    CHECK_EQ(pg2.modules(), 117);
    CHECK_EQ(int(pg2.link_len()), 1273);
    const qrview::Plan p2 = qrview::plan_in(qrview::Area{}, 117, 0, 2, 1, 2);
    CHECK_EQ(p2.box, 238);
    ui.on_button(Button::Back, ButtonEvent::Short, 1400);
    CHECK_EQ(int(ui.menu()), int(Menu::Count));
    // A v11 show reads back through the independent reader as the link text.
    ui.context().qrtest = {11, 3, 4};
    CHECK(ui.push_menu(Menu::QrTest, 1500));
    for (uint32_t t = 1502; ui.painting(); t += 2) ui.tick(t);
    const QrTestPage &pg3 = ui.qrtest_page();
    CHECK_EQ(pg3.modules(), 61);
    CHECK_EQ(int(pg3.link_len()), 321);
    std::array<char, 400> lb{};
    CHECK_EQ(int(pg3.link_text(lb)), 321);
    const std::string want(lb.data(), 321);
    const qrview::Plan p3 = qrview::plan_in(qrview::Area{}, 61, 0, 3, 4, 3);
    const qrdec::Result r = qrdec::decode(61, [&](int x, int y) {
        const int px = p3.x0 + (x + 4) * 3 + 1, py = p3.y0 + (y + 4) * 3 + 1;
        return g_panel[py * 240 + px] == 0x0000;
    });
    CHECK(r.ok);
    CHECK(r.text == want);
    // With the encode workspace leased from the scratch arena during paint(), QrTestPage is tiny and fits over both
    // ChargerTest and Tools.
    ui.on_button(Button::Back, ButtonEvent::Short, 1600);
    ui.goto_page(static_cast<std::uint8_t>(Page::ChargerTest), 1700);
    ui.context().qrtest = {13, 3, 4};
    CHECK(ui.push_menu(Menu::QrTest, 1800));
    CHECK_EQ(int(ui.menu()), int(Menu::QrTest));
    ui.on_button(Button::Back, ButtonEvent::Short, 1850);
    CHECK_EQ(int(ui.menu()), int(Menu::Count));
    ui.goto_page(static_cast<std::uint8_t>(Page::Tools), 1900);
    CHECK(ui.push_menu(Menu::QrTest, 2000));
    CHECK_EQ(int(ui.menu()), int(Menu::QrTest));
}

// A full-panel code covers the button strip, so the page hides it (PageIf::hides_strip): a key press must not paint the strip's
// pressed flash over the code, in any rotation, and the code is centred whatever the version (v18 sat in the top-left corner
// once).
static void test_strip_stays_hidden_and_code_centred() {
    for (std::uint8_t rot = 0; rot < 4; rot++) {
        Ui &ui = fresh_ui();
        ui.set_rotation(rot, g_stub.ms);
        ui.tick(1000);
        ui.context().qrtest = {18, 2, 4};
        CHECK(ui.push_menu(Menu::QrTest, 1100));
        for (uint32_t ts = 1102; ui.painting(); ts += 2) ui.tick(ts);
        const QrTestPage &pg = ui.qrtest_page();
        CHECK_EQ(pg.modules(), 89);
        const qrview::Plan p = qrview::plan_in(qrview::Area{}, 89, 0, 2, 4, 2);
        CHECK_EQ(p.box, 194);
        CHECK_EQ(p.x0, 23); CHECK_EQ(p.y0, 23);                      // (240 - 194) / 2: centred, not top-left
        static std::uint16_t before[240 * 240];
        std::memcpy(before, g_panel, sizeof before);
        std::uint32_t t = 1200;
        for (Button b : {Button::Ok, Button::Plus, Button::Minus, Button::Back}) {
            ui.set_pressed(b, true, t);
            ui.set_pressed(b, false, t + 200);
            t += 400;
        }
        ui.tick(t);
        CHECK(std::memcmp(before, g_panel, sizeof before) == 0);     // not one pixel moved: no strip flash over the code
        // The code's own margin is white on all four edges of the box: the whole panel outside the symbol is white.
        CHECK_EQ(g_panel[0], 0xFFFF); CHECK_EQ(g_panel[239], 0xFFFF);
        CHECK_EQ(g_panel[239 * 240], 0xFFFF); CHECK_EQ(g_panel[239 * 240 + 239], 0xFFFF);
        ui.on_button(Button::Back, ButtonEvent::Short, t + 100);
        CHECK_EQ(int(ui.menu()), int(Menu::Count));
    }
}

int main() {
    test_capacity_table();
    test_fit_row();
    test_check_show();
    test_list();
    test_build_link();
    test_encode_at_version();
    test_page();
    test_strip_stays_hidden_and_code_centred();
    return test_report("test_ui_qrtest");
}
