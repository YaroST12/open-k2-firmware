// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_diagnosis.hpp"

#include <algorithm>
#include <string_view>

#include "k2/pd/diag_live.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

namespace {

namespace diag = pd::diag;
namespace screen = pd::diag::screen;
namespace live = pd::diag::live;

constexpr const char kWaitText[] = "Waiting for PD";
constexpr const char kWaitNote[] = "starts at the first PD message";
constexpr const char *kWaitTexts[] = {kWaitText};
constexpr const char *kWaitNotes[] = {kWaitNote};
static_assert(all_fit(kWaitTexts, kSmallAdvPx, kit::kTailX - kit::kTextX), "the waiting row runs into the tail");
static_assert(all_fit(kWaitNotes, kLabelAdv, kit::kNoteMaxPx), "the waiting note is wider than the note box");
// The screen form's budgets (pd::diag::screen, host-tested with worst-case numbers) fit this list's geometry.
static_assert(screen::kLabelChars * kSmallAdvPx + kSmallAdvPx + screen::kTailChars * kSmallAdvPx <=
                  static_cast<unsigned>(kit::kTailX - kit::kTextX),
              "a diagnosis label and its tail do not fit one row");
static_assert(screen::kNoteChars * kLabelAdv <= static_cast<unsigned>(kit::kNoteMaxPx), "a diagnosis note is wider than the note box");
static_assert(screen::kVerdictChars <= static_cast<std::size_t>(kFooterCells), "the diagnosis verdict is wider than the footer");

}  // namespace

kit::Verdict DiagnosisPage::cell_of(diag::Sev s) {
    switch (s) {
    case diag::Sev::Fail: return kit::Verdict::Fail;
    case diag::Sev::Warn: return kit::Verdict::Partial;
    case diag::Sev::Info: return kit::Verdict::None;
    case diag::Sev::Ok: break;
    }
    return kit::Verdict::Pass;
}

int DiagnosisPage::rows() const {
    const unsigned n = screen::row_count(live::report());
    return n ? static_cast<int>(n) : 1;
}

void DiagnosisPage::draw_static() {
    title_bar();
    ctx_.rows.init(ctx_.content, kit::kBodyY, &DiagnosisPage::row_fn, this);
    ctx_.rows.set_count(rows());
    ctx_.rows.select(sel_);
    invalidate();
}

void DiagnosisPage::invalidate() {
    foot_.invalidate();
    ctx_.rows.invalidate();
}

// Row i of the screen form. A finding wears its verdict cell, a healthy line the pass cell; the data-lane row an icon and no
// verdict. The label, tail and note are views into row_, valid until the next row.
void DiagnosisPage::row_fn(void *self, int index, bool focused, kit::RowSpec &out) {
    auto *p = static_cast<DiagnosisPage *>(self);
    out = kit::RowSpec{};
    const diag::Report &r = live::report();
    if (!screen::row(r, static_cast<unsigned>(index), p->row_)) {   // no session yet: the waiting row
        out.marker = kit::Marker::Icon;
        out.icon = Icon::Blackbox;
        out.text = kWaitText;
        out.text_color = kDim;
        if (focused) out.note = kWaitNote;
        return;
    }
    const screen::Row &w = p->row_;
    if (w.lanes) {
        out.marker = kit::Marker::Icon;
        out.icon = Icon::Usb;
    } else {
        out.marker = kit::Marker::Verdict;
        out.state = static_cast<uint8_t>(cell_of(w.sev));
    }
    out.text = std::string_view(w.label.data());
    out.tail = std::string_view(w.tail.data());
    out.tail_color = kDim;
    if (focused) out.note = std::string_view(w.note.data());
}

void DiagnosisPage::update(uint32_t now_ms) {
    const diag::Report &r = live::report();
    title_badge(live::state() == live::State::Last);
    sel_ = std::clamp(sel_, 0, rows() - 1);
    ctx_.rows.set_count(rows());
    ctx_.rows.select(sel_);
    ctx_.rows.draw(now_ms);
    char v[screen::kVerdictChars + 8];
    screen::verdict(r, v);
    draw_footer_cached(ctx_.content, tone_of(r), v, foot_);
}

FooterTone DiagnosisPage::tone_of(const diag::Report &r) {
    const diag::Sev worst = screen::worst(r);
    if (worst == diag::Sev::Fail) return FooterTone::Error;
    if (worst == diag::Sev::Warn) return FooterTone::Caution;
    if (!r.empty && r.link == 0 && r.power == diag::kPowerOk) return FooterTone::Ok;
    return FooterTone::Info;
}

bool DiagnosisPage::on_event(Event e) {
    if (e.press == Press::Short && (e.button == Button::Plus || e.button == Button::Minus)) {
        sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, rows() - 1);
        update(ctx_.now);
        return true;
    }
    return back_event(e);
}

PanelLabels DiagnosisPage::labels() const {
    PanelLabels l = list_labels(base_labels());   // short [+]/[-] move the selection; nothing to open
    l.b[static_cast<int>(Button::Ok)] = {"", "", "", false};
    return l;
}

}  // namespace k2
