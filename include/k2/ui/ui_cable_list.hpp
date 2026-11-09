// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The one cable-identity list: what the charger wizard's Cable step and Tools -> Spoof both show and both change.
//
// One index space, shared by both screens: 0 Off, 1..kBuiltins the built-in chips (their emrec ids), then every saved
// clone of the e-marker library in library order. One row model: the same entries, the same short names, ratings and
// notes (the kit's radio / name / dim rating column / note under the focused row), the same pills. The pill depends on
// the View the row is built for: Tools -> Spoof arms and disarms on OK, so there the answering row alone wears ARMED;
// the wizard's Cable step applies the pick only at START, so there the pick wears NEXT and the answering row CURR while
// they differ (the shared row keeps ARMED). One selection: the identity the next charger-test START presents. One armed
// marker: "what this list armed is still what the spoof is answering with" (the arm's generation is still emk_spoof's),
// so a disarm or a re-arm anywhere else (a run ending, the remote, Tools) clears it with nobody resetting a flag.
//
// Persistence: the selection is remembered across a power cycle (k2/chargertest/cablepref.hpp, /cable.cfg, written only when it changes,
// read back by load() at Ui init), so a self-powered boot can arm it before the charger's first attach. The armed note is RAM
// only (an arm never outlives the boot). Arming stays explicit: Tools -> Spoof arms on OK, the wizard arms at START; picking
// only selects.
//
// The library reads happen in sync() / select() / fill_names() / arm() (called from a page's update() or an event), never
// while painting: row() reads the name cache only.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/chargertest/ct_cable.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2 {

namespace kit { struct RowSpec; }

class CableIdentityList {
public:
    static constexpr int kOff = 0;
    static constexpr int kBuiltins = static_cast<int>(ctcable::kDialogBuiltins);
    static constexpr int kFirstBuiltin = 1;
    static constexpr int kFirstSaved = kFirstBuiltin + kBuiltins;
    static constexpr int kNameSlots = 8;   // more than any page shows at once (the wizard's 7 rows)

    // Once at Ui init, before anything reads selected(): the remembered selection comes back. A clone that is gone (or no
    // record, or a damaged one) is Off; a stale record naming a gone clone is replaced by Off.
    void load();

    // The library changed (or this is the first call): the saved list is read again and the name cache starts empty. A chosen
    // clone that is gone falls back to Off. True when anything was re-read.
    bool sync();
    int count() const { return kFirstSaved + saved_count_; }
    ctcable::Choice choice_at(int index) const;   // Off for index 0 and for anything past the end
    int index_of(ctcable::Choice) const;          // the row of a choice; Off's row for one not in the list

    // The two screens of the one list; a row is built for one of them (the default is the Spoof screen).
    enum class View : uint8_t { Spoof, CableStep };
    enum class Mark : uint8_t { None, Use, Armed, Next, Curr };   // Armed wins when one row is both the pick and the armed one
    struct Row {
        StrBuf<emlib::kNameMax + 1> text;   // the short name: "OFF", "EPR", "CUKTECH", "OnePlus", or a clone's name (24 cells at most)
        std::string_view rating;  // dim column ("50V 5A", "no identity"); empty for a clone
        std::string_view note;    // what the identity is for (literal), shown under the focused row
        std::string_view tail;    // dim word at the right end: "saved" for a clone, so a clone named like a built-in reads apart
        Mark mark = Mark::None;
    };
    // The kit row both screens show for the row `r` (radio, name, rating column, the dim "saved" tail or the mark's
    // pill: ARMED / NEXT / CURR). The pick alone is the radio: no word. The note (focused row only) is left to the
    // caller. `out` points into `r` (the name), so `r` must outlive the paint of the row.
    static void fill_spec(const Row& r, kit::RowSpec& out);
    Row row(int index, View v = View::Spoof) const;   // cached names only: an unread clone says "Clone #n"
    // The names of the rows [first, first + n) that are not cached yet, one library read each (never from a paint).
    void fill_names(int first, int n);
    // First visible row of a `visible`-row window currently at `offset`, moved only as far as the selection needs.
    static int window_first(int offset, int selection, int visible, int count);

    // What a row's mark looks like (both screens paint with this): the text colour of the row and its right-end pill.
    struct Style {
        uint16_t color;
        std::string_view tag;
        uint16_t tag_color, tag_tint;
    };
    static Style style(Mark);

    // The one selection (what the next START presents). False = refused (malformed, or a clone that is not in the library);
    // the selection is unchanged then.
    bool select(ctcable::Choice);
    ctcable::Choice selected() const { return selected_; }
    const char* selected_name() const { return selected_name_; }

    // Arm a choice as the spoof identity: the record and quirks go to emk_spoof. On success `facts` / `name` describe it and the
    // armed marker is set. Off is Invalid (disarm() is the way back). The Ra mode and the tool / 5 V handover check stay with
    // the caller.
    enum class ArmErr : uint8_t { Ok, Gone, Invalid, Inline };
    ArmErr arm(ctcable::Choice, ctcable::Facts& facts, char (&name)[emlib::kNameMax + 1]);
    void disarm();   // emk_spoof::disarm(); the marker follows the generation
    bool armed() const;
    ctcable::Choice armed_choice() const { return armed() ? armed_ : ctcable::Choice{}; }
    const char* armed_name() const { return armed() ? armed_name_ : ""; }
    // Note an arm made elsewhere with the same record and the generation it produced (the wizard's resume path), so the
    // marker shows it.
    void note_armed(ctcable::Choice, const char* name);

private:
    static void put_label(char (&dst)[emlib::kNameMax + 1], const char* src);
    void refresh();
    void persist();   // writes selected_ when it is not what the record holds (never per tick; a refused write is retried later)
    Mark mark_of(ctcable::Choice, View) const;

    std::array<uint16_t, emlib::kCapacity> saved_{};
    int saved_count_ = 0;
    struct NameSlot {
        uint16_t seq = 0;
        char name[emlib::kNameMax + 1] = {};
    };
    std::array<NameSlot, kNameSlots> names_{};   // slot = row index % kNameSlots, valid while its seq is the row's clone
    uint32_t library_gen_ = 0;
    bool synced_ = false;
    ctcable::Choice selected_{};
    ctcable::Choice persisted_{};   // what /cable.cfg holds (Off on a blank volume), as far as this boot knows
    char selected_name_[emlib::kNameMax + 1] = {};
    ctcable::Choice armed_{};
    char armed_name_[emlib::kNameMax + 1] = {};
    uint32_t armed_gen_ = 0;
    bool armed_set_ = false;
};

}  // namespace k2
