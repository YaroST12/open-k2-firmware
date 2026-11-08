// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> Report cards: the charger report cards saved by the charger test (k2/chargertest/report_card_hw.hpp).
// A list of the saved chargers (the kit RowList), OK opens one dense card, long BACK deletes after a confirm.
// Own header and source so the tool stays out of the ui_pages.* files other branches edit.
#pragma once
#include <array>
#include <cstdint>

#include "k2/chargertest/report_card.hpp"
#include "k2/chargertest/report_card_store.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

// Host-tested text. A card is named by its user name, else "VID:PID", else "#" + fingerprint.
void cards_title_text(const report_card::Card &c, StrBuf<24> &out);
// The open list row's note line (at most 30 characters): the full title and the boot counter, "Anker 735  boot 4".
void cards_note_text(const report_card::Card &c, StrBuf<32> &out);
// One PPS/AVS range, without its algorithm label: "3.3-21V" (volts from 100 mV units).
void cards_range_text(const report_card::Range &r, StrBuf<32> &out);
// Advertised current, 50 mA steps: "3A", "1.5A"; empty when unknown (0).
void cards_amps_text(unsigned ma, StrBuf<8> &out);
// What became of one range, for the cell beside the verdict marker: "caps 19.4V" when the source plateaued below the target
// (the PD group's reason is Capped), "at 13V" when a step failed, "reached 20V" when it stopped early without a failure; empty
// when the range passed (the marker says so; a 20 V step that measured 19.4 V passed, within the tolerance) or nothing ran.
void cards_range_status_text(const report_card::Card &c, const report_card::Range &r, StrBuf<16> &out);
// The cause on the PD fixed row when nothing passed and no target is stored (22 cells): the group's reason, or "see detail"
// when the words are longer (the group's detail screen carries them in full).
void cards_pd_why_text(const report_card::GroupResult &r, StrBuf<24> &out);
// The row summary, in the badge tones of report_card::tone (the Results grid's table): "2P 1~ 1F" = rows passed, partly (capped
// or stopped), failed. Protocols the charger never answered are grey and not counted. Empty when nothing was tested.
void cards_summary_text(const report_card::Card &c, StrBuf<12> &out);
// One protocol group: the cell of the two column grid ("12V x20V": highest passing, first failing target; "5V only":
// handshake level) and the PD row's "max 20V" / "fail 28V".
void cards_group_cell_text(const report_card::GroupResult &r, StrBuf<16> &out);
void cards_group_max_text(const report_card::GroupResult &r, StrBuf<16> &out);
void cards_group_fail_text(const report_card::GroupResult &r, StrBuf<16> &out);
// Handover value: "fell back 3.2s" (outcome, then the PD give-up time; the time is only known after a try).
void cards_handover_text(const report_card::Card &c, StrBuf<32> &out);
// Footer of the open card: "run 3  boot 4  fw 0FF2C610" (at most 28 characters).
void cards_stats_text(const report_card::Card &c, StrBuf<32> &out);
// "0B05:1234  fp 1A2B3C4D", the id line under the title (at most 32 characters).
void cards_id_text(const report_card::Card &c, StrBuf<32> &out);

// ---- the detail sheet of a group or range (host-tested) ------------------------------------------------------------------
// OK on a card row opens its sheet: a key (S12, dim) and a value per row, and only rows that carry information (no "none", no
// "not stored", no cause for a group that passed). A value of up to kSheetBox cells (S16) sits beside its key; a longer one
// goes under the key at the left margin, wrapped at spaces into at most kSheetLines lines of kSheetWrap cells. Nothing is cut:
// cards_sheet leaves lines = 0 for a text that cannot wrap into that, and the host gate (tests) fails on it.
constexpr int kSheetRows = 8, kSheetBox = 18, kSheetWrap = 26, kSheetLines = 2;
constexpr int kSheetPitch = 20, kSheetKeyH = 16, kSheetLineH = 17, kSheetValueX = 80;   // px
struct SheetRow {
    const char *key = "";
    bool verdict = false;                    // the Verdict row: glyph, then the word
    kit::Verdict glyph = kit::Verdict::None;
    uint16_t color = 0;
    bool below = false;                      // the text sits under the key
    uint8_t lines = 0;                       // 0 = nothing fits (a bug the gate catches)
    std::string_view line[kSheetLines];
    char num[kSheetBox + 1] = {};            // the formatted number line[0] views, when there is one (never longer than the box)
    SheetRow() = default;
    SheetRow(const SheetRow &) = delete;
    SheetRow &operator=(const SheetRow &) = delete;
    int height() const { return below ? kSheetKeyH + kSheetLineH * lines + 2 : kSheetPitch; }
};
struct CardSheet {
    SheetRow row[kSheetRows];
    unsigned n = 0;
    int height() const { int h = 0; for (unsigned i = 0; i < n; ++i) h += row[i].height(); return h; }
};
// Splits `text` at spaces into at most `max_lines` lines of `cells` cells each; returns the line count, 0 when it cannot.
unsigned cards_wrap(std::string_view text, unsigned cells, std::string_view *lines, unsigned max_lines);
// The sheet of the PD fixed / legacy group `idx` (range = false) or of the PPS / AVS range `idx`.
void cards_sheet(const report_card::Card &c, bool range, unsigned idx, CardSheet &out);

class CardsPage final : public PageBase {
public:
    explicit CardsPage(UiContext &c) : PageBase(c, Page::Count, "Report cards", Icon::Report) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override;
    ListState list_state() const { return mode_ == Mode::List && n_ > 0 ? ctx_.rows.state() : ListState{}; }

    enum class Mode : uint8_t { List, Detail, Confirm, Group };
    Mode mode() const { return mode_; }
    int count() const { return n_; }
    int row() const { return row_; }   // focused row of the open card: PD fixed, ranges, then the legacy groups

private:
    void refresh();                 // re-reads the store
    void paint_list(bool rebuild);  // rebuild: bind and repaint every row; else select + draw (only the changed rows write)
    void paint_empty();
    void paint_detail();
    void paint_cell(int row, bool focused);   // one row of the open card (full width, or half of a legacy pair)
    void move_focus();              // Detail +/-: repaint the old and the new focused cell
    void paint_confirm();
    void paint_group();             // every stored value of the focused row
    // title_data / sub_data: the text is a name the user typed (cut by design, counted by the gate); else it is ours and must fit.
    void paint_head(kit::Verdict v, std::string_view title, std::string_view sub, bool title_data, bool sub_data);
    int rows() const;               // selectable rows of cur_
    void clear_body();
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);   // the kit RowList: one saved charger

    // The list holds only the card numbers (newest first); a card is read from the store when it is drawn.
    std::array<std::uint16_t, report_card::kMaxCards> nums_{};
    report_card::Card cur_{};       // the card at idx_ (see load_cur); in List mode the row being built (row_fn)
    void load_cur();
    void share_cur();
    int n_ = 0;
    int idx_ = 0;
    int row_ = 0;
    Mode mode_ = Mode::List;
    uint32_t sig_ = 0;              // move signature (mode/row/idx): navigation-only repaints
    uint32_t content_sig_ = 0;      // content signature (mode/count/store generation/note): full rebuild
    bool painted_ = false;
    uint16_t note_color_ = 0;
    StrBuf<24> note_;
    uint32_t note_until_ = 0;
    Elem foot_;                     // the footer band
    int prev_row_ = -1;             // Detail: the cell that holds the focus bar now
    // The text of the one list row row_fn is building; the RowSpec views it until the next row.
    struct { char text[24]; char tail[12]; char note[32]; } line_ = {};
};

}  // namespace k2
