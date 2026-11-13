// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The share view (ui_share.hpp): the layout rule inside a rectangle, the page through the real Ui (open, auto-cycle, paging,
// BACK), the panel read back by an independent QR reader in every rotation, and the Share action of each opener.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "fake_lcd.h"
#include <cstdarg>

#include "k2/core/binlog.hpp"
#include "k2/pd/emk_quirk.hpp"
#include "k2/core/version.hpp"
#include "k2/core/lz.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/ui/qrshare.hpp"
#include "k2/ui/qrshare_link.hpp"
#include "k2/ui/qrshare_records.hpp"
#include "k2/ui/qrview.hpp"
#include "k2/ui/ui_qrtest.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include "qr_decode.hpp"
#include "qrshare_fixtures.h"
#include "test_util.h"
#include "k2/ui/ui.hpp"
#include "ui_nav.h"
#include "ui_stubs.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;
namespace qs = k2::qrshare;
namespace rec = k2::qrshare::records;
namespace rc = k2::report_card;
namespace lk = k2::qrshare::link;

// ---- the layout rule -------------------------------------------------------------------------------------------------------

static void test_plan_in() {
    const qrview::Area portrait{0, 0, 224, 240}, landscape{0, 0, 240, 224};
    // Version 11 (61 modules) is what a 510-byte record needs per part: 3 px per module in either orientation.
    constexpr int th = SharePage::kTextH;
    qrview::Plan p = qrview::plan_in(portrait, 61, th, 3);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 207); CHECK_EQ(p.x0, 8); CHECK_EQ(p.y0, (240 - th - 207) / 2);   // the box and its text row centred as one block
    p = qrview::plan_in(landscape, 61, th, 3);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 207); CHECK_EQ(p.x0, 16);
    // Version 12 (65): 3 px only fits the portrait area; the landscape one drops to 2 px, and refuses at a 3 px floor.
    p = qrview::plan_in(portrait, 65, th, 3);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 219);
    p = qrview::plan_in(landscape, 65, th, 3);
    CHECK_EQ(p.px, 0);
    p = qrview::plan_in(landscape, 65, th, 2);
    CHECK_EQ(p.px, 2); CHECK_EQ(p.box, 146);
    // An area that sits right of the panel's left edge keeps the box inside it.
    const qrview::Area right{16, 0, 224, 240};
    p = qrview::plan_in(right, 41, th, 3);
    CHECK_EQ(p.x0, 16 + (224 - p.box) / 2);
    CHECK_EQ(qrview::plan_in(portrait, 0, th, 3).px, 0);
    // The whole-panel plan is unchanged.
    CHECK_EQ(qrview::plan(57, 14).px, qrview::plan_in(qrview::Area{}, 57, 14, qrview::kMinModulePx).px);
}

// ---- reading the panel back --------------------------------------------------------------------------------------------------

// The code on the panel -> the reader -> the frame text of the part on show; "" when it does not read.
static std::string read_panel(const Ui& ui) {
    const int n = ui.share().modules();
    const qrview::Plan p = ui.share().plan();
    const int quiet = ui.share().look().quiet;
    if (n == 0 || p.px == 0) return "";
    const qrdec::Result r = qrdec::decode(n, [&](int x, int y) {
        const int px = p.x0 + (x + quiet) * p.px + p.px / 2;
        const int py = p.y0 + (y + quiet) * p.px + p.px / 2;
        return g_panel[py * 240 + px] == 0x0000;
    });
    return r.ok ? r.text : "";
}

static void fill_record(Ui& ui, std::size_t len) {
    auto b = ui.share_buffer();
    unsigned x = 11;   // random-looking bytes: they do not pack, so the page keeps the record as built
    for (std::size_t i = 0; i < len; i++) { x = x * 1103515245u + 12345u; b[i] = static_cast<std::uint8_t>(x >> 16); }
}

static std::string label(Ui& ui, Button b) {
    static char buf[24];
    label_text(buf, ui.labels().b[int(b)], label_max_chars(b));
    return buf;
}

// Feeds the part on the panel to the assembler; its Result.
static qs::Assembler::Result feed(Ui& ui, qs::Assembler& a) {
    const std::string t = read_panel(ui);
    if (t.empty()) return qs::Assembler::Result::Bad;
    return a.feed(t);
}

// The link form on the panel -> the record it carries (inflated when the link says 'z'); 0 when the code does not read, is not
// a link of this base and version, or carries a bad record. `kind` gets the link's kind letter. Version '2' links (the card and
// the cable) carry the bit-packed v2 record as they are.
static char link_kind = 0;
static std::size_t read_link(const Ui& ui, std::span<std::uint8_t> out) {
    const std::string t = read_panel(ui);
    std::string_view s = t;
    link_kind = 0;
    if (s.substr(0, lk::kBase.size()) != lk::kBase) return 0;
    s.remove_prefix(lk::kBase.size());
    if (s.size() < 3 || (s[0] != lk::kVersion && s[0] != lk::kVersionV2) || s[2] != '.') return 0;
    link_kind = s[1];
    s.remove_prefix(3);
    std::array<std::uint8_t, 700> raw{};   // a link carries up to lk::kMaxRecord (659) bytes
    std::size_t n = 0;
    unsigned acc = 0, bits = 0;
    for (const char c : s) {
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        const char* p = std::strchr(a, c);
        if (c == 0 || !p) return 0;
        acc = acc << 6 | unsigned(p - a);
        bits += 6;
        if (bits >= 8) { bits -= 8; raw[n++] = std::uint8_t(acc >> bits); acc &= (1u << bits) - 1; }
    }
    if (std::strchr("cepib", link_kind)) {   // lowercase: the record as built
        std::memcpy(out.data(), raw.data(), n);
        return n;
    }
    if (!std::strchr("zEPIB", link_kind)) return 0;   // uppercase (and z, the card's): an LZ4 block
    const std::size_t m = lz::decompress({raw.data(), n}, out);
    return m == lz::kError ? 0 : m;
}

// The record a link of the current share carries: the schema v2 record when the page re-packed the record (card, cable),
// else the record as the opener built it. Returns the length, written into `out`.
static std::size_t link_payload(const Ui& ui, const std::uint8_t* rec, std::size_t len, std::span<std::uint8_t> out) {
    if (ui.share().v2()) return qs::records::pack_v2(ui.share().type(), {rec, len}, out);
    if (out.size() < len) return 0;
    std::memcpy(out.data(), rec, len);
    return len;
}

// The type a link's kind letter names ("cz" card, "eE" cable, "pP" source caps, "iI" identity, "bB" boot info).
static qs::Type link_type(char kind) {
    switch (kind) {
        case 'c': case 'z': return qs::Type::Card;
        case 'e': case 'E': return qs::Type::Emarker;
        case 'p': case 'P': return qs::Type::PdCaps;
        case 'i': case 'I': return qs::Type::Identity;
        case 'b': case 'B': return qs::Type::BootInfo;
        default: return qs::Type::Log;   // no link kind: never a link
    }
}

