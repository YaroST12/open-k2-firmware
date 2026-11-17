// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Cold main-loop paths (20 ms periodic poll bodies, 2 s heartbeat, and boot-screen stepping) compiled under COLD_SIZE_FLAGS (-Oz).
// Hot per-pass dispatch and 1 ms hooks stay in main_loop.cpp.
#include "k2/core/main_loop.hpp"

#include <cstdint>

#include "k2/battery/battery_hw.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include "k2/chargertest/session_store.hpp"
#include "k2/core/bbpersist.hpp"
#include "k2/core/bootinfo_hw.hpp"
#include "k2/core/boottrace_store.hpp"
#include "k2/core/build_config.h"
#include "k2/core/handback.hpp"
#include "k2/core/log.hpp"
#include "k2/core/loop_guard.hpp"
#include "k2/core/memstat.hpp"
#include "k2/core/rd_comeback.hpp"
#include "k2/core/rescan.hpp"
#include "k2/core/stage_meter.hpp"
#include "k2/core/survey.hpp"
#include "k2/drivers/accelerometer.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/dpdm.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/drivers/usbcfg.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/pd/diag_live.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/pd/power_source.hpp"
#include "k2/ui/bootscreen.hpp"
#include "k2/ui/bootscreen_hw.hpp"
#include "k2/ui/ui.hpp"
#if K2_NET
#include "k2/net/http_server.hpp"
#include "k2/net/net.hpp"
#endif
#if K2_HOST_REPORT
#include "k2/loopprobe/host_report_hw.hpp"
#endif

extern "C" std::uint32_t k2_stack_free_bytes();
extern "C" std::uint32_t k2_msp_free_bytes();

namespace k2::mainloop::detail {
extern Context* g_ctx;
}  // namespace k2::mainloop::detail

