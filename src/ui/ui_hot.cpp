// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Hot per-tick and slice-paint Ui methods (kept in the zero-wait flash window; cold Ui methods live in ui.cpp).
#include "k2/ui/ui.hpp"
#include "k2/ui/home_cards.hpp"
#include "k2/ui/home_model.hpp"

#include <algorithm>

#include "k2/battery/battery_hw.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/hot.h"
#include "k2/core/log.hpp"
#include "k2/core/loop_guard.hpp"
#include "k2/core/paint_meter.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/fastcharge/svooc_trig.hpp"
#if K2_LOOPPROBE
#include "k2/loopprobe/loop_probe_hw.hpp"
#endif
#include "k2/pd/link.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_source.hpp"
#if K2_TWIN
#include "k2/twin/twin_hw.hpp"
#endif
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_viewport.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

namespace {
uint32_t now_us() { return systick::micros(); }
}  // namespace

K2_HOT void Ui::pump_paint(uint32_t now_ms) {
    if (paint_off_) return;   // the screen is off: the pending state stays, set_paint_enabled(true) repaints from scratch
    PaintBudget *const bg = ctx_.budget;
    if (clear_pending_) {
        if (clear_full_lcd_) {
            ContentViewport::Pause pause;
            while (clear_y_ < clear_y1_) {
                const int rows = std::min<int>(PaintBudget::kStripRows, clear_y1_ - clear_y_);
                const int cost = static_cast<int>(lcd::kWidth) * rows;
                if (bg && !bg->admit(cost, true, true)) { paint_pending_ = true; return; }   // a solid fill: wire time only
                lcd::clear_strip(static_cast<uint16_t>(clear_y_), static_cast<uint16_t>(rows), kBg);
                clear_y_ = static_cast<int16_t>(clear_y_ + rows);
            }
        } else {
            ContentViewport viewport(layout_.content);
            while (clear_y_ < clear_y1_) {
                const int rows = std::min<int>(PaintBudget::kStripRows, clear_y1_ - clear_y_);
                const int cost = static_cast<int>(ctx_.content.w) * rows;
                if (bg && !bg->admit(cost, true, true)) { paint_pending_ = true; return; }
                gfx_fill(ctx_.content, 0, clear_y_, ctx_.content.w, static_cast<int16_t>(rows), kBg);
                clear_y_ = static_cast<int16_t>(clear_y_ + rows);
            }
        }
        clear_pending_ = false;
        clear_full_lcd_ = false;
    }

    ContentViewport viewport(layout_.content);
    ctx_.blank = paint_blank_;
    if (chrome_gaps_pending_) {
        const int h1 = kit::kBodyY - kBarH;
        const int h2 = kFooterY - kit::kListBottom;
        const int cost = static_cast<int>(ctx_.content.w) * (h1 + h2);
        if (bg && !bg->admit(cost, true, true)) { paint_pending_ = true; return; }
        gfx_fill(ctx_.content, 0, kBarH, ctx_.content.w, static_cast<int16_t>(h1), kBg);
        gfx_fill(ctx_.content, 0, kit::kListBottom, ctx_.content.w, static_cast<int16_t>(h2), kBg);
        chrome_gaps_pending_ = false;
    }
    if (static_pending_) {
        current().draw_static();
        if (bg && bg->more) { paint_pending_ = true; return; }
        static_pending_ = false;
    }
    if (panel_pending_) {
        for (unsigned i = 0; i < 4; i++) {
            if (!(panel_mask_ & (1u << i))) continue;
            if (!current().hides_strip() && layout_.slot[i].w > 0 && layout_.slot[i].h > 0) {
                const int cost = static_cast<int>(layout_.slot[i].w) * layout_.slot[i].h;
                if (bg && !bg->admit(cost, true)) { paint_pending_ = true; return; }
            }
            draw_slot(i);
            panel_mask_ &= static_cast<uint8_t>(~(1u << i));
        }
        panel_pending_ = false;
    }
    current().update(now_ms);
    if (bg && bg->more) { paint_pending_ = true; return; }

    ctx_.blank = false;
    paint_blank_ = false;
    paint_pending_ = false;
    draw_release_notice();
    draw_rotation_notice(now_ms);
    sync_panel();
    if (timing_in_flight_) {
        timing_in_flight_ = false;
        full_px_ = static_cast<uint32_t>(paint::meter.px_count() - paint_px0_);
        full_us_ = now_us() - paint_t0_us_;
        field_max_us_ = 0;
        page_since_ms_ = now_ms;
        timing_pending_ = true;
    }
}

