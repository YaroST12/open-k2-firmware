// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The no-truncation walk: every page, every submenu, every Connection tab and the charger-test wizard are drawn in all four
// rotations over idle, ordinary and extreme device states, and any UI copy that had to be cut (fit_text, a Field cell grid,
// gfx_text's surface clip) fails the test through the recorder in tests/trunc_log.cpp. test_report() prints each string, its
// box width and the call site, and the label set here ("conn-sink rot 1 epr11") says which screen it was on.
// External data (saved names, vendor strings, PDO lists read off the device) uses fit_data and is only counted.
// A truncation is fixed by rewording the copy (docs/UI_DESIGN.md section 6), not by hiding it here.
#include "test_util.h"
#include "card_fixtures.h"
#include "fake_lcd.h"
#include "k2/chargertest/report_card_hw.hpp"
#include "ui_stubs.h"
#include "ui_nav.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/text.hpp"
#include "k2/chargertest/resume_text.hpp"
#include "k2/chargertest/result_layout.hpp"
#include "k2/pd/emrec.hpp"
#include <string>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <iterator>

using namespace k2;
namespace ct = charger_test;

namespace {

uint32_t g_t = 10000;   // the stub clock this test drives

struct Scenario {
    const char *name;
    int pdos;       // 0 none, 1..11 PDOs of the extreme list
    bool epr;       // the source's list carries the EPR levels (28 / 36 / 48 V)
    bool cable;     // an e-marker is on the cable
    bool load;      // a load: high power flowing on VBUS
};
constexpr Scenario kScenarios[] = {
    {"idle", 0, false, false, false},
    {"spr7", 7, false, false, false},
    {"epr11", 11, true, true, true},
    {"spr3-cable", 3, false, true, false},
};

void apply(const Scenario &sc) {
    g_stub = {};
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = sc.load ? 36000 : 5000;
    g_stub.pm.current_ma = sc.load ? 5000 : 1000;
    g_stub.pm.power_mw = static_cast<uint32_t>(sc.load ? 180000 : 5000);
    g_stub.pm.flow = power::Flow::Forward;
    g_stub.caps = {};
    if (sc.pdos) {
        g_stub.caps.valid = true;
        g_stub.caps.num_pdos = static_cast<uint8_t>(sc.pdos);
        g_stub.caps.spec_rev = sc.epr ? 3 : 2;
        const pd::Pdo mix[pd::kMaxPdos] = {
            {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000},      {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000},
            {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000},    {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000},
            {pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000},       {pd::PdoType::Pps, 0, 3300, 21000, 5000, 100000},
            {pd::PdoType::Avs, 0, 15000, 28000, 5000, 140000},     {pd::PdoType::Fixed, 0, 28000, 28000, 5000, 140000},
            {pd::PdoType::Fixed, 0, 36000, 36000, 5000, 180000},   {pd::PdoType::Fixed, 0, 48000, 48000, 5000, 240000},
            {pd::PdoType::Avs, 0, 15000, 48000, 5000, 240000}};
        for (int i = 0; i < sc.pdos; ++i) g_stub.caps.pdos[i] = mix[i];
    }
    g_stub.em = {};
    if (sc.cable) {
        g_stub.em.valid = true;
        g_stub.em.vid = 0x2B1D; g_stub.em.pid = 0x1234;
        g_stub.em.max_current_a = 5; g_stub.em.max_voltage_v = 50; g_stub.em.max_power_w = 250;
        g_stub.em.epr_capable = true;
    }
}

// Every distinct screen of the firmware, as the remote's goto_page() names it.
struct Screen { const char *name; uint8_t page; Menu menu; bool is_menu; };

// Names the screen about to be shown: goto_page() and the button presses draw at once, so the label comes first.
void enter(const char *screen, int rot, const char *scenario) {
    char label[64];
    std::snprintf(label, sizeof label, "%s rot %d %s", screen, rot, scenario);
    trunc::context(label);
}
void draw_for_a_while(Ui &ui, const char *screen, int rot, const char *scenario) {
    enter(screen, rot, scenario);
    for (int i = 0; i < 6; ++i) { g_t += 150; ui.tick(g_t); }
}

void walk_pages(Ui &ui, int rot, const Scenario &sc) {
    constexpr uint8_t pages = static_cast<uint8_t>(Page::Count);
    static const char *const kPageNames[] = {"home", "detail", "charger-test", "lines", "tools", "settings"};
    for (uint8_t p = 0; p < pages; ++p) {
        enter(kPageNames[p], rot, sc.name);
        CHECK_EQ(ui.goto_page(p, g_t), true);
        draw_for_a_while(ui, kPageNames[p], rot, sc.name);
    }
    static const char *const kMenuNames[] = {"trigger", "emarker", "pd-trigger", "cable-r", "black-box", "spoof", "usb-link",
                                             "autorotate", "cards", "share", "connection", "diag"};
    for (uint8_t m = 0; m < static_cast<uint8_t>(Menu::Count); ++m) {
        enter(kMenuNames[m], rot, sc.name);
        ui.goto_page(static_cast<uint8_t>(Page::Tools), g_t);
        if (!ui.push_menu(static_cast<Menu>(m), g_t)) continue;   // a menu that refuses to open has nothing to draw
        draw_for_a_while(ui, kMenuNames[m], rot, sc.name);
    }
    static const char *const kTabNames[] = {"conn-source", "conn-cable", "conn-sink"};
    for (uint8_t tab = 0; tab < 3; ++tab) {
        enter(kTabNames[tab], rot, sc.name);
        if (!ui.goto_page(static_cast<uint8_t>(kConnTabAlias + tab), g_t)) continue;
        draw_for_a_while(ui, kTabNames[tab], rot, sc.name);
        // The tab's own rows: scroll the list to its end so every row has been the focused one.
        for (int i = 0; i < 12; ++i) { ui.on_button(Button::Plus, ButtonEvent::Short, g_t); g_t += 150; ui.tick(g_t); }
        draw_for_a_while(ui, kTabNames[tab], rot, sc.name);
    }
}

// The charger-test wizard: Tests, Custom, every family Group, Cable, Review and its confirmation.
void walk_wizard(Ui &ui, int rot, const Scenario &sc) {
    enter("wizard-tests", rot, sc.name);
    ui.goto_page(static_cast<uint8_t>(Page::Home), g_t);
    ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), g_t);
    auto press = [&](Button b) { g_t += 150; ui.on_button(b, ButtonEvent::Short, g_t); ui.tick(g_t + 50); g_t += 50; };
    auto step_all_rows = [&](const char *name, int rows) {
        draw_for_a_while(ui, name, rot, sc.name);
        for (int i = 0; i < rows; ++i) { press(Button::Plus); draw_for_a_while(ui, name, rot, sc.name); }
        for (int i = 0; i < rows; ++i) press(Button::Minus);
    };
    step_all_rows("wizard-tests", 8);
    // Custom: the families, then each family's Group page (OK opens, BACK leaves).
    for (int i = 0; i < 5; ++i) press(Button::Plus);
    press(Button::Ok);
    step_all_rows("wizard-custom", 10);
    for (int row = 1; row <= 10; ++row) {
        for (int i = 0; i < row; ++i) press(Button::Plus);
        press(Button::Ok);
        draw_for_a_while(ui, "wizard-group", rot, sc.name);
        step_all_rows("wizard-group", 6);
        press(Button::Back);
        for (int i = 0; i < 12; ++i) press(Button::Minus);
    }
    press(Button::Back);
    // Next: Cable, then Review.
    for (int i = 0; i < 12; ++i) press(Button::Minus);
    press(Button::Ok);
    step_all_rows("wizard-cable", 6);
    for (int i = 0; i < 12; ++i) press(Button::Minus);
    press(Button::Ok);
    step_all_rows("wizard-review", 6);
    for (int i = 0; i < 12; ++i) press(Button::Minus);
    press(Button::Ok);   // the first START press opens the confirmation, nothing is touched
    draw_for_a_while(ui, "wizard-confirm", rot, sc.name);
    press(Button::Back);
    draw_for_a_while(ui, "wizard-back", rot, sc.name);
}

