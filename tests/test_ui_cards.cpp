// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools > Report cards: the row / card text (pure) and the page through the real Ui over the RAM card store.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "card_fixtures.h"
#include "fake_lcd.h"
#include "k2/chargertest/report_card_hw.hpp"
#include "test_util.h"
#include "k2/ui/ui.hpp"
#include "ui_stubs.h"
#include "k2/ui/ui_style.hpp"

using namespace k2;
namespace rc = report_card;
namespace ct = charger_test;

static rc::Card make_card(uint32_t fp, const char *name, uint16_t vid = 0, uint16_t pid = 0) {
    rc::Card c;
    c.id = {fp, vid, pid};
    c.num_pdos = 5; c.spec_rev = 2; c.boots = 4; c.fw = 0x0FF2C610; c.uptime_s = 300;
    c.group[0] = {rc::GV::Pass, 20000, 0, 0};
    c.group[1] = {rc::GV::Pass, 12000, 0, 0};                                  // QC2
    c.group[2] = {rc::GV::Fail, 0, 0, uint8_t(ct::Reason::Timeout)};           // QC3
    c.group[4] = {rc::GV::Skip, 0, 0, uint8_t(ct::Reason::NoOffer)};           // AFC
    c.n_ranges = 2;
    c.range[0] = {uint8_t(ct::Algorithm::Pps), 0, 33, 210, 200, 130};
    c.range[1] = {uint8_t(ct::Algorithm::SprAvs), 0, 150, 280, 280, 0};
    c.handover = ct::HandoverOutcome::FellBack; c.handover_ds = 32;
    if (name) rc::set_name(c, name);
    return c;
}

