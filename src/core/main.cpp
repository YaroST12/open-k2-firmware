// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Open-K2 firmware entry: hardware bring-up, button sampling, and the periodic service loop.
#include <cstdint>
#include "k2/battery/battery_hw.hpp"
#include "k2/drivers/buttons.hpp"
#include "k2/ui/capture.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/log.hpp"
#include "k2/core/main_loop.hpp"
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
#include "k2/core/handback.hpp"
#include "k2/core/boot_pd.hpp"
#include "k2/core/rd_comeback.hpp"
#include "k2/core/rescan.hpp"
#include "k2/core/survey.hpp"
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

// VBUS for the cable read's Ra probe (fusb302::set_vbus_source): a direct monitor read, because the main loop that feeds
// vbus_fast_mv() is held while a cable read runs. -1 while the monitor is offline (the probe then refuses).
std::int32_t cable_probe_vbus_mv() {
    const auto m = k2::power::sample_now();
    return m ? m->vbus_mv : -1;
}

k2::Ui g_ui;
k2::Remote g_remote{g_ui};
k2::bootscreen::Controller g_bootscreen;  // the boot-info QR screen of a key-mode boot (drawn from the loop, k2/core/main_loop.hpp)
k2::accelerometer::Driver g_accelerometer;

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

[[noreturn]] void system_reset() {
    *reinterpret_cast<volatile uint32_t *>(0xE000ED0C) = 0x05FA0004u;
    for (;;) {}
}

// Same exit as the OK + Back chord held 3 s: say so on the LCD, let queued USB reports go out, reset.
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

// The loop's view of the objects above (k2/core/main_loop.hpp); boot() fills in the boot-key and Rescan-landing fields.
k2::mainloop::Context g_loop{g_ui, g_remote, g_bootscreen, g_accelerometer, reboot_now, dfu_now};

// Everything up to the first loop pass. Its own function (never inlined) so the boot-time locals and the inlined
// init bodies are popped before the loop runs: main's frame sits under every call of the firmware's lifetime.
[[gnu::noinline]] void boot() {
    k2::sysreset::capture_flags();          // reset cause first, before anything can reset again
    k2::reg::scb().VTOR = 0x08003800;
    const k2::bootmode::Mode keys_mode = sample_boot_mode();   // buttons first, before any slow init
    k2::usb::hold_detached();               // SE0 on the HID port until usb::init() can answer (k2/drivers/usb.hpp)
    k2::crm::init();
    k2::systick::init();
    k2::i2c::init();
    // The Rd decision of this reset and, on a comeback, the PD service before the slow init (k2/core/boot_pd.hpp).
    const bool warm_reset = k2::sysreset::warm(k2::sysreset::reset_flags());
    const k2::bootpd::Early pd_boot = k2::bootpd::early(keys_mode, warm_reset);
    const k2::bootmode::Mode boot_mode = pd_boot.mode;
    k2::boottrace::init(boot_mode);        // ADC + power monitor up, 50 ms power trace for the first 3 s
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

    k2::bootpd::late(pd_boot);              // an ordinary boot starts the FUSB302 here (a comeback did in early())
    k2::boottrace::report_previous();       // what the previous boot's supply / CC / PLUS trace ended with
    k2::session::init();                    // backup registers: what the previous boot was doing (PD session record)
    {
        const k2::uiprefs::Prefs prefs = k2::uiprefs::stored();   // imports the old raw preferences page (usbcfg did the same above) ...
        k2::uiprefs::screenshot_enabled = prefs.screenshot;
        k2::power::set_smoothing(prefs.smoothing);
        k2::fusb302::set_idle_wake(prefs.idle_wake);   // Settings > Wake sleeping port: default off, no Rd at idle
        k2::fusb302::set_vbus_source(&cable_probe_vbus_mv);   // the Ra probe at the end of a silent alone cable read
        g_ui.restore_orientation(prefs.rotation_manual, prefs.rotation, prefs.rotate_buttons);   // before g_ui.init draws the first frame
        g_ui.restore_home_hero(prefs.home_hero);
    }
    k2::fs::wipe_legacy();                  // ... then the pre-littlefs raw pages are erased, once (marker /.legacy)
    k2::fs::grow();                         // ... and only then may the volume grow into the run they lived in (blank proof); scans the rings
    k2::bbpersist::init();                  // black box: its ring is scanned now; reload the newest captures
    k2::battery::hw::init(warm_reset);      // battery recorder: a warm reset resumes its run, a cold boot closes a cut one
    k2::systick::delay_ms(20);   // let the DMA ring fill before the raw one-shot sweep
    k2::adc::log_raw();
    k2::fastcharge::init();

    g_ui.init(k2::systick::millis(), g_boot_back_held);
    g_loop.rescan_landing_due = k2::rdcomeback::found().kind == k2::rdintent::IntentKind::Rescan;   // the verdict lands in the slow tasks
    // A key-mode boot whose key stays down for 3 s from the plug-in shows what it decided as a QR code (the first frame is drawn by the
    // main loop, not from here); a key released sooner goes straight to the UI. A marker's comeback has no key and never shows it.
    // The threshold counts from reset (the uptime clock starts there), not from this line.
    if (k2::bootmode::self_powered(keys_mode)) {
        g_bootscreen.start(0);
        g_loop.boot_key_swallow = true;
        g_loop.boot_key = keys_mode == k2::bootmode::Mode::OkRdCc1 ? Button::Ok : Button::Plus;
    }
    K2LOG("[ACCEL] compatible gravity sensor %s at INA bus 0x18\n",
                    g_accelerometer.init(k2::systick::millis()) ? "online" : "unavailable");

    k2::mainloop::install_hooks();   // the delay_ms() wait hook and the LCD idle hook (PD served during long draws)
    // USB connects LAST. TinyUSB answers control requests only from tud_task(), so the D+ pull-up
    // must not appear while boot still blocks (fs remount, legacy erase with interrupts off, first
    // screen draw): the host's descriptor reads then time out (-71) and it resets the port again
    // and again (seen 2026-11-06 in the host's kernel log, 3 failed reads per boot). Until here
    // hold_detached() keeps the bus in SE0: D+ reads high without it, so the host would attach anyway.
    if (k2::usbcfg::any(k2::usbcfg::current)) k2::usb::init();
    else k2::usb::release_detached();
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

}  // namespace

extern "C" int main(void) {
    boot();
    k2::mainloop::begin(g_loop);
    for (;;) {
        k2::mainloop::pass();                       // the loop body, shared with the host world-replay test
        __asm volatile("wfi");                      // SysTick wakes this every millisecond
    }
}