// K2_DUMP=<dir>: the panel as it is now, for tools/panel2png.py (docs and the renders of a change).
static void dump_panel(const char* name) {
    const char* d = std::getenv("K2_DUMP");
    if (!d || !name) return;
    char path[256];
    std::snprintf(path, sizeof path, "%s/%s.panel", d, name);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, 240 * 240, f); std::fclose(f); }
}

// ---- the page ----------------------------------------------------------------------------------------------------------------

static void test_two_parts(uint8_t rotation) {
    g_stub = {};
    static Ui ui;
    ui.init(1000);
    ui.set_rotation(rotation, 1000);
    ui.tick(1100);
    auto settle = [](Ui& u, uint32_t t) { while (u.painting()) u.tick(t += 2); };
    fill_record(ui, 510);
    std::array<std::uint8_t, 510> want;
    for (std::size_t i = 0; i < want.size(); i++) want[i] = ui.share_buffer()[i];

    ui.tick(2000);
    CHECK(ui.open_share(qs::Type::Log, 1, "Test cable", 510, 2000));   // the log has no link form: only the frames, two parts
    settle(ui, 2000);
    CHECK(ui.menu() == Menu::Share);
    CHECK_EQ(int(ui.menu_depth()), 1);
    CHECK_EQ(int(ui.page_index()), int(kMenuPageBase) + int(Menu::Share));
    CHECK_EQ(int(ui.share().parts()), 2);
    CHECK(ui.share().cycling());
    CHECK_EQ(int(ui.share().index()), 0);
    CHECK(ui.share().title() == "Test cable");
    CHECK_STR(label(ui, Button::Ok).c_str(), "Hold");
    CHECK_STR(label(ui, Button::Minus).c_str(), "Prev");
    CHECK_STR(label(ui, Button::Plus).c_str(), "Next");
    CHECK(ui.share().modules() >= 21);

    // Part 1 on the panel reads and is part 0 of the record.
    std::array<std::uint8_t, 600> got{};
    qs::Assembler as(got);
    CHECK(feed(ui, as) == qs::Assembler::Result::Accepted);
    CHECK_EQ(int(as.count()), 2);
    CHECK(as.type() == qs::Type::Log);

    // No repaint, no new code, until the cycle time passes.
    const unsigned enc = ui.share().encodes(), pnt = ui.share().paints();
    for (uint32_t t = 2100; t < 2000 + SharePage::kCycleMs; t += 100) ui.tick(t);
    CHECK_EQ(int(ui.share().encodes()), int(enc));
    CHECK_EQ(int(ui.share().paints()), int(pnt));
    CHECK_EQ(int(ui.share().index()), 0);

    // The next part comes up by itself, once.
    ui.tick(2000 + SharePage::kCycleMs + 100);
    while (ui.painting()) ui.tick(2000 + SharePage::kCycleMs + 102);
    CHECK_EQ(int(ui.share().index()), 1);
    CHECK_EQ(int(ui.share().encodes()), int(enc) + 1);
    CHECK_EQ(int(ui.share().paints()), int(pnt) + 1);
    CHECK(feed(ui, as) == qs::Assembler::Result::Done);
    CHECK_EQ(int(as.record().size()), 510);
    CHECK(std::memcmp(as.record().data(), want.data(), 510) == 0);

    // A hand on [+] stops the cycling and stays on the part chosen.
    ui.on_button(Button::Plus, ButtonEvent::Short, 3800);
    CHECK_EQ(int(ui.share().index()), 0);
    CHECK(!ui.share().cycling());
    CHECK_STR(label(ui, Button::Ok).c_str(), "Auto");
    qs::Assembler again(got);
    CHECK(feed(ui, again) == qs::Assembler::Result::Accepted);
    const unsigned enc2 = ui.share().encodes();
    for (uint32_t t = 3900; t < 12000; t += 100) ui.tick(t);
    CHECK_EQ(int(ui.share().index()), 0);
    CHECK_EQ(int(ui.share().encodes()), int(enc2));
    ui.on_button(Button::Minus, ButtonEvent::Short, 12100);
    CHECK_EQ(int(ui.share().index()), 1);
    CHECK(feed(ui, again) == qs::Assembler::Result::Done);

    // OK resumes the cycling.
    ui.on_button(Button::Ok, ButtonEvent::Short, 12200);
    CHECK(ui.share().cycling());
    ui.tick(12200 + SharePage::kCycleMs + 100);
    CHECK_EQ(int(ui.share().index()), 0);

    // A repaint (capture strip, resync) draws the same part again (it encodes it once more: the workspace is not kept).
    const unsigned enc3 = ui.share().encodes();
    ui.resync(15000);
    for (uint32_t ts = 15002; ui.painting(); ts += 2) ui.tick(ts);
    CHECK_EQ(int(ui.share().encodes()), int(enc3) + 1);
    qs::Assembler third(got);
    CHECK(feed(ui, third) == qs::Assembler::Result::Accepted);

    ui.on_button(Button::Back, ButtonEvent::Short, 16000);
    CHECK_EQ(int(ui.menu_depth()), 0);
    CHECK(ui.menu() == Menu::Count);
}

static void test_one_part_and_refusals() {
    g_stub = {};
    static Ui ui;
    ui.init(1000);
    ui.tick(1100);
    CHECK(!ui.open_share(qs::Type::Card, 1, "none", 0, 1200));        // nothing built
    CHECK(!ui.open_share(qs::Type::Card, 1, "big", SharePage::kBuffer + 1, 1200));
    CHECK_EQ(int(ui.menu_depth()), 0);

    // A failed open leaves the page without a record. The capture and rotation tests push every menu, so such a page says
    // "Nothing to share" and takes every key without crashing.
    CHECK(ui.push_menu(Menu::Share, 1250));
    CHECK_EQ(int(ui.share().parts()), 0);
    CHECK_EQ(int(ui.share().modules()), 0);
    ui.tick(1300);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1310);
    ui.on_button(Button::Plus, ButtonEvent::Short, 1320);
    ui.on_button(Button::Minus, ButtonEvent::Short, 1330);
    ui.on_button(Button::Back, ButtonEvent::Short, 1340);
    CHECK_EQ(int(ui.menu_depth()), 0);

    fill_record(ui, 300);
    CHECK(ui.open_share(qs::Type::Log, 1, "A very long card name that does not fit the line", 300, 1300));   // no link form (the log): just one code
    for (uint32_t ts = 1302; ui.painting(); ts += 2) ui.tick(ts);
    CHECK_EQ(int(ui.share().parts()), 1);
    CHECK(!ui.share().has_link_form());
    CHECK(!ui.share().link());
    CHECK(!ui.share().cycling());
    CHECK_EQ(int(ui.share().title().size()), int(SharePage::kTitleMax));
    CHECK_STR(label(ui, Button::Ok).c_str(), "");
    CHECK_STR(label(ui, Button::Plus).c_str(), "");
    std::array<std::uint8_t, 600> got{};
    qs::Assembler as(got);
    CHECK(feed(ui, as) == qs::Assembler::Result::Done);
    CHECK_EQ(int(as.record().size()), 300);
    const unsigned pnt = ui.share().paints();
    for (uint32_t t = 1400; t < 9000; t += 100) ui.tick(t);
    CHECK_EQ(int(ui.share().paints()), int(pnt));          // a steady single code is never redrawn
    ui.on_button(Button::Plus, ButtonEvent::Short, 9100);   // no parts to step: nothing happens
    CHECK_EQ(int(ui.share().index()), 0);
    CHECK_EQ(int(ui.share().paints()), int(pnt));
    ui.on_button(Button::Back, ButtonEvent::Short, 9200);
    CHECK_EQ(int(ui.menu_depth()), 0);
}