static void test_text() {
    StrBuf<24> s;
    rc::Card c = make_card(0x1A2B3C4D, "Anker 735");
    cards_title_text(c, s);
    CHECK_STR(s.view().data(), "Anker 735");
    StrBuf<32> n;
    cards_note_text(c, n);                       // the open list row's note: the full name and the boot counter
    CHECK_STR(n.view().data(), "Anker 735  boot 4");

    c = make_card(0x1A2B3C4D, nullptr, 0x0B05, 0x1234);
    s.clear(); cards_title_text(c, s);
    CHECK_STR(s.view().data(), "0B05:1234");
    c = make_card(0x1A2B3C4D, nullptr);
    s.clear(); cards_title_text(c, s);
    CHECK_STR(s.view().data(), "#1A2B3C4D");
    c = make_card(0x1A2B3C4D, "Baseus CJ-27 Pro");   // 2026-11-02: was cut to "Baseus CJ" and touched the boot column
    c.boots = 27;
    n.clear(); cards_note_text(c, n);
    CHECK_STR(n.view().data(), "Baseus CJ-27 Pro  boot 27");
    c = make_card(0x1A2B3C4D, "A very long name 20ch");
    c.boots = 123456;                            // the counter is clamped; the note stays inside its 30 cells
    n.clear(); cards_note_text(c, n);
    CHECK_EQ(n.view().size() <= 30, 1);
    CHECK_STR(n.view().data(), "A very long name 20c  boot 999");   // names are stored 20 characters long

    c = make_card(0x1A2B3C4D, nullptr, 0x0B05, 0x1234);
    StrBuf<32> t;
    cards_range_text(c.range[0], t);
    CHECK_STR(t.view().data(), "3.3-21V");
    StrBuf<16> m;
    cards_range_status_text(c, c.range[0], m);                  // failed at 13 V after passing up to 20 V
    CHECK_STR(m.view().data(), "at 13V");                       // the verdict cell carries the cross
    m.clear(); cards_range_status_text(c, c.range[1], m);       // the whole offered range passed: the cell says it
    CHECK_EQ(int(m.view().size()), 0);
    m.clear(); cards_range_status_text(c, {uint8_t(ct::Algorithm::SprAvs), 0, 150, 280, 200, 0}, m);   // stopped early, no failure
    CHECK_STR(m.view().data(), "reached 20V");
    m.clear(); cards_range_status_text(c, {uint8_t(ct::Algorithm::Pps), 0, 50, 110, 0, 0}, m);         // nothing measured
    CHECK_EQ(int(m.view().size()), 0);
    m.clear(); cards_range_status_text(c, {uint8_t(ct::Algorithm::SprAvs), 7, 90, 200, 194, 0}, m);    // 20 V step measured 19.4 V: a pass
    CHECK_EQ(int(m.view().size()), 0);

    // FH22 card #18: AVS green and silent, PPS "caps 19.4V" (the source plateaued), the summary in the badge tones.
    const rc::Card fh = cardfx::fh22_full();
    m.clear(); cards_range_status_text(fh, fh.range[0], m);
    CHECK_EQ(int(m.view().size()), 0);
    m.clear(); cards_range_status_text(fh, fh.range[1], m);
    CHECK_STR(m.view().data(), "caps 19.4V");
    StrBuf<12> sm;
    cards_summary_text(fh, sm);
    CHECK_STR(sm.view().data(), "2P 1~");                        // never "2P 9F": the unanswered protocols are not failures
    sm.clear(); cards_summary_text(rc::Card{}, sm);
    CHECK_EQ(int(sm.view().size()), 0);
    sm.clear(); cards_summary_text(c, sm);                      // make_card: PD, AVS pass, PPS fails at 13 V (a pass + fail), QC2 pass, QC3 never answered
    CHECK_STR(sm.view().data(), "3P 1~");

    // The PD row's cause: the reason when it fits 136 px, else "see detail" (the group screen has it in full; nothing is cut).
    StrBuf<24> why;
    cards_pd_why_text({rc::GV::Fail, 0, 0, uint8_t(ct::Reason::NoOffer), 0}, why);
    CHECK_STR(why.view().data(), ct::name(ct::Reason::NoOffer));
    why.clear(); cards_pd_why_text({rc::GV::Skip, 0, 0, uint8_t(ct::Reason::ShutOff), 0}, why);
    CHECK_STR(why.view().data(), "see detail");

    StrBuf<8> am;
    cards_amps_text(3000, am); CHECK_STR(am.view().data(), "3A");
    am.clear(); cards_amps_text(1500, am); CHECK_STR(am.view().data(), "1.5A");
    am.clear(); cards_amps_text(4550, am); CHECK_STR(am.view().data(), "4.55A");
    am.clear(); cards_amps_text(0, am); CHECK_EQ(int(am.view().size()), 0);

    // Fast-charge groups: the cell keeps the passing ceiling and the first failing target.
    StrBuf<16> g;
    cards_group_cell_text({rc::GV::Pass, 12000, 0, 0}, g); CHECK_STR(g.view().data(), "\x01" "12V");
    g.clear(); cards_group_cell_text({rc::GV::Pass, 0, 0, 0}, g); CHECK_STR(g.view().data(), "\x01");
    g.clear(); cards_group_cell_text({rc::GV::Pass, 0, uint8_t(k2::fc::Fail::Detected), 0}, g); CHECK_STR(g.view().data(), "\x01");     // detected: the same check as any pass
    g.clear(); cards_group_cell_text({rc::GV::Pass, 0, uint8_t(k2::fc::Fail::NoVoltage), 0}, g); CHECK_STR(g.view().data(), "\x01");   // handshake ok too
    g.clear(); cards_group_cell_text({rc::GV::Pass, 5000, 0, 0}, g); CHECK_STR(g.view().data(), "\x01" "5V only");
    g.clear(); cards_group_cell_text({rc::GV::Fail, 12000, 8, 11, 20000}, g); CHECK_STR(g.view().data(), "12V\x02" "20V");
    g.clear(); cards_group_cell_text({rc::GV::Fail, 5000, 7, 11, 9000}, g); CHECK_STR(g.view().data(), "5V\x02" "9V");
    g.clear(); cards_group_cell_text({rc::GV::Fail, 12000, 8, 11, 0}, g); CHECK_STR(g.view().data(), "12V\x02");   // old card: no target
    g.clear(); cards_group_cell_text({rc::GV::Fail, 0, 0, 12, 5000}, g); CHECK_STR(g.view().data(), "\x02" "5V");
    g.clear(); cards_group_cell_text({rc::GV::Fail, 0, 0, 12, 0}, g); CHECK_STR(g.view().data(), "\x02");
    g.clear(); cards_group_cell_text({rc::GV::Skip, 0, 0, 2, 0}, g); CHECK_STR(g.view().data(), "skip");
    g.clear(); cards_group_max_text({rc::GV::Fail, 20000, 0, 11, 28000}, g); CHECK_STR(g.view().data(), "max 20V");
    g.clear(); cards_group_fail_text({rc::GV::Fail, 20000, 0, 11, 28000}, g); CHECK_STR(g.view().data(), "\x02" "28V");

    t.clear(); cards_handover_text(c, t);
    CHECK_STR(t.view().data(), "fell back 3.2s");
    c.handover = ct::HandoverOutcome::NotTried; c.handover_ds = 0;
    t.clear(); cards_handover_text(c, t);
    CHECK_STR(t.view().data(), "not tried");
    c.handover = ct::HandoverOutcome::StayedPd; c.handover_ds = 125;
    t.clear(); cards_handover_text(c, t);
    CHECK_STR(t.view().data(), "stayed in PD 12.5s");

    c.runs = 3;
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 3 boot 4 fw 0FF2C610");
    CHECK_EQ(t.view().size() <= static_cast<size_t>(kFooterCells), 1);   // the line is a footer: 27 cells
    c.resumed = 2;   // a run that went through power losses says so
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 3 boot 4 resumed 2");
    c.runs = 99; c.boots = 99999; c.resumed = 16;   // the words shorten before anything is cut
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 99 boot 99999 r16");
    c.runs = 3; c.boots = 4; c.resumed = 0;
    c.cable.flags = rc::kCableSelfPowered;   // a run the charger itself powered says so, in place of the firmware
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 3 boot 4 self-powered");
    c.resumed = 2;   // ...and keeps the resume count in what is left of the line
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 3 boot 4 self-pwr r2");
    c.runs = 99; c.boots = 99999; c.resumed = 16;
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 99 boot 99999 self r16");
    CHECK_EQ(t.view().size() <= static_cast<size_t>(kFooterCells), 1);
    c.runs = 3; c.boots = 4; c.resumed = 0; c.cable.flags = 0;
    c.cable.flags = rc::kCableReattach;   // the planned re-attach says so in place of the firmware, without a resume count
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 3 boot 4 re-attach");
    CHECK_EQ(t.view().size() <= static_cast<size_t>(kFooterCells), 1);
    c.runs = 99; c.boots = 99999;   // the words shorten before anything is cut
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 99 boot 99999 re-attach");
    c.runs = 3; c.boots = 4; c.cable.flags = 0;

    // A card the charger was tested on again with the same result: how often and when it last was, the fullest form that fits.
    c.times = 3; c.last_boots = 9;
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 3 boot 4 x3 last boot 9");
    CHECK_EQ(t.view().size() <= static_cast<size_t>(kFooterCells), 1);
    c.runs = 99; c.boots = 99999; c.times = 99; c.last_boots = 9999; c.resumed = 16;
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 99 boot 99999 x99 b9999");
    CHECK_EQ(t.view().size() <= static_cast<size_t>(kFooterCells), 1);
    c.runs = 255; c.times = 255; c.last_boots = 65535;
    t.clear(); cards_stats_text(c, t);
    CHECK_STR(t.view().data(), "run 255 boot 99999 x255 r16");   // the shortest form leaves room for the resume count
    CHECK_EQ(t.view().size() <= static_cast<size_t>(kFooterCells), 1);
    // The open list row's note: the boot of the latest repeat, inside 30 cells even with the longest name.
    rc::Card d = make_card(0x1A2B3C4D, "Anker 735");
    d.times = 3; d.last_boots = 9;
    n.clear(); cards_note_text(d, n);
    CHECK_STR(n.view().data(), "Anker 735  x3 boot 9");
    d = make_card(0x1A2B3C4D, "A very long name 20ch");   // stored 20 characters long
    d.times = 2; d.last_boots = 5;
    n.clear(); cards_note_text(d, n);
    CHECK_STR(n.view().data(), "A very long name 20c  x2 b5");
    d.times = 255; d.last_boots = 12345;
    n.clear(); cards_note_text(d, n);
    CHECK_EQ(n.view().size() <= 30, 1);
    CHECK_STR(n.view().data(), "A very long name 20c  x255");
    d.times = 1;   // seen once: the note is the old one
    n.clear(); cards_note_text(d, n);
    CHECK_STR(n.view().data(), "A very long name 20c  boot 4");
    c.times = 1; c.last_boots = 0;
    c.runs = 3; c.boots = 4; c.resumed = 0; c.cable.flags = 0;
    t.clear(); cards_id_text(c, t);
    CHECK_STR(t.view().data(), "0B05:1234  fp 1A2B3C4D");
    c.id.pdp_w = 65;   // the charger's rated power follows the VID:PID, and the line still fits
    t.clear(); cards_id_text(c, t);
    CHECK_STR(t.view().data(), "0B05:1234  65W  fp 1A2B3C4D");
    c.id.pdp_w = 255; c.id.vid = c.id.pid = 0;
    t.clear(); cards_id_text(c, t);
    CHECK_STR(t.view().data(), "255W  fp 1A2B3C4D");
    CHECK_EQ(t.view().size() <= 28, 1);
}

