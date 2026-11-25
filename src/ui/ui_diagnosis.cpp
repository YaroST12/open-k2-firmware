// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_diagnosis.hpp"
#include "k2/core/strpack.hpp"

#include <algorithm>
#include <string_view>

#include "k2/pd/diag_live.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

namespace {

namespace diag = pd::diag;
namespace screen = pd::diag::screen;
namespace live = pd::diag::live;

constexpr const char *kWaitText = K2_P("Waiting for PD");
constexpr const char *kWaitNote = K2_P("starts on first PD message");
constexpr const char *kWaitTexts[] = {kWaitText};
constexpr const char *kWaitNotes[] = {kWaitNote};
static_assert(all_fit_p(kWaitTexts, kSmallAdvPx, kit::kTailX - kit::kTextX), "the waiting row runs into the tail");
static_assert(all_fit_p(kWaitNotes, kLabelAdv, kit::kNoteMaxPx), "the waiting note is wider than the note box");
// The screen form's budgets (pd::diag::screen, host-tested with worst-case numbers) fit this list's geometry (208 px canvas).
// The tail is a hint (tail_yields): a long label keeps the row, the time gives way.
static_assert(screen::kLabelChars * kSmallAdvPx <= static_cast<unsigned>(kit::kTailX - kit::kTextX), "a diagnosis label runs into the tail");
static_assert(screen::kNoteChars * kLabelAdv <= static_cast<unsigned>(kit::kNoteMaxPx), "a diagnosis note is wider than the note box");
static_assert(screen::kVerdictChars * kLabelAdv + 16 <= static_cast<unsigned>(kTwinContentW - 8), "the diagnosis verdict is wider than the toast pill");
static_assert(DiagnosisPage::kStampWordX + static_cast<int>(screen::kLabelChars) * kSmallAdvPx <= kTwinContentW - 2, "the stamp's word runs off the page");
static_assert(DiagnosisPage::kStampWordX + static_cast<int>(screen::kNoteChars * kLabelAdv) <= kTwinContentW - 2, "the stamp's sub-line runs off the page");
static_assert(DiagnosisPage::kStampSubY + 12 <= DiagnosisPage::kStampH, "the stamp's sub-line leaves the stamp");

constexpr const char *kBlackBox = K2_P("Black Box");
// The Log place's list: the findings from under the stamp to the list bottom.
constexpr int16_t kPlaceListY = kit::kBelowBar + DiagnosisPage::kStampH + 2;

}  // namespace

void DiagnosisPage::as_place() {
    place_ = true;
    title_ = rail::kLog;
    icon_ = place_icon(bench::Place::Log);
    bar_icon_ = true;
}

kit::Verdict DiagnosisPage::stamp_of(const diag::Report &r) {
    switch (screen::worst(r)) {
    case diag::Sev::Fail: return kit::Verdict::Fail;
    case diag::Sev::Warn: return kit::Verdict::Partial;
    case diag::Sev::Info:
    case diag::Sev::Ok: break;
    }
    return tone_of(r) == FooterTone::Ok ? kit::Verdict::Pass : kit::Verdict::None;
}

// The stamp: the band in the verdict's fill around the glyph cell and the word, each pixel once; repainted on a change.
// The Log place keeps its row as every place does (ctx_.keep.place_sel), so a rebuilt page lands where it was left.
void DiagnosisPage::enter() {
    const int keep = ctx_.keep.place_sel[static_cast<int>(bench::Place::Log)];
    sel_ = place_ ? std::clamp(keep, 0, std::max(0, list_rows() - 1)) : 0;
}

void DiagnosisPage::leave() {
    if (place_) ctx_.keep.place_sel[static_cast<int>(bench::Place::Log)] = static_cast<uint8_t>(sel_);
}