// ---- the openers ---------------------------------------------------------------------------------------------------------------

static bool last_packed = false;   // read_record(): the code carried an LZ4 block

// The record the share view holds, read back from the panel (every part, part 0 on show to start with) into `out`; its
// length, 0 when a part does not read.
// A packed record (the LZ4 flag) is inflated, so the openers' tests compare the record the builder made.
// A record that opens as its link form is switched to the K2 data frames first (OK) and stays there: the frames are what this
// reads; read_link() reads the link.
static std::size_t read_record(Ui& ui, std::span<std::uint8_t> out, qs::Type& type) {
    static std::array<std::uint8_t, qs::kMaxRecord> joined;
    if (ui.share().link()) ui.on_button(Button::Ok, ButtonEvent::Short, 19990);
    qs::Assembler as(joined);
    const unsigned parts = ui.share().parts();
    for (unsigned i = 0; i < parts; i++) {
        if (i != 0) ui.on_button(Button::Plus, ButtonEvent::Short, 20000 + i);   // stops the cycling and steps
        const auto r = feed(ui, as);
        if (r != qs::Assembler::Result::Accepted && r != qs::Assembler::Result::Done) return 0;
    }
    type = as.type();
    if (!as.complete()) return 0;
    last_packed = as.packed();
    if (!as.packed()) {
        std::memcpy(out.data(), as.record().data(), as.record().size());
        return as.record().size();
    }
    const std::size_t n = lz::decompress(as.record(), out);
    return n == lz::kError ? 0 : n;
}

static emk_quirk::Quirks eight_rows() {
    emk_quirk::Quirks q;
    q.flags = emk_quirk::kQSopPP | emk_quirk::kQMsgIdEcho;
    q.unmatched = emk_quirk::Unmatched::Nak;
    for (unsigned i = 0; i < emk_quirk::kMaxRows; i++) {
        const std::uint32_t w[3] = {0xAAAA0000u + i, 0xBBBB0000u + i, 0xCCCC0000u + i};
        emk_quirk::add_row(q, emk_quirk::svdm_row(0x22D9, std::uint8_t(16 + i), emk_quirk::Kind::Ack, w,
                                                   i & 1 ? emk_quirk::kSopPP : emk_quirk::kSopP));
    }
    return q;
}

static void test_emarker_opener() {
    const auto old = g_stub;
    g_stub = {};
    g_stub.em.valid = true; g_stub.em.num_vdos = 5; g_stub.em.spec_rev = 2;
    g_stub.em.raw_vdos = {0xFF00A041, (3u << 27) | 0x05ACu, 0, 0x12340005, (2u << 21) | (3u << 9) | (2u << 5)};
    emlib::Entry saved;   // a saved clone with the full quirk table: the 510-byte record, two parts
    saved.seq = 1; saved.version = 1;
    saved.rec = emrec::make({g_stub.em.raw_vdos.data(), 5}, 2);
    std::snprintf(saved.name.data(), saved.name.size(), "OnePlus 100W mag");
    g_stub.lib = {saved};
    g_stub.lib_quirks = {{1, eight_rows()}};
    static Ui ui;
    ui.init(1000); ui.set_rotation(0, 1000); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::Tools), 1100);
    ui.goto_page(kConnTabAlias + 1, 1200);                 // Connection, Cable tab (the remote's alias index)
    ui.tick(1350);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1400);    // the action pop-up
    for (int i = 0; i < 3; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 1500 + i);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1550);    // Saved cables
    CHECK(ui.menu() == Menu::Emarker);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1600);    // the cable's card
    ui.tick(1650);
    CHECK_STR(ui.labels().b[int(Button::Ok)].lng, "Share");
    ui.on_button(Button::Ok, ButtonEvent::Long, 1700);
    CHECK(ui.menu() == Menu::Share);
    CHECK_EQ(int(ui.menu_depth()), 3);
    CHECK(ui.share().type() == qs::Type::Emarker);
    CHECK_EQ(int(ui.share().parts()), 1);                    // the 510-byte record re-packs to v2: a single part
    CHECK(ui.share().v2());
    CHECK(ui.share().has_link_form());                       // 189 B v2 -> a 302-character link: the 8-row cable has one now

    emlib::Entry e;
    emk_quirk::Quirks got_q;
    CHECK(storage::lib_get(1, e, got_q));
    std::array<std::uint8_t, rec::kMaxEmarker> want{};
    const std::size_t wl = rec::emarker(e.rec, std::string_view(e.name.data()), got_q, want);
    CHECK_EQ(int(wl), int(rec::kMaxEmarker));
    CHECK(ui.share().title() == std::string_view(e.name.data()));
    std::array<std::uint8_t, 600> got{};
    qs::Type type = qs::Type::BootInfo;
    const std::size_t n = read_record(ui, got, type);
    std::array<std::uint8_t, rec::kMaxEmarker> exp{};
    const std::size_t en = link_payload(ui, want.data(), wl, exp);
    CHECK_EQ(int(n), int(en));                               // the data code carries the schema v2 record
    CHECK(type == qs::Type::Emarker);
    CHECK(n == en && std::memcmp(got.data(), exp.data(), en) == 0);
    CHECK(!last_packed);                                     // no LZ4 block: the v2 record is the smaller form

    if (const char* d = std::getenv("K2_DUMP")) {   // the two-part code of a saved clone, for docs/ui_mockups/share_cable.png
        while (ui.share().index() != 0) ui.on_button(Button::Plus, ButtonEvent::Short, 22000);
        char path[256];
        std::snprintf(path, sizeof path, "%s/k2_share_cable.panel", d);
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, 240 * 240, f); std::fclose(f); }
    }

    ui.on_button(Button::Back, ButtonEvent::Short, 23000);   // back to the cable's card, one level
    CHECK(ui.menu() == Menu::Emarker);
    CHECK_EQ(int(ui.menu_depth()), 2);
    CHECK_STR(ui.labels().b[int(Button::Ok)].shrt, "Delete");
    g_stub = old;
}

