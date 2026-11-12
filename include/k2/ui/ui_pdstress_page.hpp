// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> PD Stress: the painted half of the renegotiation stress tool. The run itself lives in PdStressSession (resident in Ui), so a
// run goes on while the page is closed; this page only picks the options, starts the run, follows it and shows the results.
//
// Stages (kit components only, docs/UI_DESIGN.md):
//   Setup   "1/2 Sequences": a RowList of P1..P12 (checkboxes, the focused row shows its note), then Cycles, Pace and Done, then the
//           saved results ("Result #N"). Footer "N of 12 picked".
//   Review  "2/2 Review": a KvTable (Tests, Cycles, Pace, Charger, Output) and the START ActionCell with the two-press confirm. START calls
//           PdStressSession::start(); a refusal lands in the footer.
//   Running the sequence n/N and the elapsed time over a ProgressBar, the sequence name on its own row, VBUS at 48 px, and the counts
//           in a two-column KvTable. BACK stops the run.
//   Results the provisional score, the clean / issues / skip counters and a ResultGrid with one row per picked sequence; a live result
//           adds the Save result / Discard rows (nothing is written before Save). OK on a sequence opens Detail.
//   Detail  the sequence's event counters, two pages of seven rows.
// What must outlive the page (the options, the cursor, which result was already dealt with) lives in UiContext::keep.pdstress.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/pdstress/digest.hpp"
#include "k2/pdstress/pdstress_store.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

class PdStressSession;

class PdStressPage final : public PageBase {
public:
    enum class Stage : uint8_t { Setup, Review, Running, Results, Detail };

    static constexpr std::array<uint16_t, 4> kCyclesPct = {25, 50, 100, 200};   // UiContext::keep.pdstress.cycles indexes it
    static constexpr int kSeqRows = static_cast<int>(pdstress::kSeqCount);
    static constexpr int kCyclesRow = kSeqRows, kPaceRow = kSeqRows + 1, kDoneRow = kSeqRows + 2, kSavedRow0 = kSeqRows + 3;
    static constexpr int kDetailRows = 7, kDetailPages = 2;
    static constexpr int kCountCells = 6;   // Running: Transitions, Hard resets, Soft resets | Rej / Wait, Out of band, Reboots
    // Running paints at most kRunBudgetPx a tick after its layout (docs/UI_DESIGN.md paint budgets); the TopBar readout and the
    // footer (kChromePx: both lines, 224 x 21 + 224 x 18) are the chrome every page pays on top.
    static constexpr int kRunBudgetPx = 6000;
    static constexpr int kChromePx = 224 * 21 + 224 * 18;

    explicit PdStressPage(UiContext &c);
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override;

    // What the page shows now (host tests and the remote's view of it).
    Stage stage() const { return stage_; }
    std::string_view footer() const { return foot_text_.view(); }
    bool armed() const { return confirm_until_ != 0; }
    uint16_t viewing() const { return viewing_; }                 // 0 = the live result, else the saved result's id
    bool result_valid() const { return res_valid_; }
    pdstress::Stop result_stop() const { return res_stop_; }
    pdstress::digest::Verdict seq_verdict(pdstress::Seq s) const {
        return static_cast<pdstress::digest::Verdict>(res_seq_[static_cast<std::size_t>(s)].verdict);
    }
    int result_selected() const { return res_sel_; }
    int result_rows() const;                                       // the grid's row count (sequences, then Save / Discard)
    int detail_page() const { return detail_page_; }
    int saved_count() const { return nsaved_; }
    uint32_t count_value(int i) const;                             // Running: the count cell i shows (before the 9999 cap)
    uint32_t detail_value(int row) const;                          // Detail: row `row` of the open page (the score in hundredths)
    // The rows as the RowList / ResultGrid get them (the tests read them the same way).
    void setup_row(int index, bool focused, kit::RowSpec &out);
    void result_row(int index, kit::ResultRowSpec &out);

private:
    struct Saved {
        uint16_t id = 0;
        uint8_t n = 0;          // sequences picked in that run
        uint8_t stop = 0;       // pdstress::Stop
        uint32_t score_cp = 0;
    };

    // What the page keeps of a result: the full digest (344 B) lives in the scratch lease only while it decodes.
    struct SeqSummary {
        uint8_t verdict = 0;    // pdstress::digest::Verdict
        uint8_t na = 0;         // pdstress::Na
        uint16_t events = 0;    // every event of the sequence (saturated)
    };

    PdStressSession &session() const;
    void go(Stage s);
    void load_saved_list();
    bool load_live();
    bool load_saved(uint16_t id);
    bool decode(bool detail);   // the result viewing_ names into the summary (and det_counts_ for detail_seq_)
    void say(std::string_view text, uint16_t color);
    void compose_footer(uint32_t now_ms);
    void bar();
    bool start_run();
    void save_result();
    void close_result(bool saved, uint16_t id);
    int setup_count() const { return kSavedRow0 + nsaved_; }

    void paint_setup();
    void paint_review(uint32_t now_ms);
    void paint_running(uint32_t now_ms);
    void paint_results();
    void paint_detail();

    bool on_setup(Event e);
    bool on_review(Event e);
    bool on_results(Event e);
    bool on_detail(Event e);

    static void setup_row_fn(void *ctx, int index, bool focused, kit::RowSpec &out);
    static void result_row_fn(void *ctx, int index, kit::ResultRowSpec &out);

    Stage stage_ = Stage::Setup;
    bool laid_ = false;                  // the stage's static part is on screen
    bool bar_drawn_ = false;
    uint16_t viewing_ = 0;
    bool res_valid_ = false;
    pdstress::Stop res_stop_ = pdstress::Stop::None;
    uint32_t res_score_cp_ = 0;
    std::array<uint8_t, 3> res_tally_{};                    // clean, issues, skip
    std::array<SeqSummary, pdstress::kSeqCount> res_seq_{};
    pdstress::Counts det_counts_{};                          // Detail: the open sequence's counters
    std::array<pdstress::Seq, pdstress::kMaxSeqs> rows_{};
    uint8_t nrows_ = 0;
    std::array<Saved, pdstress::store::kMaxSaved> saved_{};
    uint8_t nsaved_ = 0;
    int res_sel_ = 0;
    pdstress::Seq detail_seq_ = pdstress::Seq::Ladder;
    int detail_page_ = 0;
    uint32_t confirm_until_ = 0;
    uint32_t hero_ms_ = 0;               // Running: the last VBUS repaint (4 Hz)
    uint32_t clock_s_ = 0xFFFFFFFF;      // Running: the elapsed second last painted
    int bar_done_ = -1;

    StrBuf<28> note_;                    // a refusal or a result for a moment
    uint16_t note_color_ = 0;
    uint32_t note_until_ = 0;
    StrBuf<28> foot_text_;
    FooterTone foot_tone_ = FooterTone::Info;
    Elem foot_;

    // Row texts the list callbacks hand out (valid until the next callback).
    StrBuf<16> row_text_, row_tail_;
    StrBuf<32> row_note_;
    StrBuf<12> cell_text_;

    // The stage's value cells, re-initialised per stage: Review 5, Running 10 (seq, clock, name, 6 counts, the VBUS hero last),
    // Results 1, Detail 7.
    static constexpr std::size_t kHeroCell = 9;
    std::array<Field, 10> cell_;
    std::array<uint32_t, 10> sig_{};      // what each cell last painted (text and colour), 0 = repaint
    kit::ActionCell action_;
    kit::ProgressBar prog_;
    kit::ResultGrid grid_;
};

}  // namespace k2
