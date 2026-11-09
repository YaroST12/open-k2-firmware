// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Open-K2 firmware entry: hardware bring-up, button sampling, and the periodic service loop.
#include <cstdint>
#include "k2/drivers/buttons.hpp"
#include "k2/ui/capture.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/log.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/accelerometer.hpp"
#include "k2/drivers/board.hpp"
#include "k2/drivers/dpdm.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/drivers/crm.hpp"
#include "k2/drivers/dfu.hpp"
#include "k2/core/fault.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/ui/uiprefs.hpp"
#include "k2/drivers/sysreset.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/i2c.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/drivers/load.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/core/bbpersist.hpp"
#include "k2/pd/transcap.hpp"
#include "k2/core/boottrace_store.hpp"
#include "k2/chargertest/session_store.hpp"
#include "k2/core/bootinfo_hw.hpp"
#include "k2/core/bootmode.hpp"
#include "k2/ui/bootscreen.hpp"
#include "k2/ui/bootscreen_hw.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/pd/power_source.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/drivers/usbcfg.hpp"
#if K2_NET
#include "k2/net/http_server.hpp"
#endif
#include "k2/drivers/msc.hpp"
#include "k2/core/remote.hpp"
#include "k2/core/shell_hw.hpp"
#include "k2/ui/ui.hpp"
#if K2_NET
#include "k2/net/net.hpp"
#endif

