// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_cable_list.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string_view>

#include "k2/chargertest/cablepref.hpp"
#include "k2/pd/emk_quirk.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/core/storage.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2 {

namespace {
// How a built-in row reads: the short name, the dim rating column and the note under the focused row. One entry per
// dialog row (ctcable::kDialogIds); the long label of emrec stays the remote name and the spoof record's name.
struct Face {
    std::string_view name, rating, note;
};
constexpr Face kFaces[] = {
    {"EPR", "50V 5A", "Passive EPR cable, 50V 5A"},
    {"CUKTECH", "6A", "50V 5A EPR, unlocks Mi 6A"},
    {"OnePlus", "100W", "OPPO VID, 30V 5A"},
};
static_assert(std::size(kFaces) == ctcable::kDialogBuiltins);

// One library read for a name: the Entry (rec included) lives in this frame only, never in a page callback.
__attribute__((noinline)) bool load_name(uint16_t seq, char (&out)[emlib::kNameMax + 1]) {
    emlib::Entry e;
    if (!storage::lib_get(seq, e)) return false;
    std::memcpy(out, e.name.data(), sizeof out);
    out[sizeof out - 1] = 0;
    return true;
}
}  // namespace

void CableIdentityList::put_label(char (&dst)[emlib::kNameMax + 1], const char* src) {
    dst[0] = 0;
    if (!src) return;
    std::strncpy(dst, src, sizeof dst - 1);
    dst[sizeof dst - 1] = 0;
}

void CableIdentityList::load() {
    persisted_ = cablepref::stored();
    selected_ = {};
    selected_name_[0] = 0;
    if (persisted_.kind == ctcable::Kind::Off) return;
    if (!select(persisted_)) persist();   // the clone is gone: the stale record becomes Off (a later clone may reuse the number)
}

void CableIdentityList::persist() {
    if (selected_ == persisted_) return;
    if (cablepref::set(selected_)) persisted_ = selected_;
}

bool CableIdentityList::sync() {
    if (synced_ && library_gen_ == storage::generation()) return false;
    refresh();
    return true;
}

__attribute__((noinline)) void CableIdentityList::refresh() {
    saved_count_ = static_cast<int>(storage::lib_list(saved_));
    names_ = {};
    if (selected_.kind == ctcable::Kind::Saved) {
        if (index_of(selected_) == kOff) { selected_ = {}; selected_name_[0] = 0; persist(); }   // the clone is gone
        else if (!load_name(selected_.seq, selected_name_)) selected_name_[0] = 0;       // renamed or unreadable
    }
    library_gen_ = storage::generation();
    synced_ = true;
}

ctcable::Choice CableIdentityList::choice_at(int index) const {
    if (index >= kFirstBuiltin && index < kFirstSaved)
        return {ctcable::Kind::Builtin, static_cast<uint16_t>(ctcable::dialog_id(static_cast<unsigned>(index - kFirstBuiltin)))};
    if (index >= kFirstSaved && index - kFirstSaved < saved_count_) return {ctcable::Kind::Saved, saved_[index - kFirstSaved]};
    return {};
}

int CableIdentityList::index_of(ctcable::Choice c) const {
    if (c.kind == ctcable::Kind::Builtin) return kFirstBuiltin + ctcable::dialog_row(c.seq);
    for (int n = 0; c.kind == ctcable::Kind::Saved && n < saved_count_; ++n)
        if (saved_[n] == c.seq) return n + kFirstSaved;
    return kOff;
}

CableIdentityList::Mark CableIdentityList::mark_of(ctcable::Choice c, View v) const {
    const bool is_armed = c.kind != ctcable::Kind::Off && armed() && c == armed_;
    if (v == View::CableStep && armed()) {
        // The pick applies at START: while another identity answers, the pick is NEXT and the answering row CURR;
        // pick and armed on the same row stay ARMED.
        if (is_armed) return c == selected_ ? Mark::Armed : Mark::Curr;
        return c == selected_ ? Mark::Next : Mark::None;
    }
    if (is_armed) return Mark::Armed;
    return c == selected_ ? Mark::Use : Mark::None;
}

CableIdentityList::Row CableIdentityList::row(int index, View v) const {
    Row r;
    const auto c = choice_at(index);
    if (c.kind == ctcable::Kind::Off) {
        r.text.put("OFF");
        r.rating = "no identity";
        r.note = "Nothing answers for the cable";
    } else if (c.kind == ctcable::Kind::Builtin) {
        const Face& f = kFaces[ctcable::dialog_row(c.seq)];
        r.text.put(f.name);
        r.rating = f.rating;
        r.note = f.note;
    } else {
        const auto& slot = names_[index % kNameSlots];
        if (slot.seq == c.seq && slot.name[0]) r.text.put(slot.name);
        else r.text.put("Clone #").put_u32(c.seq);   // not read (yet): the name never loads inside a paint
        r.note = "Saved from the e-marker";
        r.tail = "saved";
    }
    r.mark = mark_of(c, v);
    return r;
}

