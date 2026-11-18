// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Hot main-loop poll for the debug shell (kept in the zero-wait flash window; cold handlers live in shell_hw.cpp).
#include "k2/core/shell_hw.hpp"
#include "shell_hw_detail.hpp"

#include <array>
#include <cstdint>
#include <span>

#include "k2/core/dbg_stream_hw.hpp"
#include "k2/core/hot.h"
#include "k2/core/idle_hw.hpp"
#include "k2/core/loop_guard.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "tusb.h"

namespace k2::shell_hw {

K2_HOT void poll() {
    using namespace detail;
    if (!g_remote) return;
    const bool up = session_up();
    if (!up) {
        if (g_session) session_end();
        return;
    }
    if (!g_session) {
        g_session = true;
        put("open-k2 shell, 'help' lists the commands\r\n");
    }
    // A raw level above 5 V-class signalling is only safe under the monitor's ceiling: drop it if VBUS got there.
    if (g_raw_dp || g_raw_dm) {
        const auto pm = power::read();
        if (!pm || pm->vbus_mv > power::max_vbus_mv() || (!g_fc_started && pm->vbus_mv > kLabAbortMv) || loopguard::locked()) raw_to_hiz();
    }
    if (g_silent_until && static_cast<std::int32_t>(systick::millis() - g_silent_until) >= 0) silent_end();
    std::array<char, 32> buf;
    for (std::uint32_t n; (n = tud_cdc_read(buf.data(), buf.size())) > 0;) {
        idle_hw::request(true, systick::millis());   // typed input is a command: wake the screen, full clock for the lab timing
        for (std::uint32_t i = 0; i < n; i++) g_shell_ptr->feed(buf[i]);
    }
    if (!usb::cdc_hold()) {
        // Edge stream: whole frames while the FIFO has room, a few per call, never waiting.
        if (dpsniff::streaming()) {
            dpsniff::stream_service();
            std::array<std::uint8_t, edges::kFrameMax> f;
            for (int i = 0; i < 8 && tud_cdc_write_available() >= f.size(); i++) {
                const std::size_t n = dpsniff::stream_frame(f);
                if (n == 0) break;
                tud_cdc_write(f.data(), static_cast<std::uint32_t>(n));
            }
        }
        // Debug stream (k2/core/dbg_stream.hpp): whole frames while the FIFO has room, a bounded number per pass, never waiting.
        if (dbg_hw::running())
            dbg_hw::service(tud_cdc_write_available(), [](std::span<const std::uint8_t> f) {
                tud_cdc_write(f.data(), static_cast<std::uint32_t>(f.size()));
            });
    }
    tud_cdc_write_flush();
}

}  // namespace k2::shell_hw
