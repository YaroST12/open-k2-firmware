// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> PD diagnosis: the Black Box diagnosis of the PD session (k2/pd/diag_live.hpp), read-only and passive.
//
// One kit RowList in the screen form (k2/pd/diag_screen.hpp): the findings in rank order with a verdict cell (Fail red,
// Warn amber, Info none), the time of the event at the right, the evidence in the focused row's note; then the healthy
// lines (Pass) and the data-lane row, which carries no verdict (the lanes pass through the K2, it does not observe
// them). The footer is the one-line verdict in the worst finding's tone. A finished session wears the LAST pill.
// Before any session one row says the page waits for PD.
#pragma once
#include <cstdint>

#include "k2/pd/diag_screen.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

class DiagnosisPage final : public PageBase {
public:
    explicit DiagnosisPage(UiContext &c) : PageBase(c, Page::Count, "PD diagnosis", Icon::Blackbox) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override { sel_ = 0; }
    const ListState &list_state() const { return ctx_.rows.state(); }
    int selected() const { return sel_; }
    // The Log place (Bench > Log): the title and icon of the place, the verdict as a stamp over the findings, and a last row
    // that drills into the Black Box. Called before enter() and the first paint.
    void as_place();
    bool place() const { return place_; }
    // The stamp: y22..69, the verdict's fill (kSurface when it has none), its 12 px glyph in a 20 px cell at x4 and the word
    // (screen::word, the Home row-1 word, <= 16 chars) in font 24 from x30 to x222. When the word is the top finding's label
    // (screen::names_top_finding) the list does not repeat that finding: its evidence note is the stamp's sub-line (font 12,
    // kStampSubY under the stamp's top, from x30) and the word sits at the top; else the word is centred. The list starts
    // 2 px under the stamp.
    static constexpr int16_t kStampH = 48, kStampWordX = 30, kStampWordDy = 4, kStampSubY = 31;
    // The stamp's verdict: a Fail finding red, a Warn amber, a healthy link with the power it needed green, else none.
    static kit::Verdict stamp_of(const pd::diag::Report &r);

    // The verdict cell of a finding's severity (Fail red, Warn amber, Info none, Ok pass).
    static kit::Verdict cell_of(pd::diag::Sev s);
    // The footer tone of a report: the worst finding's (Error, Caution), Ok only for a healthy link that had the power it
    // needed, Info otherwise (no session, a contract not seen, Info findings only).
    static FooterTone tone_of(const pd::diag::Report &r);

private:
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);
    int rows() const;                     // the listed screen rows (less a finding the stamp names), or the one waiting row
    unsigned skip() const;                // screen rows the stamp already shows: 0, or 1 in the Log place
    int list_rows() const { return rows() + (place_ ? 1 : 0); }   // ... and the Black Box row of the Log place
    bool on_blackbox() const { return place_ && sel_ == rows(); }
    void paint_stamp();

    int sel_ = 0;
    bool place_ = false;
    Elem stamp_;                          // the stamp on screen
    pd::diag::screen::Row row_{};         // the row row_fn is building (the RowSpec views point into it)
    StrBuf<24> bb_note_;                  // the Black Box row's note
    Elem foot_;                           // footer element cache (draw_footer_cached)
    uint8_t static_step_ = 0;
};

}  // namespace k2