static void test_cards_opener() {
    const auto old = g_stub;
    g_stub = {};
    stub_cards_reset();
    rc::Card c;
    c.id = {0x1A2B3C4D, 0x0B05, 0x1234};
    c.num_pdos = 5; c.spec_rev = 2; c.boots = 4; c.fw = 0x0FF2C610; c.uptime_s = 300;
    c.group[0] = {rc::GV::Pass, 20000, 0, 0};
    c.group[2] = {rc::GV::Fail, 0, 0, uint8_t(charger_test::Reason::Timeout)};
    c.n_ranges = 2;
    c.range[0] = {uint8_t(charger_test::Algorithm::Pps), 0, 33, 210, 200, 130};
    c.range[1] = {uint8_t(charger_test::Algorithm::SprAvs), 0, 150, 280, 280, 0};
    rc::set_name(c, "Anker 735");
    CHECK_EQ(int(rc::hw::put(c)), int(rc::Err::Ok));
    static Ui ui;
    ui.init(1000); ui.tick(1000);
    ui.goto_page(uint8_t(Page::Tools), 1050);
    ui.push_menu(Menu::Cards, 1100);
    ui.tick(1150);
    ui.on_button(Button::Ok, ButtonEvent::Long, 1200);       // the list has no Share
    CHECK(ui.menu() == Menu::Cards);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1300);      // the card
    ui.tick(1350);
    CHECK_STR(ui.labels().b[int(Button::Ok)].lng, "Share");
    ui.on_button(Button::Ok, ButtonEvent::Long, 1400);
    for (uint32_t ts = 1402; ui.painting(); ts += 2) ui.tick(ts);
    CHECK(ui.menu() == Menu::Share);
    CHECK(ui.share().type() == qs::Type::Card);
    CHECK_EQ(int(ui.share().parts()), 1);
    CHECK(ui.share().title() == "Anker 735");
    rc::Card stored;
    CHECK(rc::hw::find(0x1A2B3C4D, stored));
    CHECK(ui.share().title() == "Anker 735");
    std::array<std::uint8_t, rec::kMaxCard> want{};
    const std::size_t wl = rec::card(stored, want);
    CHECK(wl > 0);
    std::array<std::uint8_t, 600> got{};
    CHECK(ui.share().link());                                // a card opens as the link a phone camera follows
    std::array<std::uint8_t, rec::kMaxCard> exp{};
    const std::size_t en = link_payload(ui, want.data(), wl, exp);
    CHECK_EQ(int(read_link(ui, got)), int(en));              // the version '2' link carries the v2 record
    CHECK(std::memcmp(got.data(), exp.data(), en) == 0);
    CHECK_EQ(link_kind, 'c');
    dump_panel("k2_share_card_link");
    ui.on_button(Button::Ok, ButtonEvent::Short, 1450);      // OK: the K2 data code, for tools/k2_qr.py
    CHECK(!ui.share().link());
    dump_panel("k2_share_card_data");
    qs::Type type = qs::Type::BootInfo;
    const std::size_t n = read_record(ui, got, type);
    CHECK(n == en && std::memcmp(got.data(), exp.data(), en) == 0);   // the schema 2 frame carries the same v2 record
    CHECK(type == qs::Type::Card);
    ui.on_button(Button::Back, ButtonEvent::Short, 1500);
    CHECK(ui.menu() == Menu::Cards);
    stub_cards_reset();
    g_stub = old;
}

static void test_pd_opener() {
    const auto old = g_stub;
    g_stub = {};
    g_stub.caps.valid = true;
    g_stub.caps.num_pdos = 5;
    g_stub.caps.active_cc = 1;
    g_stub.caps.spec_rev = 2;
    g_stub.caps.raw_header = 0x51A1;
    const std::uint32_t w[5] = {0x0801912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0xC1DC0 | (0x3u << 30)};
    for (unsigned i = 0; i < 5; i++) g_stub.caps.pdos[i].raw = w[i];
    std::array<std::uint8_t, rec::kMaxPdCaps> want{};
    const std::size_t wl = rec::pdcaps(g_stub.caps, want);
    CHECK_EQ(int(wl), 6 + 4 * 5);
    static Ui ui;
    ui.init(1000); ui.tick(1000);
    ui.goto_page(kPdPage, 1050);
    ui.tick(1200);
    CHECK_STR(ui.labels().b[int(Button::Ok)].lng, "Share");
    ui.on_button(Button::Ok, ButtonEvent::Long, 1300);
    for (uint32_t ts = 1302; ui.painting(); ts += 2) ui.tick(ts);
    CHECK(ui.menu() == Menu::Share);
    CHECK(ui.share().type() == qs::Type::PdCaps);
    CHECK_EQ(int(ui.share().parts()), 1);
    std::array<std::uint8_t, 600> got{};
    CHECK(ui.share().link());                                // the source's PDOs open as a link: the page shows the table
    CHECK_EQ(int(read_link(ui, got)), int(wl));
    CHECK(std::memcmp(got.data(), want.data(), wl) == 0);
    CHECK(link_type(link_kind) == qs::Type::PdCaps);
    dump_panel("k2_share_pdcaps_link");
    qs::Type type = qs::Type::BootInfo;
    const std::size_t n = read_record(ui, got, type);
    CHECK(n == wl && std::memcmp(got.data(), want.data(), wl) == 0);
    CHECK(type == qs::Type::PdCaps);
    ui.on_button(Button::Back, ButtonEvent::Short, 1400);
    CHECK_EQ(int(ui.menu_depth()), 1);   // back on Connection (Tools > Connection > Share)
    CHECK(on_pd(ui));

    // The Cable tab has no Share without a cable read, and neither does a page without caps.
    ui.on_button(Button::Plus, ButtonEvent::Short, 1500);
    ui.tick(1600);
    CHECK_STR(ui.labels().b[int(Button::Ok)].lng, "");
    ui.on_button(Button::Ok, ButtonEvent::Long, 1700);
    CHECK_EQ(int(ui.menu_depth()), 1);
    ui.on_button(Button::Minus, ButtonEvent::Short, 1800);
    g_stub.caps.valid = false;
    ui.tick(1900);
    CHECK_STR(ui.labels().b[int(Button::Ok)].lng, "");
    ui.on_button(Button::Ok, ButtonEvent::Long, 2000);
    CHECK_EQ(int(ui.menu_depth()), 1);
    g_stub = old;
}

