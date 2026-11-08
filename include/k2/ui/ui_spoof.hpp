// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> Spoof: make the K2 answer as a cable. The list is the one cable-identity list the charger wizard's Cable step
// shows (ui_cable_list.hpp): Off, the built-in chips, the saved clones, with the same names and the same USE / ARMED pills.
// OK here arms the row at once (the wizard only selects and arms at START); OK on the armed row disarms. Two key/value rows
// on top show what the K2 answers with (state, requests asked / replies sent) and the kit RowList below is the identity list;
// a live sink inline refuses the arm.
// Own header and source so the tool stays out of the ui_pages.* files other branches edit.
#pragma once
#include <cstdint>

#include "k2/ui/ui_pages.hpp"

namespace k2 {

// The counters row value: "3 / 3" (asked / replied).
void spoof_counts_text(uint32_t requests, uint32_t replies, StrBuf<24> &out);

class SpoofPage final : public PageBase {
public:
    explicit SpoofPage(UiContext &c) : PageBase(c, Page::Count, "Spoof", Icon::Ghost) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override;
    const ListState &list_state() const { return ctx_.rows.state(); }
    int cursor() const { return sel_; }

    static constexpr int16_t kHeaderRows = 2;   // State, Asked/Replied
    static constexpr int16_t kListY = kit::kBodyY + kHeaderRows * kit::kRowH;   // under the two key/value rows

private:
    static void row_fn(void *self, int index, bool selected, kit::RowSpec &out);
    void activate();                        // OK on the cursor row
    void say(const char *text, uint16_t color);
    void paint_header(bool armed, uint32_t req, uint32_t rep);
    int sel_ = 0;
    CableIdentityList::Row row_;            // the row row_fn is building (RowSpec views point into it)
    StrBuf<24> note_;                       // refusal / result line for the footer
    uint16_t note_color_ = 0;
    uint32_t note_until_ = 0;
    Elem hdr_;                              // the two key/value rows (signature: state and counters)
    Elem foot_;                             // footer element cache (draw_footer_cached)
};

}  // namespace k2