static Ui &open_cards(uint32_t t) {
    static Ui ui;
    ui.init(t); ui.tick(t);
    ui.goto_page(uint8_t(Page::Tools), t + 50);
    ui.push_menu(Menu::Cards, t + 100);
    ui.tick(t + 150);
    return ui;
}

static const char *label(Ui &ui, Button b) {
    static char buf[24];
    label_text(buf, ui.labels().b[int(b)], label_max_chars(b));
    return buf;
}

static void test_page() {
    g_stub = StubState();
    stub_cards_reset();
    Ui &ui = open_cards(40000);
    CHECK_EQ(ui.menu() == Menu::Cards, 1);
    CHECK_STR(label(ui, Button::Ok), "");                       // empty: nothing to open
    ui.on_button(Button::Ok, ButtonEvent::Short, 40200);
    ui.on_button(Button::Back, ButtonEvent::Long, 40300);       // nothing to delete
    ui.tick(40350);
    CHECK_EQ(ui.menu() == Menu::Cards, 1);
    ui.on_button(Button::Back, ButtonEvent::Short, 40400);
    CHECK_EQ(int(ui.menu_depth()), 0);

    rc::Card a = make_card(0x11111111, "First"), b = make_card(0x22222222, nullptr, 0x0B05, 0x1234), d = make_card(0x33333333, nullptr);
    CHECK_EQ(int(rc::hw::put(a)), int(rc::Err::Ok));
    CHECK_EQ(int(rc::hw::put(b)), int(rc::Err::Ok));
    CHECK_EQ(int(rc::hw::put(d)), int(rc::Err::Ok));
    ui.push_menu(Menu::Cards, 40500);                           // re-enter: the store is re-read
    ui.tick(40550);
    CHECK_STR(label(ui, Button::Ok), "Open");
    CHECK_STR(label(ui, Button::Back), "Back/Del");
    ui.on_button(Button::Plus, ButtonEvent::Short, 40600);      // newest first: Third, Second, First
    ui.on_button(Button::Plus, ButtonEvent::Short, 40650);
    ui.on_button(Button::Plus, ButtonEvent::Short, 40660);      // clamped at the end
    ui.on_button(Button::Ok, ButtonEvent::Short, 40700);        // open "First"
    ui.tick(40750);
    CHECK_STR(label(ui, Button::Back), "Back/Del");
    CHECK_STR(label(ui, Button::Ok), "Open");                   // rows of the card are selectable
    ui.on_button(Button::Plus, ButtonEvent::Short, 40760);      // focus the first range
    ui.on_button(Button::Ok, ButtonEvent::Short, 40770);        // its detail sub-screen
    ui.tick(40775);
    CHECK_STR(label(ui, Button::Back), "Back");
    ui.on_button(Button::Back, ButtonEvent::Long, 40780);       // no delete from the sub-screen
    ui.tick(40785);
    CHECK_STR(label(ui, Button::Back), "Back");
    ui.on_button(Button::Back, ButtonEvent::Short, 40790);      // back to the card, same row
    ui.tick(40795);
    CHECK_STR(label(ui, Button::Back), "Back/Del");
    CHECK_STR(label(ui, Button::Ok), "Open");
    ui.on_button(Button::Minus, ButtonEvent::Short, 40800);     // focus up to the PD row
    ui.on_button(Button::Back, ButtonEvent::Short, 40850);      // back to the list, still in the tool
    ui.on_button(Button::Minus, ButtonEvent::Short, 40860);     // the list focus moves to "Second", which gets deleted
    CHECK_EQ(ui.menu() == Menu::Cards, 1);
    ui.on_button(Button::Back, ButtonEvent::Long, 40900);       // asks first
    ui.tick(40950);
    CHECK_STR(label(ui, Button::Ok), "Delete");
    CHECK_STR(label(ui, Button::Back), "Back");
    rc::Card keep;
    CHECK_EQ(rc::hw::find(0x22222222, keep), 1);
    ui.on_button(Button::Back, ButtonEvent::Short, 41000);      // cancel keeps it
    CHECK_EQ(ui.menu() == Menu::Cards, 1);
    CHECK_EQ(rc::hw::find(0x22222222, keep), 1);
    ui.on_button(Button::Back, ButtonEvent::Long, 41100);
    ui.on_button(Button::Ok, ButtonEvent::Short, 41200);        // confirm
    ui.tick(41250);
    CHECK_EQ(rc::hw::find(0x22222222, keep), 0);
    CHECK_EQ(rc::hw::find(0x11111111, keep), 1);
    CHECK_EQ(rc::hw::find(0x33333333, keep), 1);
    std::array<rc::Card, 8> all;
    CHECK_EQ(int(rc::hw::list(all)), 2);
    ui.on_button(Button::Back, ButtonEvent::Short, 41300);
    CHECK_EQ(int(ui.menu_depth()), 0);
    stub_cards_reset();
}