void test_walk() {
    for (const Scenario &sc : kScenarios) {
        apply(sc);
        static Ui ui;
        g_t += 1000;
        ui.init(g_t);
        for (int rot = 0; rot < 4; ++rot) {
            enter("rotation change", rot, sc.name);
            ui.set_rotation(static_cast<uint8_t>(rot), g_t);
            g_t += 200;
            walk_pages(ui, rot, sc);
            walk_wizard(ui, rot, sc);
        }
    }
    trunc::context("");
}

// A saved clone's name is user data, up to emlib::kNameMax (24) cells. The shared cable list (the wizard's Cable stage and
// Tools -> Spoof) must show every one of them whole: the dim "saved" tail and the ARMED pill give way to the name, and a
// name this long takes the whole row. fit_data would count a cut, so a hit here means a name lost its last letters.
void test_long_cable_names() {
    apply({"idle", 0, false, false, false});
    static const char *const kNames[] = {"OnePlus C2C mag 100W", "OnePlus C2C magnetic 100W", "Short", "UGREEN-USB4-240W-E-Marker"};
    static_assert(emlib::kNameMax == 24);
    for (unsigned i = 0; i < std::size(kNames); ++i) {
        emlib::Entry e;
        e.seq = static_cast<uint16_t>(i + 1);
        e.rec = emrec::builtin_cable(1);
        std::strncpy(e.name.data(), kNames[i], e.name.size() - 1);
        e.name[std::min(std::strlen(kNames[i]), emlib::kNameMax)] = 0;
        g_stub.lib.push_back(e);
        g_stub.lib_saves++;
    }
    static Ui ui;
    g_t += 1000;
    ui.init(g_t);
    for (int rot = 0; rot < 4; ++rot) {
        ui.set_rotation(static_cast<uint8_t>(rot), g_t);
        auto press = [&](Button b) { g_t += 150; ui.on_button(b, ButtonEvent::Short, g_t); ui.tick(g_t + 50); g_t += 50; };
        const int hits = trunc::data_hits();
        // The wizard: Tests -> OK on the first preset opens the Cable stage; walk its rows.
        trunc::context("long cable names, wizard");
        ui.goto_page(static_cast<uint8_t>(Page::Home), g_t);
        ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), g_t);
        press(Button::Ok);
        CHECK_EQ(ui.charger_test_stage(), 2);   // the Cable stage: a change of the flow must not leave this walk on another list
        for (int i = 0; i < 8; ++i) { draw_for_a_while(ui, "wizard-cable-long", rot, "names"); press(Button::Plus); }
        // Spoof: arm each clone in turn (the amber ARMED pill on the longest name), then leave.
        trunc::context("long cable names, spoof");
        ui.goto_page(static_cast<uint8_t>(Page::Tools), g_t);
        ui.push_menu(Menu::Spoof, g_t);
        for (int i = 0; i < 8; ++i) { press(Button::Plus); draw_for_a_while(ui, "spoof-long", rot, "names"); press(Button::Ok); draw_for_a_while(ui, "spoof-long", rot, "names"); }
        CHECK_EQ(trunc::data_hits(), hits);
        ui.on_button(Button::Back, ButtonEvent::Short, g_t);
        ui.goto_page(static_cast<uint8_t>(Page::Home), g_t);
    }
    trunc::context("");
}