K2_HOT void Ui::draw_slot(unsigned slot) {
    ContentViewport::Pause unscaled_labels;
    const Button b = slot_button(orient_, slot);
    const Button action = oriented_button(b, orient_.rotation, rotate_buttons_);
    const SlotLabel lab = slot_label(current().labels()[action]);
    const bool down = pressed_[static_cast<int>(b)];
    slot_sig_[slot] = slot_sig(slot);
    if (current().hides_strip()) return;   // a full-panel page (the QR code) owns the strip's pixels
    const Rect &r = layout_.slot[slot];
    if (r.w <= 0 || r.h <= 0) return;

    kit::draw_key_slot(surface_of(r), panel_is_column(orient_.edge), panel_text_dir(orient_.edge), slot, lab,
                       action == Button::Ok, down);
}

// What the current page shows in this slot: its pool id (the pool holds each distinct text once), offset so it is never 0.
K2_HOT uint32_t Ui::slot_sig(unsigned slot) {
    const Button physical = slot_button(orient_, slot);
    return 0x100u | current().labels()[oriented_button(physical, orient_.rotation, rotate_buttons_)];
}

K2_HOT void Ui::repaint(uint32_t now_ms) {
    BudgetScope scope(*this);
    hold_shown_ = false;
    // An opaque page paints every pixel of the content and the strip every pixel of its slots (the switch into one skips the
    // erase for the same reason, test_ui_capture checks both pixel for pixel): the full-screen clear would only be overdraw.
    if (depth_ == 0 ? page_opaque(page_) : menu_opaque(menu())) {
        rotation_notice_drawn_ = false;
        paint_incoming(now_ms, now_us(), false);
        return;
    }
    start_full_clear();
    redraw(now_ms);
}

void Ui::set_paint_enabled(bool on, uint32_t now_ms) {
    if (paint_off_ != on) return;   // already in that state
    paint_off_ = !on;
    if (on) repaint(now_ms);
}

bool Ui::alert_active() const {
    return alert_over_ || fc_release_.state() == trig::Release::State::Unsafe ||
           charger_test_.results().state() == charger_test::State::Unsafe || footer_error_shown();
}