static void test_tools_hub_lists_cards() {
    CHECK_EQ(kToolCount, 9);
    CHECK_STR(ToolsPage::tool_name(8), "Device twins");
    CHECK_EQ(ToolsPage::tool_menu(8) == Menu::Twin, 1);
    CHECK_EQ(int(tool_readiness(8, SetupState{}, false).level), int(ToolLevel::Ready));
    CHECK_EQ(std::strlen(tool_hint(8)) <= 24 && std::strlen(tool_hint(8)) > 0, 1);
    CHECK_STR(ToolsPage::tool_name(6), "Report cards");
    CHECK_EQ(ToolsPage::tool_menu(6) == Menu::Cards, 1);
    CHECK_EQ(ToolsPage::tool_menu(2) == Menu::Spoof, 1);
    CHECK_EQ(ToolsPage::tool_menu(7) == Menu::Diag, 1);
    CHECK_EQ(int(tool_readiness(6, SetupState{}, false).level), int(ToolLevel::Ready));
    CHECK_EQ(std::strlen(tool_hint(6)) <= 24 && std::strlen(tool_hint(6)) > 0, 1);
}

// K2_DUMP=dir: the FH22 card (#18, from the golden fixtures) as the user sees it, 1:1 at rotation 3 (buttons at the bottom), into
// dir/k2_cards_<name>.panel: the list, the open card, and the detail screen of its PD fixed row, a legacy row and a range row.
// Not a check: the before/after renders of a layout change (tools/panel2png.py dir out --scale 2).
// The detail sheet's wrap: whole words, at most kSheetLines lines; a text that cannot wrap says so (lines = 0) instead of being cut.
static void test_sheet_wrap() {
    std::string_view ln[3];
    CHECK_EQ(cards_wrap("capped below target", 26, ln, 2), 1u);
    CHECK_EQ(cards_wrap("aaaa bbbb cccc", 9, ln, 2), 2u);
    CHECK_STR(std::string(ln[0]).c_str(), "aaaa bbbb");
    CHECK_STR(std::string(ln[1]).c_str(), "cccc");
    CHECK_EQ(cards_wrap("abcdefghijklmnopqrstuvwxyz0", 26, ln, 2), 0u);              // one word wider than the box
    CHECK_EQ(cards_wrap("aaaaaaaaaa bbbbbbbbbb cccccccccc", 10, ln, 2), 0u);         // three lines where two are allowed
}