// Tools -> Report cards over real cards: the list, the open card with every row focused in turn, and every row's detail screen.
// (2026-11-08: the FH22 card's "PD fixed" row drew "capped ..." through fit_data, which only counts; the walk had no card.)
// The card text is generated by the firmware, so it goes through fit_text and a cut fails here; only the saved name is data.
void test_card_screens() {
    apply({"idle", 0, false, false, false});
    stub_cards_reset();
    namespace rc = report_card;
    rc::Card a = cardfx::fh22_full();
    rc::Card b = cardfx::fh22_full();          // an old PD failure with a stored reason, a failing legacy group and a failed range
    b.id.fp = 0x22222222; rc::set_name(b, "Long charger");
    b.group[0] = {rc::GV::Fail, 12000, 5, uint8_t(ct::Reason::Timeout), 0};
    b.range[0].fail_dv = 100; b.range[1].max_dv = 0;
    b.group[2] = {rc::GV::Fail, 9000, 0, uint8_t(ct::Reason::Timeout), 12000};
    b.group[3] = {rc::GV::Pass, 12000, 0, 0, 0};
    b.group[unsigned(ct::Group::Scp)] = {rc::GV::Pass, 21000, 0, 0, 0};
    b.src_flags = uint8_t(rc::kSrcOppoSvid | (127 << 1));   // the passive OPPO SVID and "SCP cap 21V 6.35A" fact lines, under the handover row
    rc::Card c = cardfx::fh22_full();          // nothing passed: PD failed at 9 V on the first step
    c.id.fp = 0x33333333; c.name[0] = 0;
    c.group[0] = {rc::GV::Fail, 0, 0, uint8_t(ct::Reason::Timeout), 90};
    c.n_ranges = 0;
    CHECK_EQ(int(rc::hw::put(a)), int(rc::Err::Ok));
    CHECK_EQ(int(rc::hw::put(b)), int(rc::Err::Ok));
    CHECK_EQ(int(rc::hw::put(c)), int(rc::Err::Ok));
    static Ui ui;
    const int data_before = trunc::data_hits();
    for (int rot = 0; rot < 4; ++rot) {
        g_t += 1000;
        ui.init(g_t);
        ui.set_rotation(static_cast<uint8_t>(rot), g_t);
        auto press = [&](Button bt) { g_t += 150; ui.on_button(bt, ButtonEvent::Short, g_t); ui.tick(g_t + 50); g_t += 50; };
        ui.goto_page(static_cast<uint8_t>(Page::Tools), g_t);
        ui.push_menu(Menu::Cards, g_t);
        draw_for_a_while(ui, "cards-list", rot, "cards");
        for (int card = 0; card < 3; ++card) {
            press(Button::Ok);   // the open card
            draw_for_a_while(ui, "card", rot, "cards");
            // Row 0 (PD fixed) is the focused row on opening: its detail comes first. The walk once pressed PLUS before OK and so
            // never drew it (2026-11-08: "fixed levels passed" and "capped below target" were cut there, as data).
            press(Button::Ok);
            draw_for_a_while(ui, "card-pd-detail", rot, "cards");
            press(Button::Back);
            for (int row = 0; row < 16; ++row) {
                press(Button::Plus);
                draw_for_a_while(ui, "card-row", rot, "cards");
                press(Button::Ok);   // the row's detail screen
                draw_for_a_while(ui, "card-row-detail", rot, "cards");
                press(Button::Back);
            }
            for (int row = 0; row < 16; ++row) press(Button::Minus);
            press(Button::Back);
            press(Button::Plus);   // the next card of the list
            draw_for_a_while(ui, "cards-list", rot, "cards");
        }
        ui.on_button(Button::Back, ButtonEvent::Short, g_t);
        ui.goto_page(static_cast<uint8_t>(Page::Home), g_t);
    }
    // Every reason a group can carry, on the PD fixed row (failed with no stored target, skipped) and on a legacy cell.
    {
        g_t += 1000;
        ui.init(g_t);
        ui.set_rotation(0, g_t);
        for (unsigned why = 0; why <= unsigned(ct::Reason::Capped); ++why)
            for (const rc::GV verdict : {rc::GV::Fail, rc::GV::Skip}) {
                for (const uint16_t max_mv : {uint16_t(0), uint16_t(20000)}) {
                    rc::Card d = cardfx::fh22_full();
                    d.id.fp = 0x44444444; d.name[0] = 0; d.n_ranges = 0;
                    d.group[0] = {verdict, max_mv, 0, uint8_t(why), 0};
                    d.group[5] = {verdict, max_mv, 0, uint8_t(why), 0};
                    CHECK_EQ(int(rc::hw::put(d)), int(rc::Err::Ok));
                    ui.goto_page(static_cast<uint8_t>(Page::Tools), g_t);
                    ui.push_menu(Menu::Cards, g_t);
                    for (int i = 0; i < 6; ++i) { g_t += 150; ui.tick(g_t); }
                    g_t += 150; ui.on_button(Button::Ok, ButtonEvent::Short, g_t); ui.tick(g_t + 50); g_t += 50;
                    enter("card-reason", 0, "cards");
                    for (int i = 0; i < 6; ++i) { g_t += 150; ui.tick(g_t); }
                    ui.on_button(Button::Back, ButtonEvent::Short, g_t);
                    ui.on_button(Button::Back, ButtonEvent::Short, g_t);
                }
            }
    }
    // The passive QC / OEM labels over every label kind and a spread of long PPS offers (2026-11-08: "via PPS 5-20V 5A" was cut
    // to 15 characters in the Results grid, and "Samsung SFC 2.0 (via PPS 3.3-21V 6.25A)" overran the card's fact line).
    //   card: the open card is drawn (the fact rows go through put_line, so a cut fails here);
    //   grid: the label row of the Results grid is built the way ChargerTestPage::build_results builds it, then every row label
    //         is checked against the label strip and every line of cells against the strip's width.
    {
        namespace rl = result_layout;
        int labelled = 0;
        g_t += 1000;
        ui.init(g_t);
        ui.set_rotation(0, g_t);
        for (const uint16_t vid : {0, 0x05C6, 0x22B8, 0x17EF})
            for (const uint8_t pdp : {uint8_t(0), uint8_t(100)})
                for (const std::pair<uint16_t, uint16_t> span : {std::pair<uint16_t, uint16_t>{33, 210}, {50, 200}, {33, 110}})
                    for (const uint16_t ma : {uint16_t(3000), uint16_t(5250), uint16_t(6250)}) {
                        rc::Card d = cardfx::fh22_full();
                        d.id.fp = 0x55555555; d.name[0] = 0; d.id.vid = vid; d.id.pdp_w = pdp;
                        d.n_ranges = 1;
                        d.range[0] = rc::Range{uint8_t(ct::Algorithm::Pps), 1, span.first, span.second, span.second, 0, ma};
                        d.cable.max_ma = 5000;
                        d.src_flags = uint8_t(rc::kSrcOppoSvid | (127 << 1));
                        const rc::QcLabel kind = rc::qc_label(d);
                        if (kind == rc::QcLabel::None) continue;
                        ++labelled;
                        char why[48];
                        rc::qc_label_justification(d, why, sizeof why);
                        char scp[24] = {};
                        rc::scp_note(d, scp, sizeof scp);
                        rl::OemNotes oem;
                        oem.headers = false;
                        oem.qc_label = rc::qc_label_name(kind);
                        oem.qc_why = why;
                        oem.vendor = "Lenovo";
                        oem.oppo_svid = true;
                        oem.scp_cap = scp + 8;
                        rl::Layout l;
                        rl::build(l, {}, oem);
                        CHECK_EQ(int(l.cuts), 0);   // no label or word was shortened to fit its field
                        for (size_t r = 0; r < l.row_count; ++r) CHECK_EQ(text_width(kLabelFont, l.rows[r].label) <= kit::kResultLabelW - 8, true);
                        std::array<rl::Line, rl::kMaxLines + 16> lines{};
                        const size_t n = rl::wrap(l, rl::kGridMetrics, lines);
                        CHECK_EQ(n <= rl::kMaxLines, true);
                        for (size_t i = 0; i < n; ++i) {
                            int widest = 0;
                            unsigned cells = 0;
                            for (unsigned k = 0; k < lines[i].count; ++k) {
                                const rl::Token &tk = l.tokens[lines[i].first + k];
                                if (tk.glue) continue;
                                ++cells;
                                widest = std::max(widest, rl::cell_width(tk, rl::kGridMetrics));
                                CHECK_EQ(text_width(kSmall, tk.text) + 2 * rl::kGridMetrics.pad <= rl::cell_width(tk, rl::kGridMetrics), true);
                            }
                            CHECK_EQ(int(cells) * widest + (int(cells) - 1) * rl::kGridMetrics.gap <= rl::kGridMetrics.avail, true);
                        }
                        CHECK_EQ(int(rc::hw::put(d)), int(rc::Err::Ok));
                        ui.goto_page(static_cast<uint8_t>(Page::Tools), g_t);
                        ui.push_menu(Menu::Cards, g_t);
                        for (int i = 0; i < 6; ++i) { g_t += 150; ui.tick(g_t); }
                        g_t += 150; ui.on_button(Button::Ok, ButtonEvent::Short, g_t); ui.tick(g_t + 50); g_t += 50;
                        enter("card-qc-fact", 0, "cards");
                        for (int i = 0; i < 6; ++i) { g_t += 150; ui.tick(g_t); }
                        ui.on_button(Button::Back, ButtonEvent::Short, g_t);
                        ui.on_button(Button::Back, ButtonEvent::Short, g_t);
                    }
        CHECK_EQ(labelled > 20, true);   // the sweep reaches Moto, QC4+, QC5 and Samsung labels
    }
    // The card's own words (every verdict row, every detail screen) are generated copy: none of it may be cut as "external
    // data". Only the saved name of a card is data, and the names above fit the list and the head whole.
    if (trunc::data_hits() != data_before) trunc::print_report(true);
    CHECK_EQ(trunc::data_hits(), data_before);
    stub_cards_reset();
    trunc::context("");
}