void DiagnosisPage::paint_stamp() {
    const diag::Report &r = live::report();
    char w[screen::kLabelChars + 8] = {};
    diag::Sev sev = diag::Sev::Ok;
    screen::word(r, w, sev);
    const std::string_view word(w);
    const kit::Verdict v = stamp_of(r);
    // the top finding's evidence, when the word names it (the list leaves that finding out): the sub-line
    screen::Row top{};
    if (skip()) screen::row(r, 0, top);
    const std::string_view sub(top.note.data());
    stamp_.paint_budgeted(mix_u32(mix(mix(word), sub), static_cast<uint32_t>(v)),
                          ctx_.budget, static_cast<int>(ctx_.content.w) * (kStampH + 2), [&] {
        const Surface &c = ctx_.content;
        const kit::VerdictStyle st = kit::verdict_style(v);
        const uint16_t fill = st.filled ? st.fill : kSurface, ink = st.filled ? st.ink : kText;
        const int16_t y = kit::kBelowBar;
        constexpr int16_t kCellX = 4, kCell = 20, kCellDy = (kStampH - kCell) / 2;
        const kit::Box cell{kCellX, static_cast<int16_t>(y + kCellDy), kCell, kCell};
        // The word is font 24 when it fits the 208 px page, else font 16 (the longest labels, 15-16 characters).
        const bool big = static_cast<int>(word.size()) * 12 <= c.w - kStampWordX - 2;
        const Font &wf = big ? kMid : kSmall;
        const int16_t word_dy = sub.empty() ? (kStampH - wf.height) / 2 : big ? kStampWordDy : kStampWordDy + 4;
        const kit::TextRun runs[2] = {{kStampWordX, static_cast<int16_t>(y + word_dy), &wf, word, ink},
                                      {kStampWordX, static_cast<int16_t>(y + kStampSubY), &kLabelFont, sub, ink}};
        // the ground left of the word's run: the band beside and around the glyph cell
        kit::fill_except(c, {0, y, kStampWordX, kStampH}, &cell, 1, fill);
        kit::draw_verdict(c, cell.x, cell.y, cell.w, cell.h, v, "", fill);
        kit::paint_texts(c, {kStampWordX, y, static_cast<int16_t>(c.w - kStampWordX), kStampH}, runs, sub.empty() ? 1 : 2, fill);
        gfx_fill(c, 0, static_cast<int16_t>(y + kStampH), c.w, 2, kBg);   // the gap above the list
    });
}

kit::Verdict DiagnosisPage::cell_of(diag::Sev s) {
    switch (s) {
    case diag::Sev::Fail: return kit::Verdict::Fail;
    case diag::Sev::Warn: return kit::Verdict::Partial;
    case diag::Sev::Info: return kit::Verdict::None;
    case diag::Sev::Ok: break;
    }
    return kit::Verdict::Pass;
}

unsigned DiagnosisPage::skip() const { return place_ && screen::names_top_finding(live::report()) ? 1u : 0u; }

int DiagnosisPage::rows() const {
    const unsigned n = screen::row_count(live::report()) - skip();
    return n ? static_cast<int>(n) : 1;
}

void DiagnosisPage::draw_static() {
    PaintBudget *const bg = ctx_.budget;
    if (static_step_ == 0) {
        if (bg && !bg->admit(140 * kBarH, true)) return;
        title_bar();
        ctx_.rows.init(ctx_.content, place_ ? kPlaceListY : kit::kBodyY, &DiagnosisPage::row_fn, this);
        ctx_.rows.set_count(list_rows());
        ctx_.rows.select(sel_);
        foot_.invalidate();
        stamp_.invalidate();
        ctx_.rows.invalidate();
        if (!ctx_.keep_status) toast_sig_ = 0;
        pill_ = false;
        static_step_ = 1;
    }
    if (static_step_ == 1) {
        if (place_) {
            const int16_t end = kit::list_end(kPlaceListY);
            if (end < kit::kListBottom) {
                const int cost = static_cast<int>(ctx_.content.w) * (kit::kListBottom - end);
                if (bg && !bg->admit(cost, true)) return;
                kit::fill_list_gap(ctx_.content, kPlaceListY);
            }
        }
        static_step_ = 2;
    }
}

void DiagnosisPage::invalidate() {
    static_step_ = 0;
    foot_.invalidate();
    stamp_.invalidate();
    ctx_.rows.invalidate();
}

