// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Ripple Meter face (include/k2/ui/ui_ripple.hpp). Cold code (-Oz).
#include "k2/ui/ui_ripple.hpp"

#include <algorithm>
#include "k2/core/scratch.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/ui/kit.hpp"

namespace k2 {

namespace {

constexpr int16_t kFrameX = RipplePage::kPlotX - 1;
constexpr int16_t kFrameY = RipplePage::kPlotY - 1;
constexpr int16_t kFrameW = RipplePage::kPlotW + 2;
constexpr int16_t kFrameH = RipplePage::kPlotH + 2;

constexpr int16_t kRow1Y = 26;
constexpr int16_t kRow2Y = 47;

bool budget_ok(PaintBudget *bg, int px, bool fill = false) {
    return !bg || bg->admit(px, true, fill);
}

constexpr int16_t kGridQ1X = RipplePage::kPlotW / 4;
constexpr int16_t kGridMidX = RipplePage::kPlotW / 2;
constexpr int16_t kGridQ3X = (RipplePage::kPlotW * 3) / 4;
constexpr int16_t kGridQ1Y = RipplePage::kPlotH / 4;
constexpr int16_t kGridMidY = RipplePage::kPlotH / 2;
constexpr int16_t kGridQ3Y = (RipplePage::kPlotH * 3) / 4;

uint32_t hash_chips(uint8_t tb, uint8_t sc, bool hold) {
    return 1u + (uint32_t{tb} << 8) + (uint32_t{sc} << 4) + (hold ? 1u : 0u);
}

}  // namespace

void RipplePage::build_row(void *ctx, int index, bool focused, kit::RowSpec &out) {
    const auto &self = *static_cast<const RipplePage *>(ctx);
    const auto &cfg = self.ctx_.keep.ripple;
    const uint8_t tb = cfg.timebase < ripple::kTimebaseCount ? cfg.timebase : ripple::kDefaultTimebase;
    const uint8_t sc = cfg.scale < ripple::kScaleCount ? cfg.scale : 0;
    const bool free_trig = cfg.trigger == static_cast<uint8_t>(ripple::Trigger::Free);

    out.marker = kit::Marker::None;
    switch (index) {
    case 0:
        out.text = "Capture";
        out.tail = cfg.hold ? "Hold" : "Live";
        out.tail_color = cfg.hold ? kCaution : kOk;
        if (focused) out.note = "Freeze or resume waveform";
        break;
    case 1:
        out.text = "Time / div";
        out.tail = ripple::kTimebases[tb].menu_label;
        out.tail_color = kAccent;
        if (focused) out.note = "Horizontal span (4 divs)";
        break;
    case 2:
        out.text = "Scale";
        out.tail = ripple::kScaleMenu[sc];
        out.tail_color = kAccent;
        if (focused) out.note = "Vertical scale (+-2 divs)";
        break;
    case 3:
    default:
        out.text = "Trigger";
        out.tail = free_trig ? "Free" : "Auto";
        out.tail_color = kText;
        if (focused) out.note = "Rising edge or free run";
        break;
    }
}

void RipplePage::draw_static() {
    title_bar();
    const Surface &S = ctx_.content;
    if (!ctx_.blank) gfx_fill(S, 0, kit::kBelowBar, S.w, static_cast<int16_t>(S.h - kit::kBelowBar), kBg);

    vpp_f_.init(S, kSmall, 6, kRow1Y, 13, Align::Left, kText, kBg);
    freq_f_.init(S, kSmall, 126, kRow1Y, 11, Align::Right, kAccent, kBg);
    vrms_f_.init(S, kLabelFont, 6, kRow2Y, 13, Align::Left, kDim, kBg);
    vpp_ink_ = kText;
    freq_ink_ = kAccent;

    if (settings_open_) {
        ctx_.rows.init(S, kit::kBodyY, &RipplePage::build_row, this);
        ctx_.rows.set_count(4);
        ctx_.rows.select(settings_sel_);
        ctx_.rows.invalidate();
    } else {
        frame_ok_ = false;
        chip_sig_ = 0;
        for (ripple::ColSpan &c : shown_) c = {kNoCol, kNoCol};
        plot_col_ = 0;
        plot_dirty_ = true;
    }
    foot_elem_.invalidate();
}

void RipplePage::invalidate() {
    bar_valid_ = false;
    vpp_f_.invalidate();
    freq_f_.invalidate();
    vrms_f_.invalidate();
    frame_ok_ = false;
    chip_sig_ = 0;
    foot_elem_.invalidate();
    for (ripple::ColSpan &c : shown_) c = {kNoCol, kNoCol};
    plot_col_ = 0;
    plot_dirty_ = true;
    if (settings_open_) ctx_.rows.invalidate();
}

RipplePage::~RipplePage() {
    if (saved_spi_div_ != 0) {
        lcd::set_spi_div(saved_spi_div_);
        saved_spi_div_ = 0;
    }
}

void RipplePage::leave() {
    if (settings_open_) ctx_.rows.detach();
    if (saved_spi_div_ != 0) {
        lcd::set_spi_div(saved_spi_div_);
        saved_spi_div_ = 0;
    }
}

void RipplePage::enter_settings() {
    settings_open_ = true;
    const Surface &S = ctx_.content;
    gfx_fill(S, 0, kit::kBelowBar, S.w, static_cast<int16_t>(S.h - kit::kBelowBar), kBg);
    ctx_.rows.init(S, kit::kBodyY, &RipplePage::build_row, this);
    ctx_.rows.set_count(4);
    ctx_.rows.select(settings_sel_);
    ctx_.rows.invalidate();
    foot_elem_.invalidate();
}

void RipplePage::exit_settings() {
    ctx_.rows.detach();
    settings_open_ = false;
    const Surface &S = ctx_.content;
    gfx_fill(S, 0, kit::kBelowBar, S.w, static_cast<int16_t>(S.h - kit::kBelowBar), kBg);
    vpp_f_.invalidate();
    freq_f_.invalidate();
    vrms_f_.invalidate();
    frame_ok_ = false;
    chip_sig_ = 0;
    foot_elem_.invalidate();
    for (ripple::ColSpan &c : shown_) c = {kNoCol, kNoCol};
    plot_col_ = 0;
    plot_dirty_ = true;
}

void RipplePage::take_sample(uint32_t now_ms) {
    last_cap_ms_ = now_ms;
    cap_due_ = false;
    if (scratch::held()) {
        adc_busy_ = true;
        return;
    }
    auto &cfg = ctx_.keep.ripple;
    const uint8_t tb = cfg.timebase < ripple::kTimebaseCount ? cfg.timebase : ripple::kDefaultTimebase;
    const uint8_t sc = cfg.scale < ripple::kScaleCount ? cfg.scale : 0;
    const auto trig = static_cast<ripple::Trigger>(cfg.trigger & 1u);

    using Buf = std::array<uint16_t, adc::kRippleSamples>;
    scratch::acquire("ripple_ui");
    Buf &buf = scratch::as<Buf>();
    const uint32_t period_ns = adc::capture_ripple(buf, ripple::kTimebases[tb].sample_code);
    if (period_ns == 0) {
        scratch::release();
        adc_busy_ = true;
        return;
    }
    adc_busy_ = false;
    const uint16_t vref = ctx_.analog ? ctx_.analog->vref_raw : 0;
    stats_ = ripple::analyze(buf, period_ns, vref, tb, trig);
    const int32_t peak_mv = std::max(stats_.max_ac_mv, -stats_.min_ac_mv);
    mv_per_div_ = ripple::resolve_mv_per_div(sc, peak_mv);
    ripple::project(buf, period_ns, vref, stats_, tb, mv_per_div_, static_cast<uint8_t>(kPlotH), cols_);
    scratch::release();
    have_sample_ = true;
    plot_col_ = 0;
    plot_dirty_ = true;
}

void RipplePage::draw_chips() {
    const auto &cfg = ctx_.keep.ripple;
    const uint8_t tb = cfg.timebase < ripple::kTimebaseCount ? cfg.timebase : ripple::kDefaultTimebase;
    const uint8_t sc = cfg.scale < ripple::kScaleCount ? cfg.scale : 0;
    const uint32_t sig = hash_chips(tb, sc, cfg.hold);
    if (sig == chip_sig_) return;

    const Surface &S = ctx_.content;
    if (!budget_ok(ctx_.budget, 128 * 16, true)) return;
    gfx_fill(S, 92, static_cast<int16_t>(kRow2Y - 1), static_cast<int16_t>(S.w - 92), 16, kBg);

    int16_t x = static_cast<int16_t>(S.w - 6);
    auto place_right = [&](const char *txt, uint16_t fill, uint16_t ink, bool framed) {
        const int16_t w = static_cast<int16_t>(std::string_view(txt).size() * kLabelFont.adv + 8);
        x = static_cast<int16_t>(x - w);
        if (framed) kit::draw_chip_frame(S, x, static_cast<int16_t>(kRow2Y - 1), txt, fill, ink, kBg);
        else kit::draw_chip(S, x, static_cast<int16_t>(kRow2Y - 1), txt, fill, ink);
        x = static_cast<int16_t>(x - 4);
    };
    if (cfg.hold) place_right("HOLD", kCaution, kBg, false);
    place_right(ripple::kScaleLabel[sc], kRule, kDim, true);
    place_right(ripple::kTimebases[tb].label, kRule, kAccent, true);
    chip_sig_ = sig;
}

void RipplePage::draw_col(const Surface &S, int16_t x, ripple::ColSpan col, uint16_t trace_ink) {
    const int16_t sx = static_cast<int16_t>(kPlotX + x);
    gfx_fill(S, sx, kPlotY, 1, kPlotH, kBg);
    if (x == kGridQ1X || x == kGridMidX || x == kGridQ3X) {
        for (int16_t y = 0; y < kPlotH; y = static_cast<int16_t>(y + 4))
            gfx_fill(S, sx, static_cast<int16_t>(kPlotY + y), 1, 1, kRule);
    } else {
        if ((x & 1) == 0) gfx_fill(S, sx, static_cast<int16_t>(kPlotY + kGridMidY), 1, 1, kRule);
        if ((x & 3) == 0) {
            gfx_fill(S, sx, static_cast<int16_t>(kPlotY + kGridQ1Y), 1, 1, kRule);
            gfx_fill(S, sx, static_cast<int16_t>(kPlotY + kGridQ3Y), 1, 1, kRule);
        }
    }
    if (col.lo <= col.hi && col.hi < kPlotH) {
        const int16_t h = static_cast<int16_t>(col.hi - col.lo + 1);
        gfx_fill(S, sx, static_cast<int16_t>(kPlotY + col.lo), 1, h, trace_ink);
    }
}

bool RipplePage::paint_plot() {
    const Surface &S = ctx_.content;
    if (!frame_ok_) {
        if (!budget_ok(ctx_.budget, kFrameW * 2 + kFrameH * 2, true)) return false;
        draw_frame(S, kFrameX, kFrameY, kFrameW, kFrameH, 1, kRule);
        frame_ok_ = true;
    }
    if (!plot_dirty_) return true;

    const bool hold = ctx_.keep.ripple.hold;
    const uint16_t trace_ink = !have_sample_ ? kRule : hold ? kStale : stats_.below_floor ? kDim : kOk;
    for (int16_t x = plot_col_; x < kPlotW; x++) {
        const ripple::ColSpan want = have_sample_ ? cols_[x]
                                                  : ripple::ColSpan{static_cast<uint8_t>(kPlotH / 2),
                                                                    static_cast<uint8_t>(kPlotH / 2)};
        if (shown_[x].lo == want.lo && shown_[x].hi == want.hi) continue;
        if (!budget_ok(ctx_.budget, kPlotH, true)) {
            plot_col_ = static_cast<uint8_t>(x);
            return false;
        }
        draw_col(S, x, want, trace_ink);
        shown_[x] = want;
    }
    plot_col_ = 0;
    plot_dirty_ = false;
    return true;
}

void RipplePage::paint_footer() {
    const auto &cfg = ctx_.keep.ripple;
    const uint8_t tb = cfg.timebase < ripple::kTimebaseCount ? cfg.timebase : ripple::kDefaultTimebase;
    StrBuf<40> foot;
    if (settings_open_) {
        foot.put("PA1 AC-coupled");
    } else if (adc_busy_ && !have_sample_) {
        foot.put("ADC busy");
    } else {
        foot.put_u32(mv_per_div_).put("mV/d ").put(ripple::kTimebases[tb].label).put("/d ");
        foot.put(cfg.trigger == static_cast<uint8_t>(ripple::Trigger::Free) ? "Free" : "Trig");
    }
    draw_footer_budgeted(ctx_.content, FooterTone::Info, foot.c_str(), foot_elem_, ctx_.budget);
}

void RipplePage::update(uint32_t now_ms) {
    // Boost SPI3 to max (60 MHz, /2) while Ripple is active; skipped automatically during screenshot streams.
    if (saved_spi_div_ == 0) {
        const unsigned cur = lcd::spi_div();
        if (cur > 2 && lcd::set_spi_div(2)) saved_spi_div_ = static_cast<uint8_t>(cur);
    }
    title_badge(ctx_.stale());
    if (settings_open_) {
        ctx_.rows.set_count(4);
        ctx_.rows.select(settings_sel_);
        ctx_.rows.draw(now_ms, ctx_.budget);
        paint_footer();
        return;
    }

    const auto &cfg = ctx_.keep.ripple;
    if (!cfg.hold && (cap_due_ || !have_sample_ || now_ms - last_cap_ms_ >= kRefreshMs)) {
        take_sample(now_ms);
    }

    StrBuf<16> vpp_s, freq_s, vrms_s;
    if (have_sample_) {
        ripple::fmt_vpp(stats_, vpp_s);
        ripple::fmt_freq(stats_, freq_s);
        ripple::fmt_vrms(stats_, vrms_s);
    } else {
        vpp_s.put("--");
        freq_s.put("--");
        vrms_s.put("--");
    }
    const uint16_t vpp_ink = !have_sample_ || stats_.below_floor ? kDim : cfg.hold ? kStale : kText;
    const uint16_t freq_ink = !have_sample_ || stats_.below_floor || stats_.freq_hz == 0 ? kDim : cfg.hold ? kStale : kAccent;
    if (vpp_ink != vpp_ink_) { vpp_ink_ = vpp_ink; vpp_f_.set_colors(vpp_ink, kBg); vpp_f_.invalidate(); }
    vpp_f_.draw(vpp_s.c_str());
    if (freq_ink != freq_ink_) { freq_ink_ = freq_ink; freq_f_.set_colors(freq_ink, kBg); freq_f_.invalidate(); }
    freq_f_.draw(freq_s.c_str());
    vrms_f_.draw(vrms_s.c_str());
    draw_chips();
    if (!paint_plot()) return;
    paint_footer();
}

bool RipplePage::on_event(Event e) {
    auto &cfg = ctx_.keep.ripple;
    if (settings_open_) {
        if (e.press != Press::Short) return false;
        if (e.button == Button::Back) {
            exit_settings();
            return true;
        }
        if (e.button == Button::Minus) {
            settings_sel_ = settings_sel_ > 0 ? static_cast<uint8_t>(settings_sel_ - 1) : 3;
            return true;
        }
        if (e.button == Button::Plus) {
            settings_sel_ = settings_sel_ < 3 ? static_cast<uint8_t>(settings_sel_ + 1) : 0;
            return true;
        }
        if (e.button == Button::Ok) {
            switch (settings_sel_) {
            case 0: cfg.hold = !cfg.hold; break;
            case 1: cfg.timebase = static_cast<uint8_t>((cfg.timebase + 1) % ripple::kTimebaseCount); break;
            case 2: cfg.scale = static_cast<uint8_t>((cfg.scale + 1) % ripple::kScaleCount); break;
            case 3: cfg.trigger = static_cast<uint8_t>((cfg.trigger + 1) & 1u); break;
            }
            if (!cfg.hold) cap_due_ = true;
            ctx_.rows.invalidate();
            return true;
        }
        return false;
    }

    if (e.button == Button::Ok && e.press == Press::Short) {
        enter_settings();
        return true;
    }
    if (e.button == Button::Ok && e.press == Press::Long) {
        cfg.hold = !cfg.hold;
        chip_sig_ = 0;
        foot_elem_.invalidate();
        for (ripple::ColSpan &c : shown_) c = {kNoCol, kNoCol};
        plot_col_ = 0;
        plot_dirty_ = true;
        if (!cfg.hold) cap_due_ = true;
        return true;
    }
    return back_event(e);
}

Strip RipplePage::labels() const {
    if (settings_open_) {
        Strip l = list_strip(nav_strip());
        l[Button::Ok] = sl::Cycle;
        return l;
    }
    Strip l = base_labels();
    l[Button::Ok] = ctx_.keep.ripple.hold ? sl::SetRun : sl::SetHold;
    return l;
}

}  // namespace k2