// The charger-test words that no page walk can reach in every state: each must fit the line it is drawn on.
void test_reason_words() {
    char buf[kFitBuf];
    trunc::context("reason words");
    for (unsigned r = 0; r <= static_cast<unsigned>(ct::Reason::Capped); ++r) {
        const char *text = reason_text(static_cast<ct::Reason>(r));
        CHECK_EQ(fits_px(kSmallAdvPx, text, kFooterTextPx), true);   // a footer, or a 26-cell wizard row
        if (!fits_px(kSmallAdvPx, text, kFooterTextPx)) std::printf("reason %u too long: \"%s\"\n", r, text);
        (void)buf;
    }
    trunc::context("");
}

// How far a PPS / AVS range got: the Results footer reads "<row label>: <reach_footer>" (27 cells) and the Detail page's value
// column holds 16 cells. Swept over every 0.1 V ceiling of the ranges a source advertises (integer-volt maxima, as the PDOs are).
void test_reach_words() {
    trunc::context("reach words");
    static constexpr ct::Algorithm kRanges[] = {ct::Algorithm::Pps, ct::Algorithm::SprAvs, ct::Algorithm::EprAvs};
    static constexpr uint16_t kLos[] = {3300, 5000, 5900, 10000, 15000};
    static constexpr result_layout::Tone kTones[] = {result_layout::Tone::Pass, result_layout::Tone::Partial, result_layout::Tone::Fail,
                                                     result_layout::Tone::Skip, result_layout::Tone::Pending};
    unsigned too_long = 0, shapes = 0;
    for (const auto a : kRanges) {
        const size_t label = std::strlen(result_layout::display_name(a));
        for (const uint16_t lo : kLos)
            for (unsigned adv = 6000; adv <= 48000; adv += 1000)
                for (unsigned hi = lo; hi <= adv; hi += 100)
                    for (const auto tone : kTones)
                        for (const bool capped : {false, true}) {
                            result_layout::Reach r;
                            r.lo_mv = lo; r.hi_mv = static_cast<uint16_t>(hi); r.adv_mv = static_cast<uint16_t>(adv);
                            r.capped = capped; r.tone = tone;
                            const auto foot = result_layout::reach_footer(r), span = result_layout::reach_span(r);
                            ++shapes;
                            if (label + 2 + foot.len > static_cast<size_t>(kFooterCells) || span.len > 16) {
                                if (!too_long) std::printf("reach too long: %s: \"%s\" / \"%s\"\n", result_layout::display_name(a), foot.c_str(), span.c_str());
                                ++too_long;
                            }
                        }
    }
    CHECK_EQ(too_long, 0u);
    CHECK_EQ(shapes > 1000, true);
    trunc::context("");
}

