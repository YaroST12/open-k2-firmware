// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> Device twins: what each stored device would get from the charger on the port, and the live proof of it.
// Layout and wording: docs/UI_SYSTEM.md section 9 (the user's design, 2026-11-12).
//
// Two views on one page.
//  List: a kit RowList, a row per twin (its full name, never cut) with a shape marker for the prediction against this charger
//    (solid = full power, half = limited, hollow = 5 V only; half and hollow in amber ink), then "Learn from device". The
//    focused row opens a note ("gets 36 of 55W, cable"). Once a live proof ran on this charger the marker is the kit verdict
//    cell and the note reads the result ("proved 36W", "charger refused 12V"). Footer: the charger's top offer and the count
//    ("PD 20V3A: 2 of 5 full"). OK on a twin opens its detail; OK on Learn reads a device alone on the K2.
//  Detail: TopBar "Twin"; the name; the gets / wants block (font 24 watts, a bar of what it gets against what it wants, the
//    contract and what limits it); InfoBlock lines Wants / Offer / USB; then action rows Prove live, Re-learn from device,
//    Delete twin. The page scrolls by whole lines so the focused action is on screen. Prove live and Delete arm a kit
//    ActionCell (CONFIRM, 5 s drain) like the charger test's START; the proof then shows Asked / VBUS in that box.
// The page keeps a small cache of the library (name + prediction per twin, 12 at most) and repredicts only when the library
// or the charger's list (twin_hw::predict_stamp) changed; painting never reads flash. The detail's twin is read once when it
// opens (and again after the library changes).
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/twin/library.hpp"
#include "k2/twin/predict.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

class TwinPage final : public PageBase {
public:
    explicit TwinPage(UiContext &c) : PageBase(c, Page::Count, "Device twins", Icon::Phone) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override;
    void leave() override;
    const ListState &list_state() const { return ctx_.rows.state(); }
    int cursor() const { return sel_; }
    bool in_detail() const { return detail_; }
    int action() const { return act_; }            // the focused action row of the detail (0 Prove, 1 Re-learn, 2 Delete)
    int first_line() const { return first_; }      // detail lines scrolled off the top
    bool armed() const { return arm_ != Arm::None; }
    bool reading() const { return learn_wait_; }
    std::string_view footer() const { return foot_text_.view(); }   // what the footer shows now (tests, remote screenshots)
    // Row `index` as the list (or the detail's action list) paints it (views stay valid until the next call into the page).
    void peek(int index, bool focused, kit::RowSpec &out) { build_row(index, focused, out); }

    static constexpr int kActions = 3;

private:
    struct Entry {
        uint8_t seq = 0;
        bool have = false;                                   // a prediction exists (the charger listed something)
        twin::Outcome out{};
        std::array<char, twin::kNameMax + 1> name{};
    };
    enum class Arm : uint8_t { None, Prove, Delete };
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);
    void build_row(int index, bool focused, kit::RowSpec &out);
    void build_list_row(int index, bool focused, kit::RowSpec &out);
    void build_action_row(int index, bool focused, kit::RowSpec &out);
    void entry_note(const Entry &e, bool running_this, StrBuf<40> &out) const;
    void reload();                                           // the library and the predictions
    void compose_footer();
    void say(std::string_view text, uint16_t color);
    void learn();
    void learn_row(uint32_t now_ms);                         // OK on Learn / Re-learn: read a device alone first, then learn
    void learn_wait_end();
    void run_entry(const Entry &e);
    void open_detail();
    void close_detail();
    void delete_twin();
    void repaint();                                          // title, body and footer of the current view
    // Detail layout: the body lines (name, gets, bar, contract, rule, InfoBlock) and where the action box starts.
    int line_count() const { return 5 + info_n_; }
    int16_t line_h(int i) const;
    void paint_line(int i, int16_t y);
    void layout_detail();                                    // first_ and the action box for the focused action
    void paint_detail(uint32_t now_ms);
    void build_info();                                       // Wants / Offer / USB texts of the open twin
    int rows() const { return detail_ ? kActions : n_ + 1; }   // twins + the Learn row
    bool running() const;
    const Entry *entry_by_seq(uint8_t seq) const;
    const Entry *det_entry() const { return entry_by_seq(det_seq_); }

    std::array<Entry, twin::kCapacity> list_{};
    int n_ = 0;
    int full_ = 0;
    bool have_caps_ = false;
    bool loaded_ = false;
    bool wake_ok_ = false;                                   // no list, a charger waits on the plug: Prove live may wake it (twin_hw::can_wake)
    bool hide_proof_ = false;                                // another charger since the last run: its verdicts no longer apply
    bool learn_wait_ = false;                                // this page started an Identify and learns when it ends
    uint32_t learn_wait_until_ = 0;
    bool started_ = false;                                   // this page started the run in flight (leave() stops only that one)
    uint32_t gen_ = 0, stamp_ = 0;
    int sel_ = 0;                                            // list cursor
    Arm arm_ = Arm::None;
    uint32_t armed_until_ = 0;
    StrBuf<16> offer_;                                       // the charger's top offer ("PD 20V3A"), empty without a list
    StrBuf<27> foot_text_;
    FooterTone foot_tone_ = FooterTone::Info;
    StrBuf<27> note_;                                        // refusal / result line, shown for a moment
    uint16_t note_color_ = 0;
    uint32_t note_until_ = 0;
    StrBuf<40> row_note_;                                    // the row row_fn is building (RowSpec views point into it)
    Elem foot_;                                              // footer element cache (draw_footer_cached)

    // ---- detail ----
    bool detail_ = false;
    uint8_t det_seq_ = 0;
    twin::Twin det_{};                                       // the open twin, read once
    int act_ = 0;                                            // focused action row
    int first_ = 0;                                          // body lines scrolled off the top
    int16_t box_y_ = kit::kListBottom;                       // top of the action box (rows, the armed cell, the proof)
    uint32_t det_sig_ = 0;                                   // what the body was last painted from (0 = repaint)
    int info_n_ = 0;                                         // InfoBlock lines: 3..5
    std::array<const char *, 5> info_tag_{};
    std::array<StrBuf<24>, 5> info_val_{};
    kit::ActionCell cell_;
    StrBuf<12> vbus_shown_;                                  // the proof box's VBUS text as painted
    uint32_t vbus_next_ = 0;
};

}  // namespace k2
