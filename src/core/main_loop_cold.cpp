// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Cold main-loop paths (2 s heartbeat and boot-screen stepping) compiled under COLD_SIZE_FLAGS (-Oz).
// Hot per-pass dispatch, the 20 ms periodic poll bodies (slow_tasks) and the 1 ms hooks stay in main_loop.cpp (K2_HOT).
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

}  // namespace

namespace detail {

[[gnu::noinline]] void boot_screen_step(uint32_t now) {
    switch (g_ctx->bootscreen.tick(now, k2::bootinfo::boot_key_held())) {
    case k2::bootscreen::Controller::Action::Redraw: k2::bootscreen::draw_live(g_ctx->bootscreen.frame()); break;
    case k2::bootscreen::Controller::Action::Exit:   g_ctx->ui.repaint(now); break;
    case k2::bootscreen::Controller::Action::None:   break;
    }
}

// The boot-info QR screen (k2/ui/bootscreen.hpp): one frame every 500 ms while the key that chose the boot mode is held, the last
// frame for 30 s after it is released, then the normal UI is drawn again. While it is up the UI neither ticks nor takes buttons
// (a BACK short press closes it early), so nothing paints over the code.
void end_boot_screen(uint32_t now) {
    g_ctx->bootscreen.dismiss();
    g_ctx->ui.repaint(now);
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
