// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the shared cable-identity list (include/k2/ui/ui_cable_list.hpp): the row model of the wizard's Cable step and of
// Tools -> Spoof, the one selection, the one "armed" marker, and the name cache that keeps library reads out of a paint.
#include "test_util.h"
#include "ui_stubs.h"
#include "k2/ui/ui_cable_list.hpp"
#include "k2/ui/kit.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/core/storage.hpp"
#include <string.h>
#include <string>

using namespace k2;
using ctcable::Choice;
using ctcable::Kind;

static void add_clone(uint16_t seq, const char *name) {
    emlib::Entry e;
    e.seq = seq;
    e.rec = emrec::builtin_cable(1);
    strncpy(e.name.data(), name, e.name.size() - 1);
    g_stub.lib.push_back(e);
    g_stub.lib_saves++;   // the stub's storage::generation()
}
static std::string text(const CableIdentityList &l, int idx) { return std::string(l.row(idx).text.view()); }

static void test_index_space() {
    g_stub = StubState();
    CableIdentityList l;
    l.sync();
    CHECK_EQ(l.count(), 4);   // Off + the three built-in chips (ids 1, 2, 4; id 3 is retired)
    CHECK_EQ(l.choice_at(0).kind == Kind::Off, 1);
    const unsigned ids[] = {1, 2, 4};
    for (int i = 1; i <= 3; i++) {
        CHECK_EQ(l.choice_at(i).kind == Kind::Builtin, 1);
        CHECK_EQ(l.choice_at(i).seq, ids[i - 1]);
        CHECK_EQ(l.index_of(l.choice_at(i)), i);
    }
    CHECK_EQ(l.index_of(Choice{}), 0);
    add_clone(7, "Anker-USB2-60W");
    add_clone(9, "Apple-USB4-240W");
    l.sync();
    CHECK_EQ(l.count(), 6);
    CHECK_EQ(l.choice_at(4).kind == Kind::Saved, 1);
    CHECK_EQ(l.choice_at(4).seq, 7);
    CHECK_EQ(l.choice_at(5).seq, 9);
    CHECK_EQ(l.index_of(Choice{Kind::Saved, 9}), 5);
    CHECK_EQ(l.index_of(Choice{Kind::Saved, 8}), 0);   // not in the library: falls back to the Off row
    CHECK_EQ(l.index_of(Choice{Kind::Builtin, 0}), 1);   // the old spelling of the default chip
    CHECK_EQ(l.index_of(Choice{Kind::Builtin, 3}), 3);   // the retired "OnePlus 100W (mag)" id: the OnePlus 100W row
    CHECK_EQ(l.index_of(Choice{Kind::Saved, 3}), 0);     // a clone's library id 3 is not a built-in id
    CHECK_EQ(l.choice_at(99).kind == Kind::Off, 1);
}

// Built-ins come before every saved clone, and a clone that shares a built-in's VID (a sniffed copy of the same vendor's cable,
// CUKTECH C1 2026-11-07: the built-in CUKTECH 6A unlocks 6 A, the sniffed Kowsi clones do not) never moves ahead of it, so the
// first row of a vendor is the built-in. Stored choices keep their ids: a built-in is its builtin id, a clone its library id.
static uint16_t vid_of(const emrec::Record &r) { return uint16_t(emrec::vdo(r, 1) & 0xFFFF); }
static void test_builtins_lead_a_vendor_that_a_clone_shares() {
    g_stub = StubState();
    add_clone(2, "Kowsi-Mi-6A");   // seq 2 < any later clone: library order must not matter to the built-ins
    add_clone(5, "Kowsi-Nubia-6A");
    CableIdentityList l;
    l.sync();
    const uint16_t vid = vid_of(emrec::builtin_cable(2));   // the CUKTECH 6A chip (builtin id 2)
    CHECK_EQ(vid_of(g_stub.lib[0].rec), vid);                // the clone is of the same vendor
    int first_of_vendor = -1;
    for (int i = 1; i < l.count() && first_of_vendor < 0; i++) {
        const auto c = l.choice_at(i);
        const auto rec = c.kind == Kind::Builtin ? emrec::builtin_cable(c.seq) : g_stub.lib[i - CableIdentityList::kFirstSaved].rec;
        if (vid_of(rec) == vid) first_of_vendor = i;
    }
    CHECK_EQ(l.choice_at(first_of_vendor).kind == Kind::Builtin, 1);
    for (int i = CableIdentityList::kFirstBuiltin; i < CableIdentityList::kFirstSaved; i++) CHECK_EQ(l.choice_at(i).kind == Kind::Builtin, 1);
    for (int i = CableIdentityList::kFirstSaved; i < l.count(); i++) CHECK_EQ(l.choice_at(i).kind == Kind::Saved, 1);
    // Saving or removing clones never renumbers a built-in's row, and a clone is found by its library id.
    const int before = l.index_of(Choice{Kind::Builtin, 3});
    CHECK_EQ(before, 3);
    g_stub.lib.erase(g_stub.lib.begin());
    g_stub.lib_removes++;
    l.sync();
    CHECK_EQ(l.index_of(Choice{Kind::Builtin, 3}), before);
    CHECK_EQ(l.index_of(Choice{Kind::Saved, 5}), CableIdentityList::kFirstSaved);
    CHECK_EQ(l.index_of(Choice{Kind::Saved, 2}), 0);   // gone: Off's row, not a neighbour's
}