// The charger test's Results: long OK shares the report card the run saved.
static void test_charger_results_opener() {
    const auto old = g_stub;
    g_stub = {}; pd::set_fake(0); stub_cards_reset();
    g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 1; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2;
    g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300);
    g_stub.caps.raw_header = 0x1181;
    static Ui ui;
    ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    ui.goto_page(uint8_t(Page::ChargerTest), 1100);
    auto press = [&](Button b, std::uint32_t t) { ui.on_button(b, ButtonEvent::Short, t); };
    for (unsigned i = 0; i < charger_test::kMaxSteps; ++i) press(Button::Minus, 1200);
    press(Button::Ok, 1200);                       // Algorithms -> Cable
    press(Button::Ok, 1200);                       // Cable -> Review
    CHECK_EQ(ui.charger_test_stage(), 4);
    // Before a run there is nothing to share.
    ui.on_button(Button::Ok, ButtonEvent::Long, 1250);
    CHECK_EQ(int(ui.menu_depth()), 0);
    press(Button::Ok, 1300);                       // START: the wait, then the confirm window
    for (std::uint32_t t = 1320; t < 41320 && ui.charger_test_stage() == 13; t += 20) { fusb302::service(); ui.tick(t); }
    press(Button::Ok, 1400);
    for (std::uint32_t t = 1500; t <= 600000; t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);          // Results
    CHECK_STR(ui.labels().b[int(Button::Ok)].lng, "Share");
    ui.on_button(Button::Ok, ButtonEvent::Long, 700000);
    for (uint32_t ts = 700002; ui.painting(); ts += 2) ui.tick(ts);
    CHECK(ui.menu() == Menu::Share);
    CHECK(ui.share().type() == qs::Type::Card);
    CHECK_EQ(int(ui.share().parts()), 1);
    std::array<report_card::Card, 1> cards{};
    CHECK_EQ(int(report_card::hw::list(cards)), 1);
    std::array<std::uint8_t, rec::kMaxCard> want{};
    const std::size_t wl = rec::card(cards[0], want);
    std::array<std::uint8_t, 600> got{};
    CHECK(ui.share().link());
    std::array<std::uint8_t, rec::kMaxCard> exp{};
    const std::size_t en = link_payload(ui, want.data(), wl, exp);
    CHECK_EQ(int(read_link(ui, got)), int(en));
    CHECK(std::memcmp(got.data(), exp.data(), en) == 0);
    ui.on_button(Button::Ok, ButtonEvent::Short, 700050);    // the K2 data code
    qs::Type type = qs::Type::BootInfo;
    const std::size_t n = read_record(ui, got, type);
    CHECK(n == en && std::memcmp(got.data(), exp.data(), en) == 0);
    ui.on_button(Button::Back, ButtonEvent::Short, 700100);
    CHECK_EQ(int(ui.menu_depth()), 0);
    CHECK_EQ(ui.charger_test_stage(), 6);
    g_stub = old;
}

// A record that shrinks by a quarter goes out as an LZ4 block (one part instead of two); one that does not goes out as built.
static void test_packs_when_it_pays() {
    g_stub = {};
    static Ui ui;
    ui.init(1000);
    ui.tick(1100);
    auto b = ui.share_buffer();
    for (std::size_t i = 0; i < 510; i++) b[i] = std::uint8_t(i % 31 < 20 ? 0x55 : i % 5);
    std::array<std::uint8_t, 510> want;
    for (std::size_t i = 0; i < want.size(); i++) want[i] = b[i];
    CHECK(ui.open_share(qs::Type::Emarker, 1, "Packed", 510, 2000));
    CHECK(ui.share().packed());
    CHECK(ui.share().record().size() * 4 <= 510u * 3);
    CHECK_EQ(int(ui.share().parts()), 1);
    std::array<std::uint8_t, 600> got{};
    qs::Type type = qs::Type::BootInfo;
    CHECK_EQ(int(read_record(ui, got, type)), 510);
    CHECK(last_packed && std::memcmp(got.data(), want.data(), 510) == 0);
    ui.on_button(Button::Back, ButtonEvent::Short, 3000);

    fill_record(ui, 300);                                          // random bytes: kept as built
    CHECK(ui.open_share(qs::Type::Emarker, 1, "Plain", 300, 4000));
    CHECK(!ui.share().packed());
    CHECK_EQ(int(ui.share().record().size()), 300);
    CHECK_EQ(int(read_record(ui, got, type)), 300);
    CHECK(!last_packed);
}

static void bin_push(BinLog<200>& ring, std::uint32_t ms, std::uint16_t id, std::uint32_t sig, ...) {
    std::va_list ap;
    va_start(ap, sig);
    ring.push(ms, id, sig, ap);
    va_end(ap);
}

// Diag: long OK shares the newest log entries; the record carries the build's git string and the uptime base.
static void test_log_opener() {
    const auto old = g_stub;
    g_stub = {};
    BinLog<200> ring;
    bin_push(ring, 100, 6, logfmt::kString, "OK");
    bin_push(ring, 140, 8, logfmt::kSigned, -7);
    bin_push(ring, 141, 2, 0);
    std::array<std::uint8_t, 200> snap{};
    const BinLogSnapshot s = ring.snapshot(snap);
    g_stub.bin_log.assign(snap.begin(), snap.begin() + s.len);
    g_stub.bin_base = 55;
    g_stub.bin_older = true;
    static Ui ui;
    ui.init(1000); ui.tick(1000);
    ui.goto_page(kDiagPage, 1050);
    ui.tick(1200);
    CHECK_STR(ui.labels().b[int(Button::Ok)].lng, "Share");
    CHECK_STR(ui.labels().b[int(Button::Ok)].shrt, "");
    ui.on_button(Button::Ok, ButtonEvent::Long, 1300);
    for (uint32_t ts = 1302; ui.painting(); ts += 2) ui.tick(ts);
    CHECK(ui.menu() == Menu::Share);
    CHECK(ui.share().type() == qs::Type::Log);
    std::array<std::uint8_t, 600> got{};
    qs::Type type = qs::Type::BootInfo;
    const std::size_t n = read_record(ui, got, type);
    const std::string_view git = kGitHash;
    CHECK_EQ(int(n), int(1 + git.size() + 1 + 8 + s.len));
    CHECK_EQ(int(got[0]), int(git.size()));
    CHECK(std::memcmp(got.data() + 1, git.data(), git.size()) == 0);
    const std::uint8_t* p = got.data() + 1 + git.size();
    CHECK_EQ(int(p[0]), int(rec::kLogOlder));
    CHECK_EQ(int(p[1] | p[2] << 8), 55);
    CHECK(std::memcmp(p + 9, snap.data(), s.len) == 0);
    ui.on_button(Button::Back, ButtonEvent::Short, 1400);
    CHECK_EQ(int(ui.menu_depth()), 1);   // back on Diag (Tools > Diag > Share)
    CHECK(on_diag(ui));

    // An empty ring has nothing to share: the press does nothing.
    g_stub.bin_log.clear();
    ui.on_button(Button::Ok, ButtonEvent::Long, 1500);
    CHECK_EQ(int(ui.menu_depth()), 1);
    g_stub = old;
}

