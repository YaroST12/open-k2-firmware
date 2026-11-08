// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> Black Box: the trigger-frozen PD captures (k2/pd/pdtrace.hpp) grouped by the attached
// device (VID:PID from a sniffed Discover Identity, else a Source_Capabilities fingerprint), and one
// capture as a compact timeline (message names with the time since the previous event, a VBUS strip).
// Own header and source so the submenu stays out of the ui_pages.* files other branches rewrite.
#pragma once
#include "k2/pd/pdtrace.hpp"
#include "k2/pd/pdtrace_view.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

class BlackBoxPage final : public PageBase {
public:
    explicit BlackBoxPage(UiContext &c) : PageBase(c, Page::Count, "Black Box", Icon::Blackbox) {}
    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    PanelLabels labels() const override;
    void invalidate() override;
    void enter() override { mode_ = Mode::List; select_first_ = true; }   // opens on the newest capture
    const ListState &list_state() const { return list_.state(); }

    static constexpr uint8_t kLive = 0xFF;  // sel_slot_ value of the live-link row
    static constexpr int kStripMax = 232;   // VBUS strip columns (upper bound)

private:
    enum class Mode : uint8_t { List, Timeline, Link };
    // One line of the grouped list: a group header (key, vendor, count) or a capture under it.
    struct Row {
        bool header;
        uint8_t slot;     // capture slot, or the group's newest slot for a header
        uint8_t count;    // header: captures in the group
    };
    struct SlotId {       // identity of a slot, rescanned only when the slot changed
        bool valid = false, filling = false;
        uint8_t trigger = 0;
        uint16_t len = 0, pre = 0;
        pdtrace::view::Identity id;
    };

    static void draw_line_row(void *self, int index, const Rect &r, bool hi, Field &f);
    static void row_fn(void *self, int index, bool focused, kit::RowSpec &out);   // the kit RowList: groups, captures, live
    void enter_mode(Mode m);
    void refresh_model();                  // slot identities, groups, row order
    int sel_row() const;                   // list row of the highlighted capture (the live row is the last)
    void paint_list();
    void paint_empty_text();
    void paint_timeline_head();
    void paint_link();                     // the negotiated link of a capture, or the live one
    void draw_strip();
    std::span<const uint8_t> data() const { return pdtrace::capture_data(slot_); }

    Mode mode_ = Mode::List;
    ListView list_;                        // timeline rows
    SlotId ids_[pdtrace::kCapSlots];
    Row rows_[2 * pdtrace::kCapSlots] = {};
    uint8_t nrows_ = 0;
    uint8_t order_[pdtrace::kCapSlots] = {};   // capture slots in list order (groups, newest first)
    uint8_t norder_ = 0;
    uint8_t slot_ = 0;                     // capture shown in the timeline
    uint8_t sel_slot_ = 0;                 // highlighted capture (a slot, so it survives re-grouping)
    bool select_first_ = true;
    bool link_live_ = false;               // Link mode shows the live link, not a capture
    Mode link_from_ = Mode::List;          // where Back returns to
    Elem foot_;                            // footer element cache (draw_footer_cached)
    uint32_t link_sig_ = 0;
    bool link_painted_ = false;
    int8_t bound_ = -1;                    // RowList bound for: 0 = captures, 1 = the empty page, -1 = repaint needed
    uint32_t head_sig_ = 0;
    bool head_painted_ = false;
    uint16_t shown_len_ = 0;               // its length when the rows were counted
    bool strip_dirty_ = true;
    // One buffer for the two modes that never run together: the timeline's VBUS strip columns, and the text of the one list
    // row row_fn is building (the kit RowSpec views it until the next row). Entering the timeline repaints the strip.
    union Scratch {
        uint8_t heights[kStripMax];
        struct { char a[24]; char tail[8]; } row;
    } sc_ = {};
    int strip_w_ = 0;
};

}  // namespace k2
