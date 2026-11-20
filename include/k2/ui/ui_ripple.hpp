// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Ripple Meter face: live AC-coupled VBUS waveform on PA1 with Vpp / Vrms / frequency readouts
// and an OK settings sheet (Live/Hold, Time/div, Scale, Trigger) reusing UiContext::rows.
#pragma once
#include <array>
#include <cstdint>

#include "k2/core/ripple.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

class RipplePage final : public PageBase {
public:
    static constexpr int16_t kPlotX = 5;
    static constexpr int16_t kPlotY = 48;
    static constexpr int16_t kPlotW = 198;
    static constexpr int16_t kPlotH = 184;
    static constexpr uint32_t kRefreshMs = 100;   // ~10 Hz capture cadence at boosted CPU/SPI
    static constexpr uint8_t kNoCol = 0xFF;

    explicit RipplePage(UiContext &c) : PageBase(c, Page::Ripple, "Ripple", Icon::Meter) {}
    ~RipplePage();

    void draw_static() override;
    void update(uint32_t now_ms) override;
    bool on_event(Event e) override;
    Strip labels() const override;
    void invalidate() override;
    void leave() override;

    // For host tests and inspection.
    bool settings_open() const { return settings_open_; }
    const ripple::Stats &stats() const { return stats_; }
    uint16_t active_mv_per_div() const { return mv_per_div_; }

private:
    static void build_row(void *ctx, int index, bool focused, kit::RowSpec &out);

    void take_sample(uint32_t now_ms);
    void enter_settings();
    void exit_settings();
    void draw_chips();
    bool paint_plot();
    void draw_col(const Surface &S, int16_t x, ripple::ColSpan col, uint16_t trace_ink);
    void paint_footer();

    SmallField<16> vpp_f_, freq_f_, vrms_f_;
    Elem foot_elem_{};
    ripple::Stats stats_{};
    uint16_t mv_per_div_ = 50;
    uint16_t vpp_ink_ = 0;
    uint16_t freq_ink_ = 0;
    uint32_t last_cap_ms_ = 0;
    uint32_t chip_sig_ = 0;
    uint8_t settings_sel_ = 0;
    uint8_t plot_col_ = 0;
    uint8_t saved_spi_div_ = 0;   // non-zero when SPI3 was boosted to /2 (60 MHz) on entry
    bool settings_open_ = false;
    bool have_sample_ = false;
    bool cap_due_ = true;
    bool adc_busy_ = false;
    bool frame_ok_ = false;
    bool plot_dirty_ = true;

    std::array<ripple::ColSpan, kPlotW> cols_{};
    std::array<ripple::ColSpan, kPlotW> shown_{};
};

}  // namespace k2