// A card opens as the link form; OK switches to the K2 data code and back; the other keys do nothing; the worst case still reads.
static void test_link_form(uint8_t rotation) {
    g_stub = {};
    static Ui ui;
    ui.init(1000);
    ui.set_rotation(rotation, 1000);
    ui.tick(1100);
    for (const bool packs : {false, true}) {
        auto b = ui.share_buffer();
        if (packs) for (std::size_t i = 0; i < 184; i++) b[i] = std::uint8_t(i % 31 < 20 ? 0x55 : i % 5);
        else fill_record(ui, 184);                        // incompressible: the worst-case 296-character link
        std::array<std::uint8_t, 184> want;
        for (std::size_t i = 0; i < want.size(); i++) want[i] = b[i];
        CHECK(ui.open_share(qs::Type::Card, 5, "Anker 735", 184, 2000));
        CHECK(ui.share().link());
        CHECK_EQ(int(ui.share().packed()), int(packs));
        CHECK(!ui.share().cycling());
        ui.tick(2100);
        for (uint32_t ts = 2102; ui.painting(); ts += 2) ui.tick(ts);
        std::array<std::uint8_t, 600> got{};
        CHECK_EQ(int(read_link(ui, got)), 184);
        CHECK(std::memcmp(got.data(), want.data(), 184) == 0);
        CHECK_EQ(int(link_kind), int(packs ? lk::kKindCardPacked : lk::kKindCard));
        if (!packs) {   // 296 characters: version 11 (61 modules) at 3 px, the camera-safe size, quiet zone 4
            CHECK_EQ(ui.share().modules(), 61);
            CHECK_EQ(ui.share().plan().px, 3);
            CHECK_EQ(ui.share().look().quiet, 4);
            CHECK_EQ(ui.share().plan().box, 207);
        }
        CHECK_STR(label(ui, Button::Ok).c_str(), "K2 data");
        CHECK_STR(label(ui, Button::Plus).c_str(), "");
        CHECK_STR(label(ui, Button::Minus).c_str(), "");
        dump_panel(rotation == 3 && !packs ? "k2_share_link_worst" : rotation == 3 ? "k2_share_link" : nullptr);

        const unsigned pnt = ui.share().paints();
        for (uint32_t t = 2200; t < 9000; t += 100) ui.tick(t);
        ui.on_button(Button::Plus, ButtonEvent::Short, 9100);
        ui.on_button(Button::Minus, ButtonEvent::Short, 9110);
        ui.tick(9200);
        CHECK_EQ(int(ui.share().paints()), int(pnt));     // a steady code is never redrawn; the other keys do nothing
        CHECK(ui.share().link());

        ui.on_button(Button::Ok, ButtonEvent::Short, 9300);   // the data code
        ui.tick(9400);
        CHECK(!ui.share().link());
        CHECK_STR(label(ui, Button::Ok).c_str(), "Link");
        CHECK(ui.share().title() == "Anker 735");
        std::array<std::uint8_t, 600> rec_got{};
        qs::Type type = qs::Type::BootInfo;
        CHECK_EQ(int(read_record(ui, rec_got, type)), 184);
        CHECK(std::memcmp(rec_got.data(), want.data(), 184) == 0);
        CHECK(type == qs::Type::Card);
        CHECK_EQ(int(read_link(ui, got)), 0);             // the panel holds the frame text now

        ui.on_button(Button::Ok, ButtonEvent::Short, 9500);   // and back
        ui.tick(9600);
        CHECK(ui.share().link());
        CHECK_EQ(int(read_link(ui, got)), 184);
        ui.on_button(Button::Back, ButtonEvent::Short, 9700);
        CHECK_EQ(int(ui.menu_depth()), 0);
    }
}

// Every kind that fits one link opens as a link; the kind letter and the caption follow the type; a record one byte over the limit
// has no link and no toggle; one that packs under the limit has the uppercase letter.
static void test_link_kinds() {
    g_stub = {};
    static Ui ui;
    ui.init(1000);
    ui.tick(1100);
    struct Case { qs::Type type; std::size_t len; char kind; std::string_view caption; };
    const Case cases[] = {
        {qs::Type::Emarker, 91, 'e', "Scan: cable details"},
        {qs::Type::Emarker, ShareRecord::kBuffer, 'e', "Scan: cable details"},   // 510 B: a 730 character link, version 19 (2 px)
        {qs::Type::PdCaps, 26, 'p', "Scan: charger offers"},
        {qs::Type::PdCaps, 50, 'p', "Scan: charger offers"},
        {qs::Type::Identity, 40, 'i', "Scan: firmware info"},
        {qs::Type::BootInfo, 180, 'b', "Scan: firmware info"},
        {qs::Type::Card, 184, 'c', "Scan: full results"},
    };
    std::uint32_t t = 2000;
    for (const Case& c : cases) {
        CHECK(SharePage::link_caption(c.type) == c.caption);
        CHECK(c.caption.size() <= 37);                       // a 224 px line of 6 px cells
        fill_record(ui, c.len);
        std::array<std::uint8_t, 512> want{};
        for (std::size_t i = 0; i < c.len; i++) want[i] = ui.share_buffer()[i];
        CHECK(ui.open_share(c.type, 1, "Some name", c.len, t));
        CHECK(ui.share().has_link_form());
        CHECK(ui.share().link());
        CHECK(!ui.share().cycling());
        ui.tick(t + 100);
        for (uint32_t ts = t + 102; ui.painting(); ts += 2) ui.tick(ts);
        std::array<std::uint8_t, 600> got{};
        CHECK_EQ(int(read_link(ui, got)), int(c.len));
        CHECK(std::memcmp(got.data(), want.data(), c.len) == 0);
        CHECK_EQ(int(link_kind), int(c.kind));
        CHECK(link_type(link_kind) == c.type);
        CHECK(ui.share().modules() > 0 && ui.share().modules() <= qrview::modules_of(lk::kMaxVersion));   // one code within the policy
        CHECK(ui.share().plan().px >= 2);                    // 3 px up to version 13, 2 px beyond
        CHECK_STR(label(ui, Button::Ok).c_str(), "K2 data");
        ui.on_button(Button::Ok, ButtonEvent::Short, t + 200);
        CHECK(!ui.share().link());
        CHECK_STR(label(ui, Button::Ok).c_str(), "Link");
        qs::Type type = qs::Type::Log;
        std::array<std::uint8_t, 600> rec_got{};
        CHECK_EQ(int(read_record(ui, rec_got, type)), int(c.len));
        CHECK(type == c.type && std::memcmp(rec_got.data(), want.data(), c.len) == 0);
        ui.on_button(Button::Back, ButtonEvent::Short, t + 300);
        CHECK_EQ(int(ui.menu_depth()), 0);
        t += 1000;
    }

    // A record that packs under the limit takes the uppercase letter.
    auto b = ui.share_buffer();
    for (std::size_t i = 0; i < 400; i++) b[i] = std::uint8_t(i % 31 < 20 ? 0x55 : i % 5);
    CHECK(ui.open_share(qs::Type::Emarker, 1, "Packed", 400, t));
    CHECK(ui.share().packed());
    CHECK(ui.share().link());
    ui.tick(t + 100);
    for (uint32_t ts = t + 102; ui.painting(); ts += 2) ui.tick(ts);
    std::array<std::uint8_t, 600> got{};
    CHECK_EQ(int(read_link(ui, got)), 400);
    CHECK_EQ(int(link_kind), int('E'));
    ui.on_button(Button::Back, ButtonEvent::Short, t + 200);
    t += 1000;

    // A type without a link form (the log): no toggle, the data frames (two parts here) cycle as they always did.
    fill_record(ui, ShareRecord::kBuffer);
    CHECK(ui.open_share(qs::Type::Log, 1, "Big", ShareRecord::kBuffer, t));
    CHECK(!ui.share().packed());
    CHECK(!ui.share().has_link_form());
    CHECK(!ui.share().link());
    ui.tick(t + 100);
    for (uint32_t ts = t + 102; ui.painting(); ts += 2) ui.tick(ts);
    CHECK_STR(label(ui, Button::Ok).c_str(), ui.share().parts() > 1 ? "Hold" : "");
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 200);   // OK never makes a link out of it
    ui.tick(t + 300);
    CHECK(!ui.share().link());
    CHECK_EQ(int(read_link(ui, got)), 0);
    ui.on_button(Button::Back, ButtonEvent::Short, t + 400);
    t += 1000;

    // The real records of each kind: the link opens, and the screens are dumped for the renders of a change (K2_DUMP).
    {
        struct Real { qs::Type type; const char* name; const char* dump; };
        const Real reals[3] = {{qs::Type::Emarker, "OnePlus 100W mag", "k2_share_cable_link"},
                               {qs::Type::Identity, "Firmware", "k2_share_identity_link"},
                               {qs::Type::BootInfo, "Boot report", "k2_share_boot_link"}};
        for (const Real& r : reals) {
            std::array<std::uint8_t, 256> bytes{};
            std::size_t n = 0;
            if (r.type == qs::Type::Emarker) n = rec::emarker(qsfx::oneplus_mag_clone(), r.name, qsfx::quirk_rows(2), bytes);
            else if (r.type == qs::Type::Identity) n = rec::identity("1.0", "fcb3128-dirty", 1, bytes);
            else n = rec::boot(qsfx::boot_snapshot(), bytes);
            CHECK(n > 0);
            auto b2 = ui.share_buffer();
            for (std::size_t i = 0; i < n; i++) b2[i] = bytes[i];
            CHECK(ui.open_share(r.type, 1, r.name, n, t));
            CHECK(ui.share().has_link_form());
            ui.tick(t + 100);
            for (uint32_t ts = t + 102; ui.painting(); ts += 2) ui.tick(ts);
            std::array<std::uint8_t, 600> got2{};
            std::array<std::uint8_t, rec::kMaxEmarker> exp2{};
            const std::size_t en = link_payload(ui, bytes.data(), n, exp2);
            CHECK_EQ(int(read_link(ui, got2)), int(en));      // the cable's version '2' link carries its v2 record
            CHECK(std::memcmp(got2.data(), exp2.data(), en) == 0);
            CHECK(ui.share().modules() <= 61);
            dump_panel(r.dump);
            ui.on_button(Button::Back, ButtonEvent::Short, t + 200);
            t += 1000;
        }
    }

    // The log has no link form at any size.
    fill_record(ui, 20);
    CHECK(ui.open_share(qs::Type::Log, 1, "Log", 20, t));
    CHECK(!ui.share().has_link_form());
    CHECK(!ui.share().link());
    ui.on_button(Button::Back, ButtonEvent::Short, t + 100);
}

