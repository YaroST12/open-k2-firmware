// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_spoof.hpp"

#include <algorithm>

#include "k2/pd/emk_spoof.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2 {

namespace {
// Header: S12 keys at x28, S16 values at x112 (13 chars, to x216).
constexpr kit::KvLayout kSpKv{kit::kBodyY, kit::kRowH, kit::kTextX, &kLabelFont, nullptr, 112, 13, Align::Left};
constexpr const char *kSpKeys[SpoofPage::kHeaderRows] = {"State", "Asked/Replied"};
static_assert(all_fit(kSpKeys, kLabelAdv, 112 - kit::kTextX - 4), "a spoof key is wider than its column");
}  // namespace

void spoof_counts_text(uint32_t requests, uint32_t replies, StrBuf<24> &out) {
    out.put_u32(requests).put(" / ").put_u32(replies);
}

void SpoofPage::draw_static() {
    title_bar();
    ctx_.rows.init(ctx_.content, kListY, &SpoofPage::row_fn, this);
    ctx_.rows.set_count(ctx_.cables.count());
    ctx_.rows.select(sel_);
    invalidate();
}

void SpoofPage::invalidate() {
    hdr_.invalidate();
    foot_.invalidate();
    ctx_.rows.invalidate();
}

// The cursor starts on what the list says is in force: the armed row, else the selected one.
void SpoofPage::enter() {
    ctx_.cables.sync();
    sel_ = ctx_.cables.index_of(ctx_.cables.armed() ? ctx_.cables.armed_choice() : ctx_.cables.selected());
    note_ = StrBuf<24>();
    note_until_ = 0;
    invalidate();
}

void SpoofPage::say(const char *text, uint16_t color) {
    note_ = StrBuf<24>();
    note_.put(text);
    note_color_ = color;
    note_until_ = ctx_.now + 2500;
}

// OK on the cursor row. Off selects "no identity" (and turns an armed spoof off); the armed row disarms (the selection
// stays); any other row is selected and armed at once. The Ra mode is the charger test's business, never switched here.
void SpoofPage::activate() {
    CableIdentityList &cl = ctx_.cables;
    const ctcable::Choice c = cl.choice_at(sel_);
    if (c.kind == ctcable::Kind::Off) {
        cl.select({});
        if (emk_spoof::active()) { cl.disarm(); say("Spoof off", kDim); }
        else say("Next run: no identity", kDim);
        return;
    }
    if (cl.armed_choice() == c) { cl.disarm(); say("Spoof off", kDim); return; }
    if (!cl.select(c)) { say("Saved cable gone", kCaution); return; }
    if (ctx_.ui->spoof_busy()) { say("Tool / handover busy", kCaution); return; }
    ctcable::Facts facts{};
    char name[emlib::kNameMax + 1] = {};
    switch (cl.arm(c, facts, name)) {
    case CableIdentityList::ArmErr::Ok: say("Spoof on", kOk); break;
    case CableIdentityList::ArmErr::Gone: say("Saved cable gone", kCaution); break;
    case CableIdentityList::ArmErr::Inline: say("Live sink: unplug load", kCaution); break;
    case CableIdentityList::ArmErr::Invalid: say("Bad saved record", kCaution); break;
    }
}

bool SpoofPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    switch (e.button) {
    case Button::Plus:
    case Button::Minus:
        sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, ctx_.cables.count() - 1);
        update(ctx_.now);
        return true;
    case Button::Ok:
        activate();
        update(ctx_.now);
        return true;
    default:
        return false;
    }
}

PanelLabels SpoofPage::labels() const {
    PanelLabels l = list_labels(base_labels());
    const CableIdentityList &cl = ctx_.cables;
    const ctcable::Choice c = cl.choice_at(sel_);
    const char *ok = "Arm";
    if (c.kind == ctcable::Kind::Off) ok = emk_spoof::active() ? "Disarm" : cl.selected().kind != ctcable::Kind::Off ? "Off" : "";
    else if (cl.armed_choice() == c) ok = "Disarm";
    l.b[static_cast<int>(Button::Ok)] = {ok, "", "", false};
    return l;
}

// One identity per row, the Cable step's look: the radio dot marks the pick, a dim rating column, a dim "saved" tail on a
// clone, the amber ARMED pill on the identity answering now. The names come from the list's cache (filled in update()),
// so nothing is read from flash while painting.
void SpoofPage::row_fn(void *self, int index, bool, kit::RowSpec &out) {
    SpoofPage *p = static_cast<SpoofPage *>(self);
    p->row_ = p->ctx_.cables.row(index);
    CableIdentityList::fill_spec(p->row_, out);
}

// The two key/value rows: what the spoof is doing and how many requests it saw and answered. Repainted only when the
// state or a counter changed.
void SpoofPage::paint_header(bool armed, uint32_t req, uint32_t rep) {
    const Surface &c = ctx_.content;
    StrBuf<24> cnt;
    spoof_counts_text(req, rep, cnt);
    kit::kv_row(c, kSpKv, kit::kv_row_y(kSpKv, 0), kSpKeys[0], armed ? "ARMED" : "OFF", armed ? kCaution : kDim);
    kit::kv_row(c, kSpKv, kit::kv_row_y(kSpKv, 1), kSpKeys[1], cnt.view(), req ? kText : kDim);
}

void SpoofPage::update(uint32_t now_ms) {
    title_badge(false);
    if (note_until_ && static_cast<int32_t>(now_ms - note_until_) >= 0) { note_until_ = 0; note_ = StrBuf<24>(); }
    CableIdentityList &cl = ctx_.cables;
    cl.sync();
    const int count = cl.count();
    sel_ = std::clamp(sel_, 0, count - 1);
    // Names of the rows about to show are read here (one library read each, cached), never while a row paints.
    const int visible = std::max(1, ctx_.rows.visible());
    cl.fill_names(CableIdentityList::window_first(ctx_.rows.state().offset, sel_, visible, count), visible);
    const bool armed = emk_spoof::active();
    const uint32_t req = emk_spoof::requests(), rep = emk_spoof::replies();
    const bool sink = ctx_.ui->link().inline_mode();
    const Surface &c = ctx_.content;
    hdr_.paint(mix_u32(mix_u32(mix_init(), armed ? 1u : 0u), req * 65536u + rep) + 1u, [&] { paint_header(armed, req, rep); });
    // The list repaints only the rows whose text, mark or highlight changed, an idle tick nothing.
    ctx_.rows.set_count(count);
    ctx_.rows.select(sel_);
    ctx_.rows.draw(now_ms, ctx_.budget);
    StrBuf<27> st;
    FooterTone tone = FooterTone::Info;   // neutral by default; Ok only while spoofing is active
    if (note_.view().size()) { tone = footer_tone_of(note_color_); st.put(note_.view()); }
    else if (sink) { tone = FooterTone::Caution; st.put("Live sink: unplug load"); }
    else if (armed) { tone = FooterTone::Ok; st.put(cl.armed() ? "Answering SOP' requests" : "External identity armed"); }
    else { st.put("Plug in charger only"); }
    draw_footer_cached(c, tone, st.view(), foot_);
}

}  // namespace k2