// The built-in cables (src/pd/emrec.cpp, a table other work edits): the label is a wizard / list row title (the USE pill takes
// 6 cells of the 26), the detail is the Cable stage's footer note.
void test_builtin_cables() {
    for (unsigned id = 0; id < emrec::kBuiltinCount; ++id) {
        const auto &b = emrec::builtin_info(id);
        const std::string label(b.label), detail(b.detail);
        const bool ok_label = fits_px(kSmallAdvPx, label, 208 - 6 * 8), ok_detail = fits_px(kSmallAdvPx, detail, kFooterTextPx);
        CHECK_EQ(ok_label, true);
        CHECK_EQ(ok_detail, true);
        if (!ok_label) std::printf("built-in cable %u label too long: \"%s\"\n", id, label.c_str());
        if (!ok_detail) std::printf("built-in cable %u detail too long: \"%s\"\n", id, detail.c_str());
    }
}

// The Resume stage fields (status, row, note, footer): every wording of every input fits its box.
void test_resume_words() {
    for (int bits = 0; bits < 64; ++bits) {
        resumetext::In in;
        in.self_now = bits & 1; in.gate_ready = bits & 2; in.rd_on = bits & 4;
        in.wait_over = bits & 8; in.behind_cable = bits & 16; in.held = bits & 32;
        CHECK_EQ(int(resumetext::status(in).size() <= resumetext::kStatusMax), 1);
        CHECK_EQ(int(resumetext::row(in).size() <= resumetext::kRowMax), 1);
        CHECK_EQ(int(resumetext::note(in).size() <= resumetext::kNoteMax), 1);
        CHECK_EQ(int(resumetext::footer(in).size() <= static_cast<size_t>(kFooterCells)), 1);
    }
}