namespace {

using k2::Button;
using k2::ButtonDetector;
using k2::ButtonEvent;

constexpr uint32_t kLoopMs = 20;          // service period for the FUSB302 / power monitor / UI tick
constexpr uint32_t kHeartbeatMs = 2000;
extern "C" std::uint32_t k2_stack_free_bytes();
extern "C" std::uint32_t k2_msp_free_bytes();
constexpr uint32_t kIdleLogEvery = 5;       // every 10 s

k2::Ui g_ui;
k2::Remote g_remote{g_ui};
k2::bootscreen::Controller g_bootscreen;  // the boot-info QR screen of a key-mode boot (drawn from the loop, below)
ButtonDetector g_det[4];                  // indexed by Button: Back, Minus, Ok, Plus
k2::accelerometer::Driver g_accelerometer;

// Buttons are active low with pull-ups (pin map in k2/drivers/pins.hpp).
inline bool raw_down(Button b) {
    switch (b) {
    case Button::Plus:  return !k2::pins::key_plus.read();
    case Button::Minus: return !k2::pins::key_minus.read();
    case Button::Ok:    return !k2::pins::key_ok.read();
    case Button::Back:  return !k2::pins::key_back.read();
    }
    return false;
}

void buttons_init() {
    // Keys sit on A9, A13, A14 and B14: enable their own port clocks (SWJ is released by lcd::init).
    k2::reg::crm().APB2EN |= k2::reg::Crm::APB2EN_GPIOA | k2::reg::Crm::APB2EN_GPIOB;
    for (const auto pin : {k2::pins::key_back, k2::pins::key_ok, k2::pins::key_plus, k2::pins::key_minus})
        pin.pull_up();
}

// First thing in main, before the clock, LCD and USB: which of BACK/OK/PLUS is held (boot
// modes). MINUS is not read: it is the bootloader's DFU key and is handled before the app runs.
// PA13 (PLUS) is a SWJ pin at reset, so the debug port is released here (lcd::init does it again,
// harmlessly) and the keys get their pull-ups; the 8 MHz HICK start-up is slower than the RC
// settling, so no extra delay is added beyond a short spin.
bool g_boot_back_held = false;   // BACK down at power-up: cancels the resume of an interrupted charger test

k2::bootmode::Mode sample_boot_mode() {
    k2::reg::crm().APB2EN |= k2::reg::Crm::APB2EN_GPIOA | k2::reg::Crm::APB2EN_GPIOB | k2::reg::Crm::APB2EN_AFIO;
    auto& afio = k2::reg::afio();
    afio.MAP = (afio.MAP & ~k2::reg::Afio::MAP_SWJ_MASK) | k2::reg::Afio::MAP_SWJ_DISABLED;
    for (const auto pin : {k2::pins::key_back, k2::pins::key_ok, k2::pins::key_plus}) pin.pull_up();
    for (volatile int i = 0; i < 2000; i++) {}   // pull-up settling (about 0.2 ms at the 8 MHz start clock)
    std::uint8_t held = 0;
    if (!k2::pins::key_back.read()) held |= k2::bootmode::kBack;
    if (!k2::pins::key_ok.read())   held |= k2::bootmode::kOk;
    if (!k2::pins::key_plus.read()) held |= k2::bootmode::kPlus;
    g_boot_back_held = (held & k2::bootmode::kBack) != 0;
    const k2::bootmode::Mode mode = k2::bootmode::decide(held);
    std::uint8_t pa = 0;   // raw levels for `bootinfo`, 1 = high = released (MINUS is only looked at, never acted on)
    if (k2::pins::key_ok.read())    pa |= k2::bootinfo::kPaOk;
    if (k2::pins::key_plus.read())  pa |= k2::bootinfo::kPaPlus;
    if (k2::pins::key_back.read())  pa |= k2::bootinfo::kPaBack;
    if (k2::pins::key_minus.read()) pa |= k2::bootinfo::kPaMinus;
    k2::bootinfo::note_boot(mode, held, pa);
    return mode;
}

// A self-powered boot (a pinned Rd, k2/pd/pd_ccmode.hpp self_hold_wanted) runs on the charger's VBUS, and the charger wants a
// Request within tSenderResponse (24-30 ms) of every capabilities message the chip acknowledges. The init after
// fusb302::init() (black box reload, report cards, the first UI build) keeps the main loop away for about a second with a full
// store: Auto-CRC came on late, an acknowledged capabilities message went unanswered, the source sent a Hard Reset and cut
// VBUS, and a K2 without another supply died (CUKTECH 120 W, 2026-11-09, runs/selfpwr-drop/cap1). So the PD link is served
// here, before that init, until the self-hold contract is Ready or kBringupMs pass.
[[gnu::noinline]] void self_power_bringup() {
    constexpr uint32_t kBringupMs = 3000;
    const uint32_t t0 = k2::systick::millis();
    uint32_t last = t0, last_poll = t0;
    while (k2::systick::millis() - t0 < kBringupMs) {
        const uint32_t t = k2::systick::millis();
        if (t != last) { last = t; k2::fusb302::service(); }
        if (t - last_poll >= 10) { last_poll = t; k2::fusb302::poll(); }
        if (k2::fusb302::sink_status().state == k2::pd::SinkState::Ready) break;
    }
    K2LOG("[PD] self-power bring-up: %s after %u ms\n",
          k2::fusb302::sink_status().state == k2::pd::SinkState::Ready ? "contract ready" : "no contract",
          static_cast<unsigned>(k2::systick::millis() - t0));
}

[[noreturn]] void system_reset() {
    *reinterpret_cast<volatile uint32_t *>(0xE000ED0C) = 0x05FA0004u;
    for (;;) {}
}

// Same exit as a 5 s BACK hold: say so on the LCD, let queued USB reports go out, reset.
[[noreturn]] void reboot_now() {
    g_ui.show_rebooting();
    k2::systick::delay_ms(200);
    k2::usb::detach();                 // the host must see a disconnect, or it keeps stale state
    system_reset();
}

// DFU_ENTER: clear the bootloader marker and reset. Returns only when that failed; the app
// then carries on (and says why on the log).
void dfu_now() {
    g_ui.show_rebooting();
    k2::dfu::enter(g_remote.dfu_reset_mode() == 1 ? k2::dfu::ResetMode::Iwdg : k2::dfu::ResetMode::Sys);
}

// Everything up to the first loop pass. Its own function (never inlined) so the boot-time locals and the inlined
// init bodies are popped before the loop runs: main's frame sits under every call of the firmware's lifetime.
[[gnu::noinline]] void boot() {
    k2::sysreset::capture_flags();          // reset cause first, before anything can reset again
    k2::reg::scb().VTOR = 0x08003800;
    const k2::bootmode::Mode boot_mode = sample_boot_mode();   // buttons first, before any slow init
    k2::crm::init();
    k2::systick::init();
    k2::i2c::init();
    // Before the LCD and USB init: sink modes put Rd out so the charger starts VBUS (and keeps it) at once;
    // a plain boot writes SWITCHES0 = 0 (no Rd; the chip's default is Rd on both) BEFORE the CC switches close, and keeps an
    // Rd only after a warm reset that follows a pinned boot (the pin mark in DR21): a K2 powered over HID has no pin, so a
    // flash, `reboot` or watchdog is a detach for it. Both CC switches are routed closed in every mode.
    k2::fusb302::self_power_early(boot_mode, k2::sysreset::warm(k2::sysreset::reset_flags()));
    k2::boottrace::init(boot_mode);          // ADC + power monitor up, 50 ms power trace for the first 3 s
    k2::lcd::init();
    buttons_init();
    k2::dpdm::init();
    k2::load::init();   // PA8 stays a low output until a bounded probe pulse asks for the load
    k2::fault::init();
    k2::board::init();
    // USB is off by default and remembered (k2/drivers/usbcfg.hpp): with neither HID nor the drive enabled
    // the pull-up never appears and the host sees no device. DFU by MINUS at plug-in is unaffected.
    {   // littlefs volume (k2/drivers/fs_flash.hpp) before any store that lives on it
        bool formatted = false;
        k2::fs::init(formatted);
    }
    k2::usbcfg::init();   // the USB link itself is connected last, just before the main loop (see there)
    k2::systick::delay_ms(50);
    K2LOG("[BOOT] Open-K2 Firmware Starting...\n");
    K2LOG("[BOOT] mode=%s\n", k2::bootmode::name(boot_mode));
    K2LOG("[SYS] SCB_VTOR set to 0x08003800\n");
    {   // which reset got us here (captured and cleared at the top of main, so only this boot's cause)
        char names[32];
        const std::uint32_t sts = k2::sysreset::reset_flags();
        k2::sysreset::describe_flags(sts, names, sizeof names);
        K2LOG("[BOOT] reset=%s\n", k2::sysreset::cause_name(k2::sysreset::cause(sts)));
        K2LOG("[RST] CTRLSTS=%08X %s\n", static_cast<unsigned>(sts), names);
    }
    K2LOG("[LCD] LCD initialized & backlight ON\n");
    K2LOG("[I2C] Dual I2C busses initialized\n");

    k2::fusb302::init();
    if (k2::fusb302::self_powered()) self_power_bringup();   // before the slow init below: the charger feeds this boot
    k2::boottrace::report_previous();       // what the previous boot's supply / CC / PLUS trace ended with
    k2::session::init();                    // backup registers: what the previous boot was doing (PD session record)
    k2::bbpersist::init();                  // black box: remount the flash ring, reload the newest captures
    {
        const k2::uiprefs::Prefs prefs = k2::uiprefs::stored();   // imports the old raw preferences page (usbcfg did the same above) ...
        k2::uiprefs::screenshot_enabled = prefs.screenshot;
        k2::power::set_smoothing(prefs.smoothing);
    }
    k2::fs::wipe_legacy();                  // ... then the pre-littlefs raw pages are erased, once (marker /.legacy)
    k2::systick::delay_ms(20);   // let the DMA ring fill before the raw one-shot sweep
    k2::adc::log_raw();
    k2::fastcharge::init();

    g_ui.init(k2::systick::millis(), g_boot_back_held);
    // A key-mode boot shows what it decided as a QR code; the first frame is drawn by the main loop, not from here.
    if (k2::bootmode::self_powered(boot_mode)) g_bootscreen.start(k2::systick::millis());
    K2LOG("[ACCEL] compatible gravity sensor %s at INA bus 0x18\n",
                    g_accelerometer.init(k2::systick::millis()) ? "online" : "unavailable");

    k2::systick::set_wait_hook(k2::transcap::poll);   // delay_ms() waits (FUSB302 CC scan, 2 Hz) keep the capture sampling
    k2::lcd::set_idle_hook([] {   // drain PD frames during long draws (tSenderResponse ~30 ms)
        static uint32_t last;
        const uint32_t t = k2::systick::millis();
        if (t != last) { last = t; k2::fusb302::service(); k2::fctrig::service_ufcs(); }
        k2::transcap::poll();   // keep the transition capture's 1 kHz timeline through a redraw
    });
    // USB connects LAST. TinyUSB answers control requests only from tud_task(), so the D+ pull-up
    // must not appear while boot still blocks (fs remount, legacy erase with interrupts off, first
    // screen draw): the host's descriptor reads then time out (-71) and it resets the port again
    // and again (seen 2026-11-06 in the host's kernel log, 3 failed reads per boot).
    if (k2::usbcfg::any(k2::usbcfg::current)) k2::usb::init();
    if (k2::usbcfg::current.drive) k2::msc::init();
    k2::shell_hw::init(g_remote, g_ui);
#if K2_NET
    if (k2::usbcfg::current.net) {
        // The web dashboard's screenshot pulls the screen one strip at a time (capture.hpp).
        static const k2::http_server::Shot shot = {
            [] { return k2::capture_begin(g_ui); },
            [](int i) { return k2::capture_strip(g_ui, i, k2::systick::millis()); },
            [] { k2::capture_end(g_ui, k2::systick::millis()); },
        };
        k2::http_server::init(g_remote, shot);
    }
#endif
}

// The boot-info QR screen (k2/ui/bootscreen.hpp): one frame every 500 ms while the key that chose the boot mode is held, the last
// frame for 30 s after it is released, then the normal UI is drawn again. While it is up the UI neither ticks nor takes buttons
// (a BACK short press closes it early), so nothing paints over the code.
void end_boot_screen(uint32_t now) {
    g_bootscreen.dismiss();
    g_ui.repaint(now);
}

[[gnu::noinline]] void boot_screen_step(uint32_t now) {
    switch (g_bootscreen.tick(now, k2::bootinfo::boot_key_held())) {
    case k2::bootscreen::Controller::Action::Redraw: k2::bootscreen::draw_live(g_bootscreen.frame()); break;
    case k2::bootscreen::Controller::Action::Exit:   g_ui.repaint(now); break;
    case k2::bootscreen::Controller::Action::None:   break;
    }
}

// The periodic bodies of the main loop are their own functions: their locals (power readings, log arguments)
// are popped before the loop goes on, instead of widening main's frame, which sits under every call.
[[gnu::noinline]] void slow_tasks(uint32_t now) {
    k2::fusb302::poll();
    k2::power::poll();
    k2::power_source::poll(now);
    k2::dpsniff::poll(now, k2::power::read().value_or(k2::power::Metrics{}).vbus_mv);
    if (const auto pm = k2::power::read()) k2::pdtrace::vbus_sample(k2::power::vbus_fast_mv().value_or(pm->vbus_mv), pm->current_ma);   // black box, gated 50 ms / 100 mV / 20 mA; VBUS un-averaged so its steps carry their real time
    k2::adc::poll();
    const bool sensor_was_online = g_accelerometer.online();
    if (g_accelerometer.poll(now)) {
        g_ui.feed_gravity(g_accelerometer.read(), now);
        if (sensor_was_online != g_accelerometer.online())
            K2LOG("[ACCEL] %s\n", g_accelerometer.online() ? "online" : "unavailable; retrying");
    }
    k2::fastcharge::poll();
    k2::boottrace::flush_log();             // queued [BOOT] trace lines of the first seconds
    k2::session::poll();                    // PD session record into the backup registers on change
    k2::bbpersist::poll();                  // saves finished captures once the link is quiet
    if (g_bootscreen.active()) boot_screen_step(now);   // the QR screen owns the panel: the UI does not draw meanwhile
    else g_ui.tick(now);
}

// Stack watermark: one line each time the free application stack reaches a new low below 1 KB (the gate's margin),
// with the page the UI was on. The painted words stay dirty, so a deep excursion is caught on the next call.
constexpr uint32_t kStackLowBytes = 1024;

[[gnu::noinline]] void watch_stack(uint32_t &lowest) {
    const uint32_t free_bytes = k2_stack_free_bytes();
    if (free_bytes >= lowest) return;
    lowest = free_bytes;
    K2LOG("[STACK] low: free=%u page=%d\n", static_cast<unsigned>(free_bytes), static_cast<int>(g_ui.page_index()));
}

[[gnu::noinline]] void heartbeat(uint32_t now, uint32_t press_count, uint32_t heartbeats) {
    K2LOG("[SYS] Heartbeat: up=%ds presses=%d page=%d stack_free=%u msp_free=%u\n",
                static_cast<int>(now / 1000), static_cast<int>(press_count),
                static_cast<int>(g_ui.page_index()), static_cast<unsigned>(k2_stack_free_bytes()),
                static_cast<unsigned>(k2_msp_free_bytes()));
    k2::usb::log_stats();
#if K2_NET
    if (k2::usbcfg::current.net) { k2::net::log_stats(); k2::http_server::log_stats(); }
#endif
    // The boot-time "[FC] idle" line is gone by the time a log monitor attaches: repeat it.
    if (heartbeats % kIdleLogEvery == 0) k2::fastcharge::log_idle();
    const auto pm = k2::power::read().value_or(k2::power::Metrics{});
    K2LOG("[PWR] chip=%s vbus=%dmV ibus=%dmA p=%dmW dir=%s\n",
                k2::power::name(k2::power::chip()),
                static_cast<int>(pm.vbus_mv), static_cast<int>(k2::power::magnitude(pm.current_ma)),
                static_cast<int>(k2::power::magnitude(pm.power_mw)),
                pm.flow == k2::power::Flow::Forward ? "up" : pm.flow == k2::power::Flow::Reverse ? "down" : "idle");
    if (const auto a = k2::adc::read()) {
        K2LOG("[ADC] dp=%dmV dm=%dmV cc1=%dmV cc2=%dmV aux=%dmV\n", int(a->dp_mv), int(a->dm_mv),
                         int(a->cc1_mv), int(a->cc2_mv), int(a->aux_mv));
        const int mcu = a->mcu.deci_c;
        K2LOG("[ADC] mcu=%s%d.%dC%s board=n/a vref=%d vdd=%dmV\n",
                         mcu < 0 ? "-" : "", (mcu < 0 ? -mcu : mcu) / 10, (mcu < 0 ? -mcu : mcu) % 10,
                         a->mcu.valid ? " uncal" : " INVALID",
                         int(a->vref_raw), int(a->vdd_mv));
        K2LOG("[FC] %s\n", k2::fastcharge::name(k2::fastcharge::read()));
    }
}

}  // namespace

