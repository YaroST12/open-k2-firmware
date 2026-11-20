// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Graphs page (lab runs/ui-home/graphs-lab.html v5, approved 2026-11-14): four stacked full-width graphs, W, V, A and
// Wh, over the session's trend (UiContext::trend, recorded while any page shows), and one info line. No title bar, no
// footer. The pure parts (geometry, scales, texts) are graphs_model.hpp; this class decides what to repaint.
//
// No framebuffer: every graph column remembers what it shows (its segment, the limit dot, the newest-point dot), and a
// scroll repaints only the columns whose content changed, each as one SPI window over the union of the old and the new
// rows. A flat line costs nothing. The page draws in panel pixels (ContentViewport::Pause, Geom): a 1 px line keeps its
// row in the landscape rotations.
#pragma once
#include "k2/ui/graphs_model.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

class GraphsPage final : public PageBase {
public:
    explicit GraphsPage(UiContext &c) : PageBase(c, Page::Graphs, "") {}
    void draw_static() override {}   // update() lays every pixel on its first pass (the page is opaque)
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    kit::OppRailSpec opp_rail() const override { return {"GRAPHS", 0, 0, kit::OppTelemetry::PeakSagR}; }
    void invalidate() override;

    // What a column shows, in rows from the graph's top. kNoRow = nothing; lo == kUnknown = the panel is not known (the
    // column repaints whole).
    static constexpr uint8_t kNoRow = 0xFF, kUnknown = 0xFE;
    struct Col { uint8_t lo = kUnknown, hi = kNoRow; };

private:
    struct Shown;   // the values and texts one update shows (ui_graphs.cpp)
    void sample_shown(Shown &s, uint32_t now_ms);
    bool paint_frame();
    // digits = false: only the ground, label and texts when their signature changed; true: the digit field too.
    bool paint_head(int k, const Shown &s, bool digits);
    bool paint_info(const Shown &s, bool digits);
    bool paint_graph(int k);
    static constexpr uint16_t band_color(int k) {
        return k == 0 ? kWatt : k == 1 ? kVolt : k == 2 ? kAmp : kEnergy;
    }
    static constexpr uint16_t tone(home::Mode m, uint16_t live) {
        return m == home::Mode::Live ? live : m == home::Mode::Stale ? kStale : kFaint;
    }
    static bool admit(PaintBudget *bg, int px) { return !bg || bg->admit(px, true); }
    [[gnu::noinline]] void want(int k, Col (&out)[graphs::kCols], uint8_t (&lim_out)[graphs::kCols / 2],
                                uint8_t &dot_out) const;
    [[gnu::noinline]] void paint_col(int k, int c, const Col &now, uint8_t lim, uint8_t dot, int r0, int r1) const;
    // Left x of the newest point's 3 x 3 dot: one pixel before the right-most column, kept inside the graph.
    int dot_x0() const {
        const int x = g_.col_x(graphs::kCols - 1) - 1, last = g_.col_x(graphs::kCols) - 3;
        return x < last ? x : last;
    }

    graphs::Geom g_{};
    Throttle thr_{250};                      // the digits change at most 4 times a second
    int32_t v_mv_ = 0, a_ma_ = 0, w_mw_ = 0;  // |values| as shown
    uint8_t mode_ = 0xFF;                    // home::Mode last seen; 0xFF = none yet
    trend::Window shown_w_ = trend::Window::S10;
    trend::Window &window_ = ctx_.keep.home_window;   // the span, shared with Home's trends (OK = Zoom on both)
    bool frame_ok_ = false;                  // rules, margins and ground rows
    bool head_ok_[graphs::kBands] = {};
    uint32_t head_sig_[graphs::kBands] = {};
    bool info_ok_ = false;
    uint32_t info_sig_ = 0;
    bool el_shown_ = false;
    SmallField<6> val_[graphs::kBands];
    SmallField<6> el_;
    uint32_t seq_seen_ = 0;
    bool graph_dirty_ = true;                // columns left for the next pass (budget) or a full repaint pending
    uint8_t rr_ = 0;                         // the band whose columns go first (a sliced pass starves none)
    Col col_[graphs::kBands][graphs::kCols];
    uint8_t lim_[graphs::kCols / 2] = {};    // W-band limit row at odd columns (c >> 1)
    uint8_t dot_row_[graphs::kBands] = {kNoRow, kNoRow, kNoRow, kNoRow};
};

}  // namespace k2