void CableIdentityList::fill_spec(const Row& r, kit::RowSpec& out) {
    const Style st = style(r.mark);
    const bool on = r.mark != Mark::None;
    out.marker = kit::Marker::Radio;
    out.state = on ? 1 : 0;
    if (on) out.marker_color = kFocus;
    out.text = r.text.view();
    out.text_color = st.color;
    out.data = !r.tail.empty();   // a clone's name comes from the library; the built-in names are literals
    out.col2 = r.rating;
    if (!st.tag.empty() && !r.rating.empty()) {
        // A pill takes the tail's room: the rating column stays only when it fits beside the pill (copy is never cut).
        const int room = kit::kTailX - pill_width(text_cells(st.tag)) - kit::kTailGap - kit::kCol2X;
        if (text_width(kSmall, r.rating) > room) out.col2 = {};
    }
    out.tail_yields = true;     // the name is what the user reads: a long one takes the room of the "saved" word or the pill
    if (!st.tag.empty()) {        // a pill (ARMED / NEXT / CURR) wins over the dim word
        out.tail = st.tag;
        out.tail_color = st.tag_color;
        out.tail_tint = st.tag_tint;
    } else {
        out.tail = r.tail;
        out.tail_color = kDim;
    }
}

void CableIdentityList::fill_names(int first, int n) {
    const int end = std::min(first + n, count());
    for (int index = std::max(first, kFirstSaved); index < end; ++index) {
        auto& slot = names_[index % kNameSlots];
        const uint16_t seq = saved_[index - kFirstSaved];
        if (slot.seq == seq) continue;
        slot.seq = seq;   // an unreadable clone is remembered too (empty name): no read per tick
        if (!load_name(seq, slot.name)) slot.name[0] = 0;
    }
}

int CableIdentityList::window_first(int offset, int selection, int visible, int count) {
    int off = std::clamp(offset, 0, std::max(0, count - visible));
    if (selection < off) off = selection;
    if (selection >= off + visible) off = selection - visible + 1;
    return off;
}

CableIdentityList::Style CableIdentityList::style(Mark m) {
    switch (m) {
    case Mark::Use: return {kText, {}, kDim, kDimT};   // the picked row is the radio, not a pill
    case Mark::Armed: return {kCaution, "ARMED", kCaution, kCautionT};
    case Mark::Next: return {kOk, "NEXT", kOk, kOkT};   // the pick of the next run (the Cable step)
    case Mark::Curr: return {kCaution, "CURR", kCaution, kDimT};   // answering now, but not the pick: dim amber
    case Mark::None: break;
    }
    return {kText, {}, kDim, kDimT};
}

bool CableIdentityList::select(ctcable::Choice c) {
    if (!ctcable::valid(c)) return false;
    c = ctcable::canonical(c);
    char name[emlib::kNameMax + 1] = {};
    if (c.kind == ctcable::Kind::Saved && !load_name(c.seq, name)) return false;
    if (c.kind == ctcable::Kind::Builtin) put_label(name, emrec::builtin_info(c.seq).label.data());   // table literals: NUL terminated
    selected_ = c;
    std::memcpy(selected_name_, name, sizeof name);
    persist();
    return true;
}

__attribute__((noinline)) CableIdentityList::ArmErr CableIdentityList::arm(ctcable::Choice c, ctcable::Facts& facts,
                                                                           char (&name)[emlib::kNameMax + 1]) {
    if (!ctcable::valid(c) || c.kind == ctcable::Kind::Off) return ArmErr::Invalid;
    c = ctcable::canonical(c);
    emlib::Entry e;           // a saved clone's record and name; a built-in fills the same slots
    emk_quirk::Quirks quirks;
    if (c.kind == ctcable::Kind::Builtin) {
        e.rec = emrec::builtin_cable(c.seq);
        quirks = emk_quirk::builtin_quirks(c.seq);
        std::strncpy(e.name.data(), emrec::builtin_info(c.seq).label.data(), e.name.size() - 1);
    } else if (!storage::lib_get(c.seq, e, quirks)) return ArmErr::Gone;
    const auto r = emk_spoof::arm(e.rec, quirks);
    if (r != emk_spoof::Err::Ok) return r == emk_spoof::Err::Inline ? ArmErr::Inline : ArmErr::Invalid;
    facts = ctcable::facts(e.rec);
    put_label(name, e.name.data());
    note_armed(c, name);
    return ArmErr::Ok;
}

void CableIdentityList::note_armed(ctcable::Choice c, const char* name) {
    armed_ = c;
    put_label(armed_name_, name);
    armed_gen_ = emk_spoof::generation();
    armed_set_ = true;
}

void CableIdentityList::disarm() {
    emk_spoof::disarm();
}

bool CableIdentityList::armed() const {
    return armed_set_ && emk_spoof::active() && armed_gen_ == emk_spoof::generation();
}

}  // namespace k2