K2_HOT UiAction Ui::tick(uint32_t now_ms) {
    BudgetScope scope(*this);
    gap::ui.note(now_ms);
    if (requested_rotation_ < 4) {
        const uint8_t target = requested_rotation_;
        requested_rotation_ = 0xFF;
        set_rotation(target, now_ms);
    }
    ContentViewport viewport(layout_.content);
    if (!down_[static_cast<int>(Button::Minus)] && !down_[static_cast<int>(Button::Plus)]) rotation_chord_ = false;
    ctx_.now = now_ms;
    for (int k = 0; k < 4; k++)   // end a press flash that outlived its press
        if (pressed_[k] && !held(k)) update_lit(k, true, now_ms);
    const auto pm = power::read_display();
    ctx_.pm_online = pm.has_value();
    ctx_.pm = pm.value_or(power::Metrics{});
    ctx_.pm_avg = power::read().value_or(power::Metrics{});
    ctx_.vbus_fast_mv = power::vbus_fast_mv().value_or(ctx_.pm.vbus_mv);
    const auto previous_release = fc_release_.state();
    if (TriggerPage *trigger = find<TriggerPage>(Menu::Trigger)) trigger->poll();   // no page, no session: leave() ended it
#if K2_LOOPPROBE
    if (LoopProbePage *probe = find<LoopProbePage>(Menu::LoopProbe)) probe->poll(now_ms);   // the same: leaving the page ends a run
#endif
    const auto fs = fctrig::status().state;
    const auto &sk = fusb302::sink_status();
    const bool released = fs == fc::State::Idle && !fctrig::scan_running() && !fctrig::svooc_running() &&
        !sk.held && !sk.epr_mode &&
        (sk.state == pd::SinkState::Idle || sk.state == pd::SinkState::Failed ||
         (sk.state == pd::SinkState::Ready && sk.mv == 5000));
    fc_release_.poll(now_ms, ctx_.pm_online ? ctx_.vbus_fast_mv : -1, released);
    if (previous_release != fc_release_.state() &&
        (fc_release_.state() == trig::Release::State::Safe || fc_release_.state() == trig::Release::State::Unsafe)) {
        current().invalidate();
        paint_pending_ = true;
    }
    const bool live_now = ctx_.pm_online && ctx_.pm.vbus_mv >= 1000;
    if (live_now && !extremes_live_) {   // an attach: Home's peaks, sag and card cycle start over (they stay frozen while stale)
        ctx_.extremes.reset();
        ctx_.keep.cards.reset();
        ctx_.keep.attention.reset();
        sag_settle_.reset();
    }
    if (ctx_.pm_online) {
        // energy: the unfiltered sample, so smoothing never changes the Wh total
        const power::Metrics raw = power::read_raw().value_or(ctx_.pm);
        ctx_.meter.update(now_ms, raw.power_mw, raw.current_ma);
        if (ctx_.pm.vbus_mv >= 1000) {
            ctx_.trend.push(now_ms, static_cast<uint32_t>(ctx_.pm.vbus_mv) / 10, power::magnitude(ctx_.pm.current_ma) / 10,
                            power::magnitude(ctx_.pm.power_mw) / 10, ctx_.limit_w, ctx_.meter.mwh());
            // Home's PEAK and SAG cards: every raw sample counts (a short spike the display filter rounds away is a peak).
            ctx_.extremes.sample(raw.vbus_mv, raw.current_ma, raw.power_mw, ctx_.sag_level_mv);
        }   // the trace freezes when stale
        if (ctx_.cable_mode == cable_r::Mode::Passive) ctx_.cable.sample(ctx_.pm_avg.vbus_mv, ctx_.pm_avg.current_ma);
    } else {
        ctx_.cable.reset();
    }
    ctx_.analog = adc::read();
    ctx_.plus_key = down_[static_cast<int>(Button::Plus)];   // the physical key (remote presses never set down_): the Waiting stage logs while it is held
    {   // remember the last live reading for the stale state (Home and the LAST pill)
        const bool live = ctx_.pm_online && ctx_.pm.vbus_mv >= 1000;
        ctx_.live.note(now_ms, live);
        if (live) {
            // The LAST snapshot takes a reading only once it has held for a window (home::steady_snapshot): the tail of a
            // session (a released contract, VBUS falling through the display filter) is never what stays on screen.
            const bool attach = !extremes_live_;
            if (attach || now_ms - snap_ms_ >= home::kSnapshotMs) {
                const UiContext::Frozen cur{ctx_.pm, ctx_.analog, ctx_.meter.elapsed_s(now_ms), proto_now()};
                if (attach || home::steady_snapshot(snap_prev_.pm.vbus_mv, cur.pm.vbus_mv, snap_prev_.proto, cur.proto))
                    ctx_.last = cur;   // on attach at once, so even a short session has a snapshot
                snap_prev_ = cur;
                snap_ms_ = now_ms;
            }
            ctx_.last.elapsed_s = ctx_.meter.elapsed_s(now_ms);   // the session's length runs to its last live tick
        }
        extremes_live_ = live;
        if (live && now_ms - sag_level_ms_ >= 100) {   // the level a sag is measured against (home::sag_level_mv), settled
            sag_level_ms_ = now_ms;
            const fctrig::Status fst = fctrig::status();
            const bool qc3 = fst.state == fctrig::State::Active && (fst.proto == fctrig::Proto::Qc3 || fst.proto == fctrig::Proto::Qc3Plus);
            const home::ProtoInfo proto = proto_now();
            const uint32_t level = home::sag_level_mv(proto, qc3 && fst.target_mv > 0 ? static_cast<uint32_t>(fst.target_mv) : 0u);
            ctx_.sag_level_mv = sag_settle_.update(now_ms, level);
            ctx_.limit_w = (graphs::limit_mw(proto) + 500u) / 1000u;   // the Graphs page's dotted limit, per trend column
            // The alarm the idle logic holds the screen for: Home's OVER on W or A (the same rule, home::alarm_of).
            alert_over_ = home::alarm_of(static_cast<uint32_t>(power::magnitude(ctx_.pm.power_mw)), home::contract_mw(proto)) == home::Alarm::Over ||
                          home::alarm_of(static_cast<uint32_t>(power::magnitude(ctx_.pm.current_ma)), proto.ma) == home::Alarm::Over;
        } else if (!live) {
            ctx_.sag_level_mv = 0;
            ctx_.limit_w = 0;
            alert_over_ = false;
        }
    }
    // Kelvin mode: the far-end sense wires of the fixture arrive on CC1, CC2 and D-.
    ctx_.kelvin.reset();
    if (ctx_.cable_mode == cable_r::Mode::Kelvin && ctx_.pm_online && ctx_.analog)
        ctx_.kelvin = cable_r::kelvin(ctx_.analog->cc1_mv, ctx_.analog->cc2_mv, ctx_.analog->dm_mv, ctx_.pm_avg.current_ma);
    ctx_.fc = fastcharge::read();
    ctx_.vooc = fastcharge::vooc_live();
    {   // hard cap for requests and the measured VBUS for the engine's over-voltage abort
        const power::Chip chip = power::chip();
        ctx_.cap_mv = pd::request_cap_mv(power::max_request_mv(chip), power::max_vbus_mv(chip));
        fusb302::sink_set_cap(ctx_.cap_mv);
        if (ctx_.pm_online) fusb302::sink_vbus(ctx_.vbus_fast_mv);
    }

    pd::LinkInput in;
    in.now_ms = now_ms;
    in.vbus_known = ctx_.pm_online;
    in.vbus_mv = ctx_.vbus_fast_mv;
    in.current_known = ctx_.pm_online;
    in.current_ma = ctx_.pm_avg.current_ma;
    in.cc = fusb302::active_cc();
    in.caps_rx = fusb302::rx_count();
    in.requests = fusb302::contract().requests;
    // The PD verdict's inputs (ROADMAP 2026-11-10, Row C): any PD message decoded this attach, and whether the
    // sink side answered anything (a GoodCRC / Request / caps / VDM from the far side this attach).
    in.any_pd_msg = fusb302::pd_seen_attach();
    in.sink_answered = fusb302::over_load_facts().far_pd_message;
    in.self_supply_ma = usb::configured() ? 0 : pd::kSelfSupplyAllowanceMa;   // no USB host: the K2 may run on this VBUS
    // The waiting classifier's input: the passive CC levels and the same passive gate the connection page uses.
    in.cc1_mv = ctx_.analog ? ctx_.analog->cc1_mv : -1;
    in.cc2_mv = ctx_.analog ? ctx_.analog->cc2_mv : -1;
    in.passive = !fusb302::rd_for_run_on() && !fusb302::ra_active() && !fusb302::self_powered() && !link_.inline_mode();
    in.line_held = fusb302::idle_wake_line() != 0 || fusb302::pin_rd() != 0;   // a hold of ours (Wake sleeping port, boot-pinned Rd): not the far ports' own levels
    {   // VBUS this K2 drives itself: the PD trigger (held or mid-negotiation), a fast-charge trigger, or the SuperVOOC trigger
        // (its exit watch included). The SuperVOOC adapter raises VBUS on a PD-silent link with no contract of the K2's own: without
        // this the ramp reads as a sink behind the K2 (cause Rise), the Rd goes off and the adapter drops VBUS (hardware 2026-11-15).
        const pd::SinkStatus &sk = fusb302::sink_status();
        const bool pd_busy = sk.held || (sk.state != pd::SinkState::Idle && sk.state != pd::SinkState::Failed);
        const fctrig::State fs = fctrig::status().state;
        in.own_contract = pd_busy || fs == fctrig::State::Negotiating || fs == fctrig::State::Active || svooc_trig::running();
    }
    if (pd::is_fake()) link_.force_attached(now_ms);   // fake PDOs count as attached
    const bool was_attached = link_.attached();
    link_.update(in);
    if (link_.cc_cross() != cross_seen_) {   // one log line per edge of the crossed-CC verdict (Source tab, bar, Home and the remote state read it)
        cross_seen_ = link_.cc_cross();
        K2LOG("[LINK] cc cross=%s cc1=%u cc2=%u\n", cross_seen_ == raguard::CrossKind::Crossed ? "crossed" : cross_seen_ == raguard::CrossKind::Hint ? "hint" : "none",
              unsigned(in.cc1_mv < 0 ? 0 : in.cc1_mv), unsigned(in.cc2_mv < 0 ? 0 : in.cc2_mv));
    }
    if (link_.pd_verdict() != pd_verdict_seen_) {   // one log line per edge of the "PD present" verdict (Row C; %s is cut at 12 bytes)
        pd_verdict_seen_ = link_.pd_verdict();
        K2LOG("[LINK] pd verdict=%s caps=%u\n", pd::pd_verdict_token(link_.pd_verdict(), link_.pd_none_why()),
              unsigned(link_.caps_rx_this_attach()));
    }
    if (was_attached && !link_.attached()) fusb302::clear_source();   // one attach = one list
    if (link_.take_new_source()) fusb302::new_source();
    if (fusb302::cable_seq() != cable_seq_seen_) {   // the source's own SOP' exchange was decoded
        cable_seq_seen_ = fusb302::cable_seq();
        ctx_.em_scanned = true;
        ctx_.em_listen_until = 0;
        ctx_.em_quiet = false;
        link_.emarker_scanned(now_ms);
    } else if (ctx_.em_listen_until && static_cast<int32_t>(now_ms - ctx_.em_listen_until) >= 0) {
        ctx_.em_listen_until = 0;   // the window is over and the cable never answered
        ctx_.em_quiet = true;
        K2LOG("[UI] e-marker listen window over: nothing heard, replug the charger end\n");
    }
    fusb302::set_inline(link_.inline_mode());
    fusb302::set_wait_kind(link_.wait_kind());   // the alone-Identify decision reads this (pd::identify_alone)
    if (ctx_.analog) fusb302::sink_cc(ctx_.analog->cc1_mv, ctx_.analog->cc2_mv);
    link::set_inline_mode(link_.inline_mode(), link_.inline_cause());
    ctx_.inline_mode = link_.inline_mode();
    charger_test_.poll(now_ms);
#if K2_TWIN
    twin_hw::live_poll(twin_ctx(now_ms));   // the twin proof ends itself under the loop lock (its busy input, twin_hw.cpp)
    twin_hw::inline_poll();                 // an inline interrogation that ended: learn from it once
#endif
#if K2_PDSTRESS
    if (loopguard::locked() && pdstress_.running()) pdstress_.cancel();   // the HID-cable loop 5 V lock: released to 5 V at this poll
    pdstress_.poll(now_ms);   // after the charger test's poll: that one clears the run-owner flag a run of ours has set
    const bool stress_note = pdstress_.footer() != nullptr || loopguard::locked();   // a footer note: the run's line or the loop lock
#else
    const bool stress_note = loopguard::locked();
#endif
#if K2_PDSPEC
    pdspec_.poll(now_ms);   // after the charger test's poll, like PD Stress (it sets the run-owner flag while it runs)
    const bool spec_note = pdspec_.footer() != nullptr;
#else
    const bool spec_note = false;
#endif
    if (pdstress_note_ && !(stress_note || spec_note)) { current().invalidate(); paint_pending_ = true; }   // the note goes: the page repaints its own footer
    pdstress_note_ = stress_note || spec_note;
    // The battery recorder's running dot (draw_release_notice): when the run stops, the page repaints whole once, so whatever
    // the dot covered (a bar, a hero) comes back.
    const bool batt_run = battery::hw::service().rec().active();
    if (batt_dot_ && !batt_run && !hold_shown_ && menu() != Menu::Share && menu() != Menu::QrTest) repaint(now_ms);
    batt_dot_ = batt_run;
    if (ctx_.trig_armed && now_ms >= trig_until_) ctx_.trig_armed = false;

    const RebootChord::Step chord = chord_.step(now_ms);
    if (chord == RebootChord::Step::Reboot) return UiAction::Reboot;
    // If the hold card was active and the chord was released, dismiss and restart paint now. While a sliced paint is still
    // pending (e.g. a page switch while the chord stays held), finish pump_paint so ui.painting() always drains cleanly,
    // and redraw the hold card on top afterward so no background slice punches a hole through it.
    if (hold_shown_ && chord == RebootChord::Step::None) update_hold_card(now_ms, chord);
    if (!hold_shown_ || paint_pending_) {
        const bool period = !hold_shown_ && (now_ms - last_update_ms_ >= kPageTickMs);
        if (period || paint_pending_) {
            const uint32_t t0 = now_us();
            if (period) {
                last_update_ms_ = now_ms;
                current().sample();
            }
            pump_paint(now_ms);
            if (!paint_pending_ && rotation_notice_until_ && static_cast<int32_t>(now_ms - rotation_notice_until_) >= 0) {
                rotation_notice_until_ = 0;
                notice_ = nullptr;
                current().invalidate();
                paint_pending_ = true;
                pump_paint(now_ms);
            }
            if (hold_shown_) hold_shown_ = false;
            const uint32_t dt = now_us() - t0;
            if (dt > field_max_us_) field_max_us_ = dt;
        }
    }
    if (chord != RebootChord::Step::None) update_hold_card(now_ms, chord);
    if (timing_pending_) log_timing(now_ms);
    return UiAction::None;
}

}  // namespace k2