// Every group and range sheet over every reason, verdict and engine detail code the firmware can store: each row has its lines
// (nothing was dropped as unwrappable) and the whole sheet stands above the footer (the last rows drop first, never overlap it).
static void test_sheet_never_overflows() {
    constexpr int kSheetRoom = 222 - 60;   // the footer top minus the sheet top
    auto check = [&](const rc::Card &c, bool range, unsigned idx) {
        CardSheet s;
        cards_sheet(c, range, idx, s);
        CHECK_EQ(s.n > 0, true);
        CHECK_EQ(s.height() <= kSheetRoom, true);
        for (unsigned i = 0; i < s.n; ++i) {
            CHECK_EQ(s.row[i].lines > 0 || s.row[i].verdict, true);
            if (s.row[i].lines == 0 && !s.row[i].verdict) std::printf("sheet row %u (%s) has no lines: group/range %u %d\n", i, s.row[i].key, idx, int(range));
        }
    };
    for (unsigned g = 0; g < rc::kGroups; ++g)
        for (const rc::GV verdict : {rc::GV::Fail, rc::GV::Skip, rc::GV::Pass})
            for (unsigned why = 0; why <= unsigned(ct::Reason::Capped); ++why)
                for (const unsigned detail : {0u, 1u, 5u, 40u, 127u, 128u, 140u, 200u, 255u})
                    for (const uint16_t max_mv : {uint16_t(0), uint16_t(20000)}) {
                        rc::Card c = cardfx::fh22_full();
                        c.group[g] = {verdict, max_mv, uint8_t(detail), uint8_t(why), 9000};
                        check(c, false, g);
                    }
    for (const rc::GV verdict : {rc::GV::Fail, rc::GV::Skip, rc::GV::Pass})   // the UFCS group with all four modes behind its cause rows
        for (unsigned why = 0; why <= unsigned(ct::Reason::Capped); ++why)
            for (const unsigned detail : {0u, 5u, 127u, 140u, 255u}) {
                rc::Card c = cardfx::ufcs_modes();
                c.ufcs[3] = {255, 511, 127};
                c.group[unsigned(ct::Group::Ufcs)] = {verdict, 20000, uint8_t(detail), uint8_t(why), 9000};
                check(c, false, unsigned(ct::Group::Ufcs));
            }
    for (unsigned r = 0; r < 2; ++r)
        for (const rc::GV pd : {rc::GV::Fail, rc::GV::Pass})
            for (unsigned why = 0; why <= unsigned(ct::Reason::Capped); ++why) {
                rc::Card c = cardfx::fh22_full();
                c.group[0] = {pd, 12000, 0, uint8_t(why), 9000};
                c.range[r].fail_dv = 100;
                check(c, true, r);
            }
}