// ---- the full-screen look -----------------------------------------------------------------------------------------------------

static bool row_has_black(int y, int x0 = 0, int x1 = 240) {
    for (int x = x0; x < x1; x++) if (g_panel[y * 240 + x] == 0x0000) return true;
    return false;
}

// A code shows full screen and centred, at the size its version allows, whatever the rotation; the button strip is not drawn
// (the code may cover it), the corners stay white, a steady screen is not redrawn, and the code reads back.
static void test_full_screen_centred(uint8_t rotation) {
    g_stub = {};
    static Ui ui;
    ui.init(1000);
    ui.set_rotation(rotation, 1000);
    ui.tick(1100);
    bool saw3 = false, saw2 = false;
    std::uint32_t t = 2000;
    for (const std::size_t len : {20u, 91u, 184u, 210u, 270u, 330u, 400u, 510u}) {
        fill_record(ui, len);
        std::array<std::uint8_t, 512> want{};
        for (std::size_t i = 0; i < len; i++) want[i] = ui.share_buffer()[i];
        CHECK(ui.open_share(qs::Type::Emarker, 1, "Cable", len, t));
        ui.tick(t + 100);
        for (uint32_t ts = t + 102; ui.painting(); ts += 2) ui.tick(ts);
        const int n = ui.share().modules();
        const int v = qrview::version_of(n);
        const qrview::Look lo = qrview::look_for(v);
        CHECK(n > 0 && lo.px >= 2);
        CHECK(!lo.wide);
        CHECK_EQ(lo.px, v <= 13 ? 3 : 2);
        (lo.px == 3 ? saw3 : saw2) = true;
        const int box = qrview::box_of(v, lo);
        CHECK_EQ(ui.share().plan().px, lo.px);
        CHECK_EQ(ui.share().plan().box, box);
        CHECK_EQ(ui.share().look().quiet, lo.quiet);

        // the panel itself: the first black module row and column (the top finder patterns, above any caption) and the last
        // black column are as far from the edges as the box centred on the 240 px panel puts them
        int top = 240, left = 240, right = -1;
        for (int y = 0; y < 120; y++)
            for (int x = 0; x < 240; x++)
                if (g_panel[y * 240 + x] == 0x0000) { top = std::min(top, y); left = std::min(left, x); right = std::max(right, x); }
        CHECK_EQ(left, (240 - box) / 2 + lo.quiet * lo.px);
        CHECK_EQ(top, left);
        CHECK(std::abs(left - (239 - right)) <= 1);
        for (const int c : {0, 239, 239 * 240, 239 * 240 + 239}) CHECK_EQ(int(g_panel[c]), 0xFFFF);   // no strip, no chrome

        std::array<std::uint8_t, 600> got{};
        CHECK_EQ(int(read_link(ui, got)), int(len));
        CHECK(std::memcmp(got.data(), want.data(), len) == 0);

        // the caption sits in the margin under the box when the margin holds a line, else nothing is drawn there
        CHECK_EQ(int(ui.share().caption_shown()), int(qrview::caption_fits(box, SharePage::kTextH)));
        bool below = false;
        for (int y = (240 - box) / 2 + box; y < 240; y++) below = below || row_has_black(y);
        CHECK_EQ(int(below), int(ui.share().caption_shown()));

        static std::array<std::uint16_t, 240 * 240> snap;
        std::copy(g_panel, g_panel + 240 * 240, snap.begin());
        const unsigned pnt = ui.share().paints(), win = g_lcd_windows;
        for (std::uint32_t k = t + 200; k < t + 6000; k += 100) ui.tick(k);
        ui.on_button(Button::Plus, ButtonEvent::Short, t + 6100);
        ui.on_button(Button::Minus, ButtonEvent::Short, t + 6110);
        ui.tick(t + 6200);
        CHECK_EQ(int(ui.share().paints()), int(pnt));
        CHECK_EQ(int(g_lcd_windows), int(win));
        CHECK(std::equal(snap.begin(), snap.end(), g_panel));
        ui.on_button(Button::Back, ButtonEvent::Short, t + 6300);
        CHECK_EQ(int(ui.menu_depth()), 0);
        t += 10000;
    }
    CHECK(saw3 && saw2);
}

// The look policy's constants the page relies on.
static void test_look_constants() {
    CHECK_EQ(int(SharePage::kBusyChars), qrtest::byte_l(SharePage::kBusyVersion - 1));   // past version 17's capacity
    CHECK(SharePage::hint_for(qrview::look_for(21)).empty());
    CHECK(SharePage::hint_for(qrview::look_for(13)).empty());
    for (const int v : {22, 23, 24}) CHECK(SharePage::hint_for(qrview::look_for(v)) == "Use the wide camera");
    CHECK(SharePage::kBusyText.size() <= 40);
    CHECK(SharePage::kWideHint.size() <= 39);
}