namespace k2::mainloop {
namespace {

constexpr uint32_t kIdleLogEvery = 5;       // every 10 s
constexpr uint32_t kEraseQuietMs = 1000;    // A ring-log erase waits for this long without PD traffic or a VBUS change.

using Stage = k2::stage::Id;
inline void mark(Stage s) { k2::stage::meter.mark(s, k2::dpdm::cycles(), k2::systick::millis()); }

[[gnu::noinline]] void boot_screen_step(uint32_t now) {
    switch (detail::g_ctx->bootscreen.tick(now, k2::bootinfo::boot_key_held())) {
    case k2::bootscreen::Controller::Action::Redraw: k2::bootscreen::draw_live(detail::g_ctx->bootscreen.frame()); break;
    case k2::bootscreen::Controller::Action::Exit:   detail::g_ctx->ui.repaint(now); break;
    case k2::bootscreen::Controller::Action::None:   break;
    }
}

}  // namespace

namespace detail {

// The boot-info QR screen (k2/ui/bootscreen.hpp): one frame every 500 ms while the key that chose the boot mode is held, the last
// frame for 30 s after it is released, then the normal UI is drawn again. While it is up the UI neither ticks nor takes buttons
// (a BACK short press closes it early), so nothing paints over the code.
void end_boot_screen(uint32_t now) {
    g_ctx->bootscreen.dismiss();
    g_ctx->ui.repaint(now);
}

// The periodic bodies of the main loop are their own functions: their locals (power readings, log arguments)
// are popped before the loop goes on, instead of widening main's frame, which sits under every call.
[[gnu::noinline]] void slow_tasks(uint32_t now) {
    Context& c = *g_ctx;
    mark(Stage::Fusb302);
    k2::fusb302::poll();
    mark(Stage::Power);
    k2::power::poll();
    mark(Stage::PowerSource);
    k2::power_source::poll(now);
    mark(Stage::Dpsniff);
    k2::dpsniff::poll(now, k2::power::read().value_or(k2::power::Metrics{}).vbus_mv);
    mark(Stage::Pdtrace);
    if (const auto pm = k2::power::read()) k2::pdtrace::vbus_sample(k2::power::vbus_fast_mv().value_or(pm->vbus_mv), pm->current_ma);   // black box, gated 50 ms / 100 mV / 20 mA; VBUS un-averaged so its steps carry their real time
    mark(Stage::DiagLive);
    k2::pd::diag::live::poll(now);          // Black Box diagnosis: the fresh sample into its power window, its clock
    mark(Stage::Adc);
    k2::adc::poll();
    mark(Stage::LoopGuard);
    k2::loopguard::poll(now);              // the HID-cable loop 5 V lock: before the UI tick, so its tools see the lock this pass
#if K2_HOST_REPORT
    k2::loopprobe::host::hw::poll(now);    // the loop probe's Host report: the boot journal, the session (prototype)
#endif
    mark(Stage::Accel);
    const bool sensor_was_online = c.accelerometer.online();
    if (c.accelerometer.poll(now)) {
        c.ui.feed_gravity(c.accelerometer.read(), now);
        if (sensor_was_online != c.accelerometer.online())
            K2LOG("[ACCEL] %s\n", c.accelerometer.online() ? "online" : "unavailable; retrying");
    }
    mark(Stage::Fastcharge);
    k2::fastcharge::poll();
    mark(Stage::Boottrace);
    k2::boottrace::flush_log();             // queued [BOOT] trace lines of the first seconds
    mark(Stage::Session);
    k2::session::poll();                    // PD session record into the backup registers on change
    mark(Stage::RdComeback);
    k2::rdcomeback::poll();                 // the Rd intent marker (/rd.rec) in step with the session, written on a quiet link
    mark(Stage::Handback);
    k2::handback::poll(now);                // the phase-1 lab hand-back sequence, if the remote armed one
    mark(Stage::Rescan);
    k2::rescan::poll();                     // a Refresh's Get_Source_Cap: the answer, or the CC cycle when the source stays silent
    mark(Stage::Survey);
    k2::survey::poll(c.ui.charger_test_busy());   // the rest of the Refresh's survey, one message at a time (nothing runs before a Refresh)
    if (c.rescan_landing_due && !k2::rdcomeback::pending()) {   // a Refresh's CC cycle took the K2 down: back on Connection -> Source
        c.rescan_landing_due = false;
        if (k2::rdcomeback::rescan_landing()) c.ui.land_rescanned(now);
    }
    // ring maintenance erases (~35 ms each) only with no charger test, PD Stress run or other port owner and a quiet link.
    // No flash at all while the SuperVOOC trigger runs: a Black Box save stalls this loop for 95-290 ms (and masks
    // interrupts for every half-word), so the wire's tick cannot let D- go after a reply's hold. The trigger's own edge
    // recording (4096 edges) is a finished capture after ~72 pairs, where every hardware run of 2026-11-12 stopped.
    // Waiting captures go out once the run is over.
    if (!k2::fctrig::svooc_running()) {
        const bool may_erase = !c.ui.ports_busy() && k2::pdtrace::since_activity_ms() >= kEraseQuietMs;
        mark(Stage::Bbpersist);
        k2::bbpersist::poll(may_erase);     // saves finished captures once the link is quiet
        mark(Stage::ReportCard);
        k2::report_card::hw::poll(may_erase);
        k2::battery::hw::poll(now, may_erase);   // battery recorder: every sample; its record only at a phase boundary
        k2::memstat::poll(now, may_erase);       // memory readout cache: stacks at most 1 Hz, the littlefs walk only when quiet
    }
    mark(Stage::BootScreen);
    if (c.bootscreen.active()) boot_screen_step(now);   // waits for the key's 3 s, then owns the panel
    // The QR screen owns the panel: the UI does not draw meanwhile. The UI's tick ends the OK + Back reboot chord.
    mark(Stage::UiTick);
    if (!c.bootscreen.showing() && c.ui.tick(now) == k2::UiAction::Reboot) c.reboot();
}

[[gnu::noinline]] void heartbeat(uint32_t now, uint32_t press_count, uint32_t heartbeats) {
    K2LOG("[SYS] Heartbeat: up=%ds presses=%d page=%d stack_free=%u msp_free=%u\n",
                static_cast<int>(now / 1000), static_cast<int>(press_count),
                static_cast<int>(g_ctx->ui.page_index()), static_cast<unsigned>(k2_stack_free_bytes()),
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
        K2LOG("[FC] %s\n", k2::fastcharge::name(k2::fastcharge::label()));
    }
}

}  // namespace detail
}  // namespace k2::mainloop