// The UFCS group's sheet of a v6 card lists the modes with their current ("Mode 1  3.3-5.5V 5A"), also for a pass; a card without
// modes (before format 6, or a run that never reached UFCS) shows the volts alone. The widest mode text stays in the value box, and a
// sheet that is already full of cause rows drops the last modes instead of overflowing the screen.
static void test_sheet_ufcs_modes() {
    constexpr unsigned kUfcs = unsigned(ct::Group::Ufcs);
#define CHECK(x) CHECK_EQ(bool(x), true)
    const rc::Card c = cardfx::ufcs_modes();
    CHECK_EQ(rc::ufcs_modes(c), 3u);
    CardSheet s;
    cards_sheet(c, false, kUfcs, s);
    std::string rows;
    for (unsigned i = 0; i < s.n; ++i) rows += std::string(s.row[i].key) + "=" + std::string(s.row[i].line[0]) + ";";
    CHECK(rows.find("Mode 1=3.3-5.5V 5A;Mode 2=5-11V 3A;Mode 3=5-20V 3A;") != std::string::npos);
    CHECK(rows.find("Detail code") == std::string::npos);                         // a pass has no cause and no detail code

    rc::Card old = c;                                                              // a v5 card: the modes are unknown
    old.format = 5;
    for (auto &m : old.ufcs) m = {};
    CardSheet o;
    cards_sheet(old, false, kUfcs, o);
    for (unsigned i = 0; i < o.n; ++i) CHECK(std::strncmp(o.row[i].key, "Mode", 4) != 0);

    rc::Card wide = c;                                                             // the widest text each field can hold
    wide.ufcs[0] = {255, 511, 127};
    wide.ufcs[3] = {33, 55, 100};
    CardSheet w;
    cards_sheet(wide, false, kUfcs, w);
    for (unsigned i = 0; i < w.n; ++i) CHECK(w.row[i].lines > 0 || w.row[i].verdict);
    CHECK(rc::ufcs_modes(wide) == 4u);
    CHECK_EQ(w.height() <= 222 - 60, true);

    // The cause rows come before the modes; with all four modes and a failed group the last mode drops rather than overflowing.
    rc::Card bad = wide;
    bad.group[kUfcs] = {rc::GV::Fail, 9000, 127, uint8_t(ct::Reason::Capped), 12000};
    CardSheet b;
    cards_sheet(bad, false, kUfcs, b);
    CHECK_EQ(b.height() <= 222 - 60, true);
    CHECK(b.n <= unsigned(kSheetRows));
    for (unsigned i = 0; i < b.n; ++i) CHECK(b.row[i].lines > 0 || b.row[i].verdict);
#undef CHECK
}

