// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The idle glue (k2/core/idle_hw.hpp): inputs from the live firmware into the idle machine, its outputs onto the backlight,
// the panel, the UI paint gate and the AHB divider.
#include "k2/core/idle_hw.hpp"

#include "k2/core/idle_cfg.hpp"
#include "k2/core/log.hpp"
#include "k2/core/stage_meter.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/crm.hpp"
#include "k2/drivers/dpdm.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/load.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/uart2.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/fastcharge/svooc_trig.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/ui/ui.hpp"

namespace k2::idle_hw {

namespace {

constexpr std::uint32_t kPassGuardMs = 100;   // PD or VBUS activity this recent pulls a low clock back up

idle::Machine g_m;
bool g_asleep;            // we sent the panel to sleep and it has not been fully woken and repainted yet
bool g_waking;            // the wake sequence is running (lcd::panel_poll drives it)
bool g_wait_paint;        // woken and repainting: the backlight comes up when the repaint is done
std::uint8_t g_force_div; // 0 = automatic
std::uint32_t g_hold_until_ms;
bool g_hold;
std::uint32_t g_last_tick_ms;
bool g_tick_seen;
idle::Config g_saved;      // what /idle.cfg holds (or the defaults when it was missing)
bool g_dirty;              // the persisted part changed since g_saved
std::uint32_t g_dirty_ms;  // when it last changed

bool before(std::uint32_t t, std::uint32_t now) { return static_cast<std::int32_t>(now - t) < 0; }

std::uint8_t blocked_mask(std::uint32_t now) {
    std::uint8_t m = 0;
    if (load::active()) m |= kBlockLoad;
    if (uart2::claimed()) m |= kBlockUart2;
    if (dpsniff::armed()) m |= kBlockSniff;
    if (dpsniff::monitoring()) m |= kBlockMonitor;
    if (fctrig::svooc_running() || fctrig::scan_running() || fctrig::ufcs_probe_running() || fctrig::ufcs_exiting() ||
        svooc_trig::running())
        m |= kBlockFc;
    if (g_hold && before(now, g_hold_until_ms)) m |= kBlockHold;
    return m;
}

// The divider changes in one place: the stage meter's cycle windows restart (its microseconds assume 240 cycles each).
void apply_div(std::uint8_t n) {
    if (crm::hclk_div() == n) return;
    crm::set_hclk_div(n);
    stage::meter.reset(dpdm::cycles(), systick::millis());
}

void full_clock() {
    if (g_force_div == 0) apply_div(1);
}

// The sink engine is mid-negotiation: Requesting, Transition, an EPR entry or an Abort (Idle, Ready and Failed are quiet).
bool sink_in_flight() {
    const pd::SinkState s = fusb302::sink_status().state;
    return s != pd::SinkState::Idle && s != pd::SinkState::Ready && s != pd::SinkState::Failed;
}

}  // namespace

void init() {}

void load_config() {
    idle::Config c = g_m.config();
    const idle::Config s = idle_cfg::load(fs::instance());
    c.brightness = s.brightness;
    c.dim_brightness = s.dim_brightness;
    c.dim_after_s = s.dim_after_s;
    c.off_after_s = s.off_after_s;
    g_m.set_config(c);
    g_saved = g_m.config();
    g_dirty = false;
    K2LOG("[IDLE] settings bright=%u dim=%u dim_after=%u off_after=%u\n", unsigned(g_saved.brightness), unsigned(g_saved.dim_brightness),
          unsigned(g_saved.dim_after_s), unsigned(g_saved.off_after_s));
}

const idle::Config& config() { return g_m.config(); }

void set_config(const idle::Config& c) {
    g_m.set_config(c);
    g_dirty = !idle_cfg::same(idle_cfg::persisted(g_m.config(), g_saved), g_saved);
    if (g_dirty) g_dirty_ms = systick::millis();
}

void force(idle::Force f) { g_m.force(f); }

void force_div(std::uint8_t n) {
    if (n != 0 && !crm::hclk_div_valid(n)) return;
    g_force_div = n;
}

// A button wakes the machine and the clock goes up here, not at the next 20 ms service pass.
bool press(std::uint32_t now) {
    const bool deliver = g_m.press(now);
    full_clock();
    return deliver;
}

void touch(std::uint32_t now) {
    g_m.touch(now);
    full_clock();
}

void request(bool state_changing, std::uint32_t now) {
    if (!state_changing) return;
    g_m.remote_activity(now);
    g_hold = true;
    g_hold_until_ms = now + kClockHoldMs;
    full_clock();
}

void pass_guard() {
    if (crm::hclk_div() <= 1 || g_force_div != 0) return;
    if (pdtrace::since_activity_ms() < kPassGuardMs || sink_in_flight()) full_clock();
}

Status status() {
    Status s;
    s.state = g_m.state();
    s.force = g_m.force_mode();
    s.brightness_now = lcd::brightness();
    s.panel = g_waking ? 2 : g_asleep ? 1 : 0;
    s.hclk_div = crm::hclk_div();
    s.force_div = g_force_div;
    s.blocked = blocked_mask(systick::millis());
    s.idle_s = g_m.idle_s();
    s.cfg = g_m.config();
    return s;
}

void service(Ui& ui, bool boot_screen, std::uint32_t now) {
    const bool ripple_boost = ui.page() == Page::Ripple && ui.menu_depth() == 0;
    idle::Inputs in;
    in.now_ms = now;
    in.busy = boot_screen || ui.ports_busy() || ripple_boost;
    in.alarm = ui.alert_active();
    // A PD message or VBUS move after the last tick: its age is shorter than the time since that tick.
    const std::uint32_t since = pdtrace::since_activity_ms();
    in.pd_negotiating = idle::pd_settling(sink_in_flight(), since);
    in.pd_event = g_tick_seen && since < now - g_last_tick_ms;
    g_last_tick_ms = now;
    g_tick_seen = true;
    if (const auto v = power::vbus_fast_mv()) {
        in.vbus_known = true;
        in.vbus_mv = *v;
    }
    if (const auto pm = power::read()) {
        if (!in.vbus_known) {
            in.vbus_known = true;
            in.vbus_mv = pm->vbus_mv;
        }
        in.ibus_ma = pm->current_ma;
    }
    const std::uint8_t blocked = blocked_mask(now);
    const idle::Output o = g_m.tick(in);

    // The panel: off stops the paint first, then sleeps; on wakes, repaints, and only then the backlight comes up.
    if (!o.panel_on) {
        if (!g_asleep) {
            ui.set_paint_enabled(false, now);
            lcd::panel_sleep(true);
            g_asleep = true;
        }
        g_waking = false;
        g_wait_paint = false;
    } else if (g_asleep && !g_waking) {
        apply_div(1);
        lcd::panel_sleep(false);
        g_waking = true;
    }
    lcd::panel_poll();
    if (g_waking && lcd::panel_awake()) {
        ui.set_paint_enabled(true, now);
        g_waking = false;
        g_asleep = false;
        g_wait_paint = true;
    }
    if (g_wait_paint && !ui.painting()) g_wait_paint = false;
    const std::uint8_t target_bright = ripple_boost ? 100u : o.brightness;
    if (idle::backlight_write_due(!g_asleep && !g_waking && !g_wait_paint && !ui.loop_probe_busy(), lcd::brightness(), target_bright))
        lcd::set_brightness(target_bright);

    // The clock: the forced divider wins, else the low clock while the machine allows it and nothing needs timing. Off waits for
    // the panel to be asleep; Dim waits for the wake repaint to finish. Ripple keeps full 240 MHz so ADC_CLK stays 15 MHz.
    std::uint8_t want = 1;
    if (g_force_div != 0) want = g_force_div;
    else if (!ripple_boost && o.low_clock_ok && !g_waking && !g_wait_paint && (o.state != idle::State::Off || g_asleep) && blocked == 0)
        want = kLowClockDiv;
    if (want > 1 && ((blocked & ~kBlockHold) != 0 || ripple_boost)) want = 1;   // DWT or Ripple: never low, even forced
    apply_div(want);

    // Deferred save: one flash write per burst, never while a test owns the ports.
    if (g_dirty && !in.busy && now - g_dirty_ms >= kSaveDelayMs) {
        const idle::Config c = idle_cfg::persisted(g_m.config(), g_saved);
        const bool ok = idle_cfg::save(fs::instance(), c);
        g_dirty = false;
        if (ok) g_saved = c;
        K2LOG("[IDLE] saved bright=%u dim=%u dim_after=%u off_after=%u (%s)\n", unsigned(c.brightness), unsigned(c.dim_brightness),
              unsigned(c.dim_after_s), unsigned(c.off_after_s), ok ? "ok" : "FLASH ERROR");
    }
}

}  // namespace k2::idle_hw