extern "C" int main(void) {
    boot();
    uint32_t press_count = 0;
    uint32_t last_ms = k2::systick::millis();
    uint32_t last_loop_ms = last_ms;
    uint32_t last_heartbeat_ms = last_ms;
    uint32_t heartbeats = 0;
    uint32_t stack_lowest = kStackLowBytes;
    for (;;) {
        k2::gap::loop.note(k2::systick::millis());   // the longest iteration of a charger-test row ([CT] gap)
        k2::usb::poll();
        k2::shell_hw::poll();
        if (k2::usbcfg::current.drive) k2::msc::poll();
#if K2_NET
        if (k2::usbcfg::current.net) k2::http_server::service();
#endif
        switch (g_remote.poll()) {
        case k2::RemoteEvent::Reboot:     reboot_now(); break;
        case k2::RemoteEvent::Screenshot:
            if (g_bootscreen.active()) end_boot_screen(k2::systick::millis());   // a capture repaints the UI's fields over the code
            k2::capture_screen(g_ui, g_remote.screenshot_seq(), k2::systick::millis()); break;
        case k2::RemoteEvent::Dfu:        dfu_now(); break;
        case k2::RemoteEvent::Fault:      k2::fault::trigger(g_remote.fault_kind()); break;
        case k2::RemoteEvent::None:       break;
        }

        k2::fctrig::poll();
        k2::transcap::poll();                       // transition capture: one VBUS/CC sample when due (no-op unless armed)

        const uint32_t now = k2::systick::millis();
        if (now != last_ms) {                       // sample buttons once per millisecond
            last_ms = now;
            k2::fusb302::service();
            for (int i = 0; i < 4; i++) {
                const Button b = static_cast<Button>(i);
                // An injected press is a virtual pin level, OR-ed in before the debouncer.
                const bool down = raw_down(b) || g_remote.button_down(b, now);
                const ButtonEvent ev = g_det[i].update(down, now);
                if (g_bootscreen.active()) {   // the QR screen takes no input but the early close
                    if (b == Button::Back && ev == ButtonEvent::Short) end_boot_screen(now);
                    continue;
                }
                g_ui.set_pressed(b, g_det[i].pressed(), now);
                if (ev != ButtonEvent::None) {
                    press_count++;
                    if (g_ui.on_button(b, ev, now) == k2::UiAction::Reboot) reboot_now();
                }
            }
        }

        if (now - last_loop_ms >= kLoopMs) {
            last_loop_ms = now;
            slow_tasks(now);
            watch_stack(stack_lowest);
        }

        if (now - last_heartbeat_ms >= kHeartbeatMs) {
            last_heartbeat_ms = now;
            heartbeat(now, press_count, ++heartbeats);
        }

        __asm volatile("wfi");                      // SysTick wakes this every millisecond
    }
}