// Source lint for the cards page (2026-11-08: raw gfx_text calls and fit_data on firmware text let cuts through the gate):
// firmware text is drawn only through put_line / put_marked (fit_text, which the gate fails on); gfx_text may be called directly
// only by that helper and for the user's own card name; fit_data / fit_text_mid only on that name.
static void test_cards_source_draws_firmware_text_through_checked_helpers() {
    FILE *f = std::fopen("src/ui/ui_cards.cpp", "rb");
    if (!f) f = std::fopen("../src/ui/ui_cards.cpp", "rb");
    CHECK_EQ(f != nullptr, true);
    if (!f) return;
    char line[512];
    int n = 0;
    while (std::fgets(line, sizeof line, f)) {
        ++n;
        const std::string s(line);
        const bool name = s.find("title_data") != std::string::npos || s.find("sub_data") != std::string::npos ||
                          s.find("title.view()") != std::string::npos || s.find("t.view()") != std::string::npos;
        if (s.find("gfx_text(") != std::string::npos && s.find("fit_text(") == std::string::npos && !name) {
            std::printf("ui_cards.cpp:%d: raw gfx_text on firmware text\n", n);
            CHECK_EQ(0, 1);
        }
        if ((s.find("fit_data(") != std::string::npos || s.find("fit_text_mid(") != std::string::npos) && !name) {
            std::printf("ui_cards.cpp:%d: fit_data / fit_text_mid on firmware text\n", n);
            CHECK_EQ(0, 1);
        }
    }
    std::fclose(f);
}

static void test_dump_renders() {
    const char *dir = std::getenv("K2_DUMP");
    if (!dir) return;
    g_stub = StubState();
    stub_cards_reset();
    rc::Card fh = cardfx::fh22_full();
    CHECK_EQ(int(rc::hw::put(fh)), int(rc::Err::Ok));
    Ui &ui = open_cards(60000);
    ui.set_rotation(3, 60100);
    uint32_t t = 60200;
    const auto shot = [&](const char *name) {
        ui.tick(t += 200); ui.tick(t += 200);
        char path[256];
        std::snprintf(path, sizeof path, "%s/k2_cards_%s.panel", dir, name);
        if (FILE *f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
    };
    const auto press = [&](Button b) { ui.on_button(b, ButtonEvent::Short, t += 150); ui.tick(t += 50); };
    ui.push_menu(Menu::Cards, t);
    shot("list");
    press(Button::Ok);
    shot("card");
    press(Button::Ok);
    shot("pd_detail");
    press(Button::Back);
    press(Button::Plus);   // the AVS range
    press(Button::Plus);   // the PPS range, capped
    press(Button::Ok);
    shot("range_detail");
    press(Button::Back);
    for (int i = 0; i < 2; ++i) press(Button::Plus);   // QC2 (a real skip), then AFC (never answered)
    shot("legacy_row");
    press(Button::Ok);
    shot("legacy_detail");
    // A QC / OEM label too long for one fact line (a 6.25 A, 3.3-21 V PPS): the name, then why, under the handover row.
    stub_cards_reset();
    rc::Card q = cardfx::fh22_full();
    q.id.fp = 0x77777777; q.id.vid = 0; q.name[0] = 0;
    q.n_ranges = 1;
    q.range[0] = rc::Range{uint8_t(ct::Algorithm::Pps), 1, 33, 210, 210, 0, 6250};
    q.cable.max_ma = 5000;
    q.src_flags = uint8_t(rc::kSrcOppoSvid | (127 << 1));
    CHECK_EQ(int(rc::hw::put(q)), int(rc::Err::Ok));
    Ui &ui2 = open_cards(70000);
    ui2.set_rotation(3, 70100);
    ui2.push_menu(Menu::Cards, 70200);
    ui2.on_button(Button::Ok, ButtonEvent::Short, 70400); ui2.tick(70450);
    ui2.tick(70700); ui2.tick(70900);
    if (FILE *f = std::fopen((std::string(dir) + "/k2_cards_qc_fact.panel").c_str(), "wb")) {
        std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f);
        std::fclose(f);
    }
    stub_cards_reset();
}

int main() {
    test_text();
    test_page();
    test_tools_hub_lists_cards();
    test_sheet_wrap();
    test_sheet_never_overflows();
    test_sheet_ufcs_modes();
    test_cards_source_draws_firmware_text_through_checked_helpers();
    test_dump_renders();
    return test_report("test_ui_cards");
}