// Long OK shows the text view (what the code is, and the wide-camera hint for a code that needs it) and back; BACK from the
// text view returns to the code instead of leaving the share. A code whose margin cannot hold the caption has it there.
static void test_info_view() {
    g_stub = {};
    static Ui ui;
    ui.init(1000);
    ui.tick(1100);
    static std::array<std::uint16_t, 240 * 240> code_panel;

    fill_record(ui, 180);   // a 290 character link: version 11, the caption fits the margin
    CHECK(ui.open_share(qs::Type::Emarker, 1, "Cable", 180, 2000));
    ui.tick(2100);
    for (uint32_t ts = 2102; ui.painting(); ts += 2) ui.tick(ts);
    CHECK_EQ(qrview::version_of(ui.share().modules()), 11);
    CHECK(ui.share().caption_shown());
    ui.on_button(Button::Back, ButtonEvent::Short, 2200);
    CHECK_EQ(int(ui.menu_depth()), 0);

    fill_record(ui, 210);   // 330 characters: version 12, box 219 px, the caption does not fit
    CHECK(ui.open_share(qs::Type::Emarker, 1, "Cable", 210, 3000));
    ui.tick(3100);
    for (uint32_t ts = 3102; ui.painting(); ts += 2) ui.tick(ts);
    CHECK_EQ(qrview::version_of(ui.share().modules()), 12);
    CHECK(!ui.share().caption_shown());
    CHECK(!ui.share().info());
    std::copy(g_panel, g_panel + 240 * 240, code_panel.begin());
    const unsigned enc = ui.share().encodes();

    ui.on_button(Button::Ok, ButtonEvent::Long, 3200);
    CHECK(ui.share().info());
    CHECK_EQ(int(ui.share().encodes()), int(enc));                       // text only: no code is built for it
    CHECK(!std::equal(code_panel.begin(), code_panel.end(), g_panel));
    bool text_rows = false;
    for (int y = 16; y < 64; y++) text_rows = text_rows || row_has_black(y);   // the text lines
    CHECK(text_rows);
    ui.tick(3300 + SharePage::kCycleMs * 3);
    CHECK(ui.share().info());                                            // stays until a key
    ui.on_button(Button::Back, ButtonEvent::Short, 3400);
    CHECK(!ui.share().info());
    CHECK(ui.menu() == Menu::Share);                                     // BACK left the text view, not the share
    CHECK_EQ(int(ui.menu_depth()), 1);
    CHECK_EQ(int(ui.share().encodes()), int(enc) + 1);
    CHECK(std::equal(code_panel.begin(), code_panel.end(), g_panel));

    ui.on_button(Button::Ok, ButtonEvent::Long, 3500);                   // and Long OK again returns
    CHECK(ui.share().info());
    ui.on_button(Button::Ok, ButtonEvent::Long, 3600);
    CHECK(!ui.share().info());
    CHECK(std::equal(code_panel.begin(), code_panel.end(), g_panel));
    ui.on_button(Button::Ok, ButtonEvent::Long, 3700);
    ui.on_button(Button::Ok, ButtonEvent::Short, 3800);                  // a key in the text view acts, and shows its result
    CHECK(!ui.share().info());
    CHECK(!ui.share().link());                                           // OK switched to the K2 data code
    ui.on_button(Button::Back, ButtonEvent::Short, 3900);
    CHECK_EQ(int(ui.menu_depth()), 0);

    // Nothing to share: the text view does not open.
    CHECK(!ui.open_share(qs::Type::Card, 1, "none", 0, 3950));
    CHECK(ui.push_menu(Menu::Share, 4000));
    ui.on_button(Button::Ok, ButtonEvent::Long, 4100);
    CHECK(!ui.share().info());
    ui.on_button(Button::Back, ButtonEvent::Short, 4200);
    CHECK_EQ(int(ui.menu_depth()), 0);
}

// A code of version 18 and up says "Preparing code..." before the slow encode and keeps servicing USB through it (one poll per mask
// candidate); a smaller code does neither. The erase pass of a page change builds no code at all.
static void test_busy_and_erase() {
    g_stub = {};
    static Ui ui;
    ui.init(1000);
    ui.tick(1100);

    fill_record(ui, 180);   // version 11
    CHECK(ui.open_share(qs::Type::Emarker, 1, "Cable", 180, 2000));
    CHECK_EQ(int(ui.share().busy_shown()), 0);
    ui.on_button(Button::Back, ButtonEvent::Short, 2100);

    fill_record(ui, 510);   // a 730 character link: version 19
    std::array<std::uint8_t, 512> want{};
    for (std::size_t i = 0; i < 510; i++) want[i] = ui.share_buffer()[i];
    g_pump_snap_armed = true;
    const unsigned pumps = g_lcd_pumps;
    CHECK(ui.open_share(qs::Type::Emarker, 1, "Cable", 510, 3000));
    for (uint32_t ts = 3002; ui.painting(); ts += 2) ui.tick(ts);
    CHECK_EQ(qrview::version_of(ui.share().modules()), 19);
    CHECK_EQ(int(ui.share().busy_shown()), 1);
    CHECK(g_lcd_pumps - pumps >= 8);
    CHECK(!g_pump_snap_armed);                                           // the first poll came during the encode
    // what the panel showed then: the notice on one row, nothing else
    bool notice = false, stray = false;
    for (int y = 0; y < 240; y++) {
        bool black = false;
        for (int x = 0; x < 240; x++) black = black || g_pump_panel[y * 240 + x] == 0x0000;
        if (y >= 114 && y < 126) notice = notice || black; else stray = stray || black;
    }
    CHECK(notice);
    CHECK(!stray);
    std::array<std::uint8_t, 600> got{};
    CHECK_EQ(int(read_link(ui, got)), 510);                              // and then the code
    CHECK(std::memcmp(got.data(), want.data(), 510) == 0);
    CHECK_EQ(ui.share().plan().px, 2);

    // The erase pass paints the background and builds nothing.
    const unsigned enc = ui.share().encodes(), pumps2 = g_lcd_pumps;
    lcd::set_erase(true);
    const_cast<SharePage&>(ui.share()).draw_static();
    lcd::set_erase(false);
    CHECK_EQ(int(ui.share().encodes()), int(enc));
    CHECK_EQ(int(g_lcd_pumps), int(pumps2));
    bool all_bg = true;
    for (int i = 0; i < 240 * 240; i++) all_bg = all_bg && g_panel[i] == 0;
    CHECK(all_bg);
    ui.on_button(Button::Back, ButtonEvent::Short, 3500);
    CHECK_EQ(int(ui.menu_depth()), 0);
}

int main() {
    for (uint8_t r = 0; r < 4; r++) test_full_screen_centred(r);
    test_look_constants();
    test_info_view();
    test_busy_and_erase();
    for (uint8_t r = 0; r < 4; r++) test_link_form(r);
    test_link_kinds();
    test_plan_in();
    test_packs_when_it_pays();
    for (uint8_t r = 0; r < 4; r++) test_two_parts(r);
    test_one_part_and_refusals();
    test_emarker_opener();
    test_cards_opener();
    test_pd_opener();
    test_charger_results_opener();
    test_log_opener();
    return test_report("test_ui_share");
}