// The Tools entry that opens the Connection page is called "Who's attached" (14 cells, the longest Tools name): its menu row and
// the page's title bar must be drawn whole in every rotation (the font has the apostrophe: Spleen and Atkinson cover ASCII 32..126).
void test_whos_attached() {
    CHECK_STR(ToolsPage::tool_name(4), "Who's attached");
    CHECK_EQ(fits_px(kSmallAdvPx, "Who's attached", 130), true);   // the Tools row title box
    apply(kScenarios[0]);
    static Ui ui;
    g_t += 1000;
    ui.init(g_t);
    const int cuts_before = trunc::failures();
    for (int rot = 0; rot < 4; ++rot) {
        ui.set_rotation(static_cast<uint8_t>(rot), g_t);
        ui.goto_page(static_cast<uint8_t>(Page::Tools), g_t);
        for (int row = 0; row < 8; ++row) {   // every Tools row has been the focused one: the long name is drawn selected too
            draw_for_a_while(ui, "whos-attached tools", rot, "idle");
            g_t += 150; ui.on_button(Button::Plus, ButtonEvent::Short, g_t); ui.tick(g_t + 50); g_t += 50;
        }
        CHECK_EQ(ui.push_menu(Menu::Connection, g_t), true);
        draw_for_a_while(ui, "whos-attached title", rot, "idle");
        ui.on_button(Button::Back, ButtonEvent::Short, g_t);
    }
    CHECK_EQ(trunc::failures(), cuts_before);
    trunc::context("");
}

}  // namespace

int main() {
    test_whos_attached();
    test_reason_words();
    test_reach_words();
    test_builtin_cables();
    test_resume_words();
    test_long_cable_names();
    test_card_screens();
    test_walk();
    return test_report("test_ui_no_truncation");
}
