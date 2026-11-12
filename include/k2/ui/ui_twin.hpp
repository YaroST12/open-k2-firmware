// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> Device twins: what each stored device would get from the charger on the port, and the live proof of it.
//
// One kit RowList: a row per twin (name left, the prediction right in the Home protocol-row style: "27W PPS", "60W of 96W",
// "5V only"), the last row "Learn from device". The focused row opens a note that names the limiter (full / the device
// takes no more / limited by this charger / limited by the cable). OK on a twin row asks the live runner to start without the
// confirm: it answers with the real refusal (a test running, a device inline, no charger ...) or "need the confirm", and then
// the row is armed for kConfirmWindowMs, like Review's START; the second OK starts the proof. A finished row wears the verdict
// cell (pass / fail / skipped / stopped) and the note reads the live text ("27W PPS ok 9.0V"). The footer states the one fact
// that matters now (present::footer, or the run's progress, or the last refusal).
// The page keeps a small cache of the library (name + prediction per twin, 12 at most) and repredicts only when the library
// or the charger's list (twin_hw::predict_stamp) changed; painting never reads flash. The texts are the ones the shell and the
// host tool print (k2/twin/present.hpp).
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/twin/library.hpp"
#include "k2/twin/predict.hpp"
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
    bool armed() const { return armed_seq_ != 0; }
    bool reading() const { return learn_wait_; }
    std::string_view footer() const { return foot_text_.view(); }   // what the footer shows now (tests, remote screenshots)
    // Row `index` as the list paints it (views stay valid until the next call into the page).
    void peek(int index, bool focused, kit::RowSpec &out) { build_row(index, focused, out); }

private:
    struct Entry {
        uint8_t seq = 0;
        bool have = false;                                   // a prediction exists (the charger listed something)
        twin::Outcome out{};
        std::array<char, twin::kNameMax + 1> name{};
    };
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);
    void build_row(int index, bool focused, kit::RowSpec &out);
    void reload();                                           // the library and the predictions
    void compose_footer();
    void say(std::string_view text, uint16_t color);
    void learn();
    void learn_row(uint32_t now_ms);                         // OK on the Learn row: read a device alone first, then learn
    void learn_wait_end();
    void run_row();
    int rows() const { return n_ + 1; }                      // twins + the Learn row
    bool running() const;
    const Entry *entry_by_seq(uint8_t seq) const;

    std::array<Entry, twin::kCapacity> list_{};
    int n_ = 0;
    int full_ = 0;
    bool have_caps_ = false;
    bool loaded_ = false;
    bool hide_proof_ = false;                                // another charger since the last run: its verdicts no longer apply
    bool learn_wait_ = false;                                // this page started an Identify and learns when it ends
    uint32_t learn_wait_until_ = 0;
    bool started_ = false;                                   // this page started the run in flight (leave() stops only that one)
    uint32_t gen_ = 0, stamp_ = 0;
    int sel_ = 0;
    uint8_t armed_seq_ = 0;
    uint32_t armed_until_ = 0;
    StrBuf<27> foot_text_;
    FooterTone foot_tone_ = FooterTone::Info;
    StrBuf<27> note_;                                        // refusal / result line, shown for a moment
    uint16_t note_color_ = 0;
    uint32_t note_until_ = 0;
    StrBuf<16> row_tail_;                                    // the row row_fn is building (RowSpec views point into these)
    StrBuf<40> row_note_;
    Elem foot_;                                              // footer element cache (draw_footer_cached)
};

}  // namespace k2