// The store keeps names up to emlib::kNameMax (24) characters; the row's text buffer must hold every one (a 23-byte buffer cut the
// last letter before the kit ever saw the name), and the kit must be told to draw it whole.
static void test_longest_name_is_held_whole() {
    g_stub = StubState();
    const char *const longest = "Anker 240W USB4 e-marker";   // 24 characters
    CHECK_EQ(int(std::strlen(longest)), int(emlib::kNameMax));
    add_clone(3, longest);
    CableIdentityList l;
    l.sync();
    l.fill_names(0, 7);
    CHECK_STR(text(l, 4).c_str(), longest);
}

static void test_row_text_and_names() {
    g_stub = StubState();
    add_clone(3, "Anker-USB2-60W");
    CableIdentityList l;
    l.sync();
    CHECK_STR(text(l, 0).c_str(), "OFF");
    CHECK_STR(std::string(l.row(0).rating).c_str(), "no identity");
    CHECK_STR(text(l, 1).c_str(), "EPR");
    CHECK_STR(std::string(l.row(1).rating).c_str(), "50V 5A");
    CHECK_STR(text(l, 2).c_str(), "CUKTECH");
    CHECK_STR(std::string(l.row(2).rating).c_str(), "6A");
    CHECK_STR(text(l, 3).c_str(), "OnePlus");
    CHECK_STR(std::string(l.row(3).rating).c_str(), "100W");
    for (int i = 0; i < 4; i++) CHECK_EQ(l.row(i).note.empty() ? 1 : 0, 0);   // every built-in says what it is for
    CHECK_EQ(l.row(4).rating.empty() ? 1 : 0, 1);                              // a clone has no rating column
    CHECK_STR(std::string(l.row(4).tail).c_str(), "saved");                    // ... but a dim tail that tells it from a built-in
    CHECK_EQ(l.row(2).tail.empty() ? 1 : 0, 1);
    kit::RowSpec spec;
    const auto clone_row = l.row(4);
    CableIdentityList::fill_spec(clone_row, spec);
    CHECK_EQ(spec.tail_yields ? 1 : 0, 1);                                     // the "saved" word gives way to a long name (kit draws it)
    CHECK_STR(text(l, 4).c_str(), "Clone #3");   // not read yet: a paint never loads a name
    const int reads = int(g_stub.lib.size());
    (void)reads;
    l.fill_names(0, 7);
    CHECK_STR(text(l, 4).c_str(), "Anker-USB2-60W");
    // The names stay across a sync that sees no change, and are dropped when the library changed.
    CHECK_EQ(l.sync() ? 1 : 0, 0);
    CHECK_STR(text(l, 4).c_str(), "Anker-USB2-60W");
    g_stub.lib[0].name[0] = 'B';
    g_stub.lib_saves++;
    CHECK_EQ(l.sync() ? 1 : 0, 1);
    CHECK_STR(text(l, 4).c_str(), "Clone #3");
    l.fill_names(0, 7);
    CHECK_STR(text(l, 4).c_str(), "Bnker-USB2-60W");
}

