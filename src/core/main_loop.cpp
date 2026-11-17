// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The main loop's work (k2/core/main_loop.hpp): button sampling, the periodic service bodies and the LCD idle hook. main.cpp
// runs it on the device; tests/test_world_replay.cpp runs the same code on the host against a virtual clock.
#include "k2/core/main_loop.hpp"

#include <cstdint>

#include "k2/core/build_config.h"
#include "k2/core/fault.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/hot.h"
#include "k2/core/log.hpp"
#include "k2/core/loop_stats.hpp"
#include "k2/core/paint_meter.hpp"
#include "k2/core/remote.hpp"
#include "k2/core/shell_hw.hpp"
#include "k2/core/stack_probe.hpp"
#include "k2/core/stage_meter.hpp"
#include "k2/core/trace_hid_hw.hpp"
#include "k2/drivers/buttons.hpp"
#include "k2/drivers/dpdm.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/msc.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/drivers/usbcfg.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/pd/transcap.hpp"
#include "k2/ui/bootscreen.hpp"
#if K2_SCREENSHOT
#include "k2/ui/capture.hpp"
#endif
#include "k2/ui/ui.hpp"
#if K2_NET
#include "k2/net/http_server.hpp"
#endif

extern "C" std::uint32_t k2_stack_free_bytes();

