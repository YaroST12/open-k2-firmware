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

    // The verdict cell of a finding's severity (Fail red, Warn amber, Info none, Ok pass).
    static kit::Verdict cell_of(pd::diag::Sev s);
    // The footer tone of a report: the worst finding's (Error, Caution), Ok only for a healthy link that had the power it
    // needed, Info otherwise (no session, a contract not seen, Info findings only).
    static FooterTone tone_of(const pd::diag::Report &r);

private:
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);
    int rows() const;                     // the screen rows, or the one waiting row before any session

    int sel_ = 0;
    pd::diag::screen::Row row_{};         // the row row_fn is building (the RowSpec views point into it)
    Elem foot_;                           // footer element cache (draw_footer_cached)
};

}  // namespace k2