static void test_selection() {
    g_stub = StubState();
    add_clone(3, "Anker-USB2-60W");
    CableIdentityList l;
    l.sync();
    CHECK_EQ(l.selected().kind == Kind::Off, 1);
    CHECK_EQ(int(l.row(0).mark), int(CableIdentityList::Mark::Use));   // Off is the use until another one is picked
    CHECK_EQ(l.select(Choice{Kind::Builtin, 2}) ? 1 : 0, 1);
    CHECK_EQ(int(l.row(2).mark), int(CableIdentityList::Mark::Use));
    CHECK_EQ(int(l.row(0).mark), int(CableIdentityList::Mark::None));
    CHECK_STR(l.selected_name(), emrec::builtin_info(2).label.data());
    CHECK_EQ(l.select(Choice{Kind::Builtin, 0}) ? 1 : 0, 1);   // canonical: seq 0 is the default chip
    CHECK_EQ(l.selected().seq, ctcable::kBuiltinKind);
    CHECK_EQ(l.select(Choice{Kind::Builtin, 3}) ? 1 : 0, 1);   // canonical: the retired id 3 is OnePlus 100W (4), also when armed
    CHECK_EQ(l.selected().seq, 4);
    CHECK_STR(l.selected_name(), "OnePlus 100W");
    ctcable::Facts retired{};
    char retired_name[emlib::kNameMax + 1] = {};
    CHECK_EQ(int(l.arm(Choice{Kind::Builtin, 3}, retired, retired_name)), int(CableIdentityList::ArmErr::Ok));
    CHECK_EQ(l.armed_choice().seq, 4);
    CHECK_EQ(retired.pid, 0x6116);
    emk_spoof::disarm();
    CHECK_EQ(l.select(Choice{Kind::Saved, 3}) ? 1 : 0, 1);
    CHECK_STR(l.selected_name(), "Anker-USB2-60W");
    CHECK_EQ(int(l.row(4).mark), int(CableIdentityList::Mark::Use));
    CHECK_EQ(l.select(Choice{Kind::Saved, 4}) ? 1 : 0, 0);    // not in the library: refused, the choice stays
    CHECK_EQ(l.selected().seq, 3);
    CHECK_EQ(l.select(Choice{Kind::Off, 5}) ? 1 : 0, 0);      // malformed
    // The chosen clone is removed from the library: the selection falls back to Off.
    g_stub.lib.clear();
    g_stub.lib_removes++;
    l.sync();
    CHECK_EQ(l.selected().kind == Kind::Off, 1);
    CHECK_STR(l.selected_name(), "");
    CHECK_EQ(l.select(Choice{}) ? 1 : 0, 1);
    CHECK_EQ(l.selected().kind == Kind::Off, 1);
}

static void test_armed_marker() {
    g_stub = StubState();
    add_clone(3, "Anker-USB2-60W");
    CableIdentityList l;
    l.sync();
    CHECK_EQ(l.armed() ? 1 : 0, 0);
    ctcable::Facts f;
    char name[emlib::kNameMax + 1] = {};
    CHECK_EQ(int(l.arm(Choice{Kind::Builtin, 4}, f, name)), int(CableIdentityList::ArmErr::Ok));
    CHECK_EQ(g_stub.spoof_on ? 1 : 0, 1);
    CHECK_STR(name, emrec::builtin_info(4).label.data());
    CHECK_EQ(l.armed() ? 1 : 0, 1);
    CHECK_EQ(int(l.row(3).mark), int(CableIdentityList::Mark::Armed));
    CHECK_STR(l.armed_name(), emrec::builtin_info(4).label.data());
    // Arming does not move the selection (arming is explicit, choosing is not): both marks on one row show Armed.
    CHECK_EQ(l.select(Choice{Kind::Builtin, 4}) ? 1 : 0, 1);
    CHECK_EQ(int(l.row(3).mark), int(CableIdentityList::Mark::Armed));
    CHECK_EQ(l.select(Choice{Kind::Builtin, 1}) ? 1 : 0, 1);
    CHECK_EQ(int(l.row(3).mark), int(CableIdentityList::Mark::Armed));
    CHECK_EQ(int(l.row(1).mark), int(CableIdentityList::Mark::Use));
    // Disarmed anywhere (a run ended, Tools, the remote): the marker goes with the generation, no one resets it.
    emk_spoof::disarm();
    CHECK_EQ(l.armed() ? 1 : 0, 0);
    CHECK_EQ(int(l.row(3).mark), int(CableIdentityList::Mark::None));
    CHECK_EQ(int(l.arm(Choice{Kind::Saved, 3}, f, name)), int(CableIdentityList::ArmErr::Ok));
    CHECK_STR(name, "Anker-USB2-60W");
    CHECK_EQ(int(l.row(4).mark), int(CableIdentityList::Mark::Armed));
    // Armed by someone else (the remote, a re-arm): not this list's arm, so no marker.
    emk_spoof::disarm();
    emk_spoof::arm(emrec::builtin_cable(1), {});
    CHECK_EQ(g_stub.spoof_on ? 1 : 0, 1);
    CHECK_EQ(l.armed() ? 1 : 0, 0);
    CHECK_EQ(int(l.row(4).mark), int(CableIdentityList::Mark::None));
    emk_spoof::disarm();
}