// Row i of the screen form. A finding wears its verdict cell, a healthy line the pass cell; the data-lane row an icon and no
// verdict. The label, tail and note are views into row_, valid until the next row.
void DiagnosisPage::row_fn(void *self, int index, bool focused, kit::RowSpec &out) {
    auto *p = static_cast<DiagnosisPage *>(self);
    out = kit::RowSpec{};
    const diag::Report &r = live::report();
    if (p->place_ && index == p->rows()) {   // the Log place's last row: the Black Box, with its captures in the note
        unsigned caps = 0;
        for (std::size_t i = 0; i < pdtrace::kCapSlots; i++) caps += pdtrace::capture_info(i).trigger != pdtrace::Trigger::None;
        out.marker = kit::Marker::Icon;
        out.icon = Icon::Blackbox;
        strpack::assign_sv(out.text, kBlackBox);
        out.chevron = true;
        if (focused) {
            p->bb_note_.clear();
            if (caps) p->bb_note_.put_u32(caps).put(caps == 1 ? K2_P(" capture kept") : K2_P(" captures kept"));
            else p->bb_note_.put(K2_P("no captures yet"));
            out.note = p->bb_note_.view();
        }
        return;
    }
    if (!screen::row(r, static_cast<unsigned>(index) + p->skip(), p->row_)) {   // no session yet: the waiting row
        out.marker = kit::Marker::Icon;
        out.icon = Icon::Blackbox;
        strpack::assign_sv(out.text, kWaitText);
        out.text_color = kDim;
        if (focused) strpack::assign_sv(out.note, kWaitNote);
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
    strpack::assign_sv(out.text, w.label.data());
    strpack::assign_sv(out.tail, w.tail.data());
    out.tail_color = kDim;
    out.tail_yields = true;
    if (focused) strpack::assign_sv(out.note, w.note.data());
}

void DiagnosisPage::update(uint32_t now_ms) {
    PaintBudget *const bg = ctx_.budget;
    const diag::Report &r = live::report();
    title_badge(live::state() == live::State::Last);
    if (bg && bg->more) return;
    if (place_) {
        paint_stamp();
        if (bg && bg->more) return;
    }
    sel_ = std::clamp(sel_, 0, list_rows() - 1);
    ctx_.rows.set_count(list_rows());
    ctx_.rows.select(sel_);
    ctx_.rows.draw(now_ms, bg);
    if (bg && bg->more) return;
    char v[screen::kVerdictChars + 8];
    screen::verdict(r, v);
    const FooterTone tone = verdict_tone(r);
    if (tone == FooterTone::Error || tone == FooterTone::Caution) {
        pill_ = draw_footer_budgeted(ctx_.content, tone, v, foot_, bg) || pill_;
        return;
    }
    if (pill_) {   // the finding is gone: take its pill off, the rows under it repaint
        erase_toast(ctx_.content);
        ctx_.rows.invalidate();
        foot_.invalidate();
        pill_ = false;
    }
    status_toast(ctx_.toast, now_ms, toast_sig_, tone, v);   // a healthy or neutral verdict is no exception: it times out
}

FooterTone DiagnosisPage::tone_of(const diag::Report &r) {
    const diag::Sev worst = screen::worst(r);
    if (worst == diag::Sev::Fail) return FooterTone::Error;
    if (worst == diag::Sev::Warn) return FooterTone::Caution;
    if (!r.empty && r.link == 0 && r.power == diag::kPowerOk) return FooterTone::Ok;
    return FooterTone::Info;
}

FooterTone DiagnosisPage::verdict_tone(const diag::Report &r) {
    if (r.empty || r.no_pd) return FooterTone::Info;
    if (r.link == 2 || r.power == diag::kPowerShort) return FooterTone::Error;
    if (r.link == 1 || r.power == diag::kPowerOver) return tone_of(r) == FooterTone::Error ? FooterTone::Error : FooterTone::Caution;   // a failing finding keeps the hero's red
    return r.power == diag::kPowerOk ? FooterTone::Ok : FooterTone::Info;
}

bool DiagnosisPage::on_event(Event e) {
    if (e.press == Press::Short && (e.button == Button::Plus || e.button == Button::Minus)) {
        sel_ = std::clamp(sel_ + (e.button == Button::Plus ? 1 : -1), 0, list_rows() - 1);
        update(ctx_.now);   // the Ui re-reads labels() after the event: the OK verb comes and goes with the Black Box row
        return true;
    }
    if (e.press == Press::Short && e.button == Button::Ok && on_blackbox()) {
        ctx_.ui->push_menu(Menu::BlackBox, ctx_.now);
        return true;
    }
    return false;
}

Strip DiagnosisPage::labels() const {
    Strip l = list_strip(base_labels(), sel_, list_rows());   // short [+]/[-] move the selection; OK opens only the Black Box row
    l[Button::Ok] = on_blackbox() ? sl::Open : sl::None;
    return l;
}

}  // namespace k2
