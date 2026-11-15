// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> PD Spec Check: the painted half of the no-load PD conformance check. The run lives in PdSpecSession (resident in Ui), so a
// run goes on while the page is closed; this page picks the families, starts the run, follows it and shows the result. K2_PDSPEC=1 only.
//
// Stages (kit components only, the charger test's language):
//   Setup   "1/2 Checks": a RowList of the six families (checkboxes, the focused row shows its note), Done, then "Last result" once a
//           run exists. Footer "N of 6 picked".
//   Review  "2/2 Review": a KvTable (Checks, Charger, Ceiling, EPR, Output) and the START ActionCell with the two-press confirm.
//   Running the family and the clock over a ProgressBar, the item line, VBUS at 48 px and four counts. BACK stops the run.
//   Results the score (penalty, 0 = ideal), the pass / fail / skip counters and a ResultGrid with one row per check: the worst value
//           measured, or the reason it was skipped. OK opens Detail.
//   Detail  the check's KvTable: verdict, worst value, limit, measurements, fails, the item it came from, penalty points.
// Nothing is written to flash: the remote (Cmd::PdSpec, `k2_remote pd-spec`) reads the full result.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/pdspec/result.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

class PdSpecSession;

class PdSpecPage final : public PageBase {
public:
    enum class Stage : uint8_t { Setup, Review, Running, Results, Detail };

    static constexpr int kFamilyRows = 6;                     // pdspec::kFamilies
    static constexpr int kDoneRow = kFamilyRows, kLastRow = kFamilyRows + 1;
    static constexpr int kDetailRows = 7;
    static constexpr int kCountCells = 4;                     // Running: Steps, PPS points | Fails, Score
    static constexpr int kRunBudgetPx = 6000;                 // Running paints at most this a tick after its layout

    explicit PdSpecPage(UiContext &c);
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override;

    // What the page shows now (host tests).
    Stage stage() const { return stage_; }
    std::string_view footer() const { return foot_text_.view(); }
    bool armed() const { return confirm_until_ != 0; }
    int result_selected() const { return res_sel_; }
    void setup_row(int index, bool focused, kit::RowSpec &out);
    void result_row(int index, kit::ResultRowSpec &out);   // index: the grid row (row_of maps a check to its row)
    static int row_of(pdspec::Check c);
    static pdspec::Check check_of(int row);
    void detail_value(int row, StrBuf<20> &out, uint16_t &color) const;
    // Detail "Where": the PDO pair, the programmable move (accept_time's too: b of 16 and up is a level, not a PDO), a PDO or the
    // source; false = "the source" (shown dim).
    static bool put_where(int check, uint8_t a, uint8_t b, StrBuf<20> &out);

private:
    PdSpecSession &session() const;
    void go(Stage s);
    void say(std::string_view text, uint16_t color);
    void compose_footer(uint32_t now_ms);
    void bar();
    bool start_run();
    int setup_count() const;
    const pdspec::Agg *agg(int check) const;

    void paint_setup();
    void paint_review(uint32_t now_ms);
    void paint_running(uint32_t now_ms);
    void paint_results();
    void paint_detail();

    bool on_setup(Event e);
    bool on_review(Event e);
    bool on_results(Event e);

    static void setup_row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    static void result_row_fn(void *ctx, int index, kit::ResultRowSpec &out);

    Stage stage_ = Stage::Setup;
    bool laid_ = false;
    bool bar_drawn_ = false;
    int res_sel_ = 0;
    uint32_t confirm_until_ = 0;
    uint32_t hero_ms_ = 0;
    uint32_t clock_s_ = 0xFFFFFFFF;

    StrBuf<28> note_;
    uint16_t note_color_ = 0;
    uint32_t note_until_ = 0;
    StrBuf<28> foot_text_;
    FooterTone foot_tone_ = FooterTone::Info;
    Elem foot_;

    StrBuf<24> cell_text_;
    StrBuf<8> row_tail_;

    // The stage's value cells: Review 5, Running 7 (family, clock, item, 4 counts) + the VBUS hero, Detail 7.
    static constexpr std::size_t kHeroCell = 8;
    std::array<Field, 9> cell_;
    std::array<uint32_t, 9> sig_{};
    kit::ActionCell action_;
    kit::ProgressBar prog_;
    kit::ResultGrid grid_;
};

}  // namespace k2