static void test_arm_errors() {
    g_stub = StubState();
    CableIdentityList l;
    l.sync();
    ctcable::Facts f;
    char name[emlib::kNameMax + 1] = {};
    CHECK_EQ(int(l.arm(Choice{}, f, name)), int(CableIdentityList::ArmErr::Invalid));   // Off arms nothing
    CHECK_EQ(int(l.arm(Choice{Kind::Saved, 9}, f, name)), int(CableIdentityList::ArmErr::Gone));
    g_stub.inl = true;
    CHECK_EQ(int(l.arm(Choice{Kind::Builtin, 1}, f, name)), int(CableIdentityList::ArmErr::Inline));
    CHECK_EQ(l.armed() ? 1 : 0, 0);
    g_stub.inl = false;
    CHECK_EQ(int(l.arm(Choice{Kind::Builtin, 1}, f, name)), int(CableIdentityList::ArmErr::Ok));
    // The facts are the cable VDO's claims (the report card records them).
    CHECK_EQ(int(f.max_ma), 5000);
    emk_spoof::disarm();
}

static void test_window() {
    CHECK_EQ(CableIdentityList::window_first(0, 0, 5, 20), 0);
    CHECK_EQ(CableIdentityList::window_first(0, 7, 5, 20), 3);    // the selection scrolls into view
    CHECK_EQ(CableIdentityList::window_first(10, 12, 5, 20), 10);  // inside: the window stays
    CHECK_EQ(CableIdentityList::window_first(10, 4, 5, 20), 4);
    CHECK_EQ(CableIdentityList::window_first(30, 19, 5, 20), 15);  // clamped to the end
    CHECK_EQ(CableIdentityList::window_first(0, 0, 5, 3), 0);
}

static void test_pill_style() {
    const auto none = CableIdentityList::style(CableIdentityList::Mark::None);
    CHECK_EQ(int(none.tag.size()), 0);
    const auto use = CableIdentityList::style(CableIdentityList::Mark::Use);
    CHECK_EQ(int(use.tag.size()), 0);   // the picked row is the radio; only ARMED is a pill
    const auto armed = CableIdentityList::style(CableIdentityList::Mark::Armed);
    CHECK_STR(std::string(armed.tag).c_str(), "ARMED");
    CHECK_EQ(armed.tag_color != use.tag_color ? 1 : 0, 1);   // two states, two colours
}

static void test_fill_spec() {
    CableIdentityList::Row r;
    r.text.put("CUKTECH 6A");
    r.tail = "saved";
    r.mark = CableIdentityList::Mark::Use;
    kit::RowSpec sp;
    CableIdentityList::fill_spec(r, sp);
    CHECK_EQ(sp.state, 1);                                 // the radio alone marks the pick
    CHECK_STR(std::string(sp.tail).c_str(), "saved");      // dim word, not a pill
    CHECK_EQ(sp.tail_tint, 0);
    CHECK_EQ(sp.data ? 1 : 0, 1);                          // a clone's name is library data
    r.mark = CableIdentityList::Mark::Armed;
    CableIdentityList::fill_spec(r, sp);
    CHECK_STR(std::string(sp.tail).c_str(), "ARMED");      // the amber pill wins
    CHECK_EQ(sp.tail_tint != 0 ? 1 : 0, 1);
}

int main() {
    test_fill_spec();
    test_index_space();
    test_builtins_lead_a_vendor_that_a_clone_shares();
    test_row_text_and_names();
    test_longest_name_is_held_whole();
    test_selection();
    test_armed_marker();
    test_arm_errors();
    test_window();
    test_pill_style();
    return test_report("test_ui_cable_list");
}