namespace k2::mainloop {
namespace detail {
Context* g_ctx = nullptr;
void end_boot_screen(uint32_t now);
void slow_tasks(uint32_t now);
void heartbeat(uint32_t now, uint32_t press_count, uint32_t heartbeats);
}  // namespace detail

namespace {

constexpr uint32_t kLoopMs = 20;          // service period for the FUSB302 / power monitor / UI tick
constexpr uint32_t kHeartbeatMs = 2000;

ButtonDetector g_det[4];                  // indexed by Button: Back, Minus, Ok, Plus
ButtonDetector g_vdet[4];                 // the remote's keys (logical, separate from the physical pins)
struct QueuedButton {
    Button button = Button::Back;
    ButtonEvent ev = ButtonEvent::None;
    uint32_t now = 0;
    bool remote = false;
};
constexpr int kBtnQueueCap = 16;
QueuedButton g_btn_q[kBtnQueueCap]{};
uint8_t g_btn_head = 0;
uint8_t g_btn_tail = 0;
uint8_t g_btn_count = 0;
uint32_t g_press_count = 0;
uint32_t g_last_ms = 0;
uint32_t g_last_btn_ms = 0;
uint32_t g_last_loop_ms = 0;
uint32_t g_last_heartbeat_ms = 0;
uint32_t g_heartbeats = 0;

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

K2_HOT void push_btn(Button b, ButtonEvent ev, uint32_t now, bool remote) {
    if (ev == ButtonEvent::None || g_btn_count >= kBtnQueueCap) return;
    g_btn_q[g_btn_tail] = {b, ev, now, remote};
    g_btn_tail = static_cast<uint8_t>((g_btn_tail + 1) % kBtnQueueCap);
    g_btn_count++;
}

K2_HOT bool pop_btn(QueuedButton &out) {
    if (g_btn_count == 0) return false;
    out = g_btn_q[g_btn_head];
    g_btn_head = static_cast<uint8_t>((g_btn_head + 1) % kBtnQueueCap);
    g_btn_count--;
    return true;
}

// Sample physical and remote buttons at 1 ms resolution so presses shorter than a UI paint or driver wait
// are debounced and queued without invoking Ui inside the idle/wait hook (ARCH_REWORK E3).
K2_HOT bool sample_buttons(uint32_t now) {
    Context *const c = detail::g_ctx;
    if (!c || now == g_last_btn_ms) return false;
    g_last_btn_ms = now;
    for (int i = 0; i < 4; i++) {
        const Button b = static_cast<Button>(i);
        // A remote press is a logical key with its own debouncer: it never mixes into the physical pin level,
        // so the UI does not remap it by the rotation and it cannot join the +/- rotation gesture.
        const bool vdown = c->remote.button_down(b, now);
        const ButtonEvent vev = g_vdet[i].update(vdown, now);
        const bool down = raw_down(b);
        const ButtonEvent ev = g_det[i].update(down, now);
        if (vev != ButtonEvent::None) push_btn(b, vev, now, true);
        // The key that chose the boot mode is still down (its Long / Hold would reach the UI, and it could be one half of
        // the OK + Back reboot chord): its events are the boot's, not the user's, until it is up. Other keys work at once.
        if (c->boot_key_swallow && b == c->boot_key) {
            if (!down && !g_det[i].pressed()) c->boot_key_swallow = false;
            continue;
        }
        if (ev != ButtonEvent::None) push_btn(b, ev, now, false);
    }
    return true;
}

K2_HOT void wait_hook() {
    k2::transcap::sample();   // a kept capture is written by pass()'s poll()
    sample_buttons(k2::systick::millis());
}

// The stage meter (k2/core/stage_meter.hpp): the start of each poll below, so a stall is named and timed.
using Stage = k2::stage::Id;
inline void mark(Stage s) { k2::stage::meter.mark(s, k2::dpdm::cycles(), k2::systick::millis()); }

// Stack watermark: one line each time the free application stack reaches a new low below 1 KB (the gate's margin),
// with the page the UI was on. The painted words stay dirty, so a deep excursion is caught on the next call.
constexpr uint32_t kStackLowBytes = 1024;
uint32_t g_stack_lowest = kStackLowBytes;

[[gnu::noinline]] K2_HOT void watch_stack(uint32_t &lowest) {
#if K2_STACKPROBE
    // The PSP probe's newest low: the code SysTick interrupted at the deepest sample (addr2line -f -C on pc and lr).
    k2::stackprobe::Low probe;
    uint32_t probe_free = 0;
    if (k2::stackprobe::take(probe, probe_free))
        K2LOG("[STK] low=%u pc=0x%08X lr=0x%08X\n", static_cast<unsigned>(probe_free), static_cast<unsigned>(probe.pc),
              static_cast<unsigned>(probe.lr));
#endif
    const uint32_t free_bytes = k2_stack_free_bytes();
    if (free_bytes >= lowest) return;
    lowest = free_bytes;
    K2LOG("[STACK] low: free=%u page=%d\n", static_cast<unsigned>(free_bytes), static_cast<int>(detail::g_ctx->ui.page_index()));
}

}  // namespace

K2_HOT void idle_hook() {   // drain PD frames during long draws (tSenderResponse ~30 ms)
    static uint32_t last;
    const uint32_t t = k2::systick::millis();
    if (t != last) {
        last = t;
        k2::loopstat::stats.service.note(t);   // instrumentation: the PD service cadence inside a paint
        k2::fusb302::service(); k2::fctrig::service_ufcs(); k2::fctrig::service_hs();
    }
    sample_buttons(t);   // capture 1 ms button edges even across long draws (E3)
    // Unattached, poll() does not run during a long redraw (a Home repaint as VBUS appears is ~150 ms): read VBUS here and
    // let a rise kick the CC scan, or the source's offers ~130 ms after its VBUS go by unrouted (fusb302.hpp scan_service).
    static uint32_t last_scan;
    if (k2::fusb302::active_cc() == 0 && t - last_scan >= 10) {
        last_scan = t;
        int32_t mv = -1;
        if (!k2::power::fast_vbus_mv(mv)) mv = -1;
        k2::fusb302::scan_service(mv);
    }
    k2::transcap::sample();   // keep the transition capture's 1 kHz timeline through a redraw
}

void install_hooks() {
    k2::systick::set_wait_hook(wait_hook);   // delay_ms() waits keep transition capture & 1 ms button polling active
    k2::lcd::set_idle_hook(idle_hook);
}

void begin(Context& ctx) {
    detail::g_ctx = &ctx;
    ctx.ui.set_paint_clock(k2::systick::micros);   // every paint slice also stops on measured time (PaintBudget::kPassUs)
    for (auto &d : g_det) d = {};
    for (auto &d : g_vdet) d = {};
    g_btn_head = g_btn_tail = g_btn_count = 0;
    g_last_ms = k2::systick::millis();
    g_last_btn_ms = g_last_ms;
    g_last_loop_ms = g_last_ms;
    g_last_heartbeat_ms = g_last_ms;
}

[[gnu::noinline]] K2_HOT void pass() {   // noinline: main() would otherwise swallow it into its cold boot-time body
    Context& c = *detail::g_ctx;
    const uint32_t pass_t0_us = k2::systick::micros();          // instrumentation (k2/core/loop_stats.hpp): this pass's length
    const uint64_t pass_px0 = k2::paint::meter.px_count();      // and the pixels it sent
    k2::loopstat::stats.loop.note(k2::systick::millis());
    k2::gap::loop.note(k2::systick::millis());   // the longest iteration of a charger-test row ([CT] gap)
    mark(Stage::Usb);
    k2::usb::poll();
    if (k2::usbcfg::current.drive && k2::msc::io()) k2::usb::poll();   // READ10 renders here, never under a paint
    mark(Stage::Shell);
    k2::shell_hw::poll();
    mark(Stage::Msc);
    if (k2::usbcfg::current.drive) k2::msc::poll();
#if K2_NET
    if (k2::usbcfg::current.net) k2::http_server::service();
#endif
    mark(Stage::Remote);
    switch (c.remote.poll()) {
    case k2::RemoteEvent::Reboot:     c.reboot(); break;
    case k2::RemoteEvent::Screenshot:
#if K2_SCREENSHOT
        mark(Stage::Capture);
        if (c.bootscreen.active()) detail::end_boot_screen(k2::systick::millis());   // a capture repaints the UI's fields over the code
        k2::capture_screen(c.ui, c.remote.screenshot_seq(), k2::systick::millis(), c.remote.screenshot_transport());
#endif
        break;
    case k2::RemoteEvent::Dfu:        c.dfu(); break;
    case k2::RemoteEvent::Fault:      k2::fault::trigger(c.remote.fault_kind()); break;
    case k2::RemoteEvent::None:       break;
    }
    k2::trace_hid_hw::service();                // Black Box push over HID: queues reports while the mode is on (no-op otherwise)

    mark(Stage::Fctrig);
    k2::fctrig::poll();
    mark(Stage::Transcap);
    k2::transcap::poll();                       // transition capture: one VBUS/CC sample when due (no-op unless armed)

    const uint32_t now = k2::systick::millis();
    if (now != g_last_ms) {                     // PD service once per millisecond
        g_last_ms = now;
        k2::loopstat::stats.service.note(now);
        mark(Stage::Service);
        k2::fusb302::service();
    }
    const bool sampled = sample_buttons(now);
    if (sampled || g_btn_count > 0) {
        mark(Stage::Buttons);
        if (!c.bootscreen.showing()) {
            for (int i = 0; i < 4; i++) {
                const Button b = static_cast<Button>(i);
                c.ui.set_remote_pressed(b, g_vdet[i].pressed(), now);
                if (!c.boot_key_swallow || b != c.boot_key) c.ui.set_pressed(b, g_det[i].pressed(), now);
            }
        }
        QueuedButton qb;
        while (pop_btn(qb)) {
            if (c.bootscreen.showing()) {   // the QR screen takes no input but the early close
                if (qb.button == Button::Back && qb.ev == ButtonEvent::Short) detail::end_boot_screen(qb.now);
                continue;
            }
            g_press_count++;
            const k2::UiAction act = qb.remote ? c.ui.on_remote_button(qb.button, qb.ev, qb.now)
                                               : c.ui.on_button(qb.button, qb.ev, qb.now);
            if (act == k2::UiAction::Reboot) c.reboot();
        }
    }

    if (now - g_last_loop_ms >= kLoopMs) {
        g_last_loop_ms = now;
        detail::slow_tasks(now);
        mark(Stage::Stack);
        watch_stack(g_stack_lowest);
    } else if (c.ui.painting() && !c.bootscreen.showing()) {
        // A sliced paint goes on at the pass rate: waiting for the next 20 ms tick between slices would stretch a page
        // switch to 20 ms per slice. One slice is bounded by the paint budget (about 8 ms), the pass work above is the
        // same as on any pass, and the button, PD and USB services keep their 1 ms rhythm between slices.
        mark(Stage::UiTick);
        c.ui.paint_more(now);
    }

    if (now - g_last_heartbeat_ms >= kHeartbeatMs) {
        g_last_heartbeat_ms = now;
        mark(Stage::Heartbeat);
        detail::heartbeat(now, g_press_count, ++g_heartbeats);
    }
    k2::loopstat::stats.note_pass(c.ui.page_index(), k2::systick::micros() - pass_t0_us,
                                  static_cast<uint32_t>(k2::paint::meter.px_count() - pass_px0));
    mark(Stage::Idle);   // the wfi up to the next pass
}

}  // namespace k2::mainloop
