// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/dbg_stream_hw.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

#include "k2/drivers/adc.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/chargertest/run_events.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/sysreset.hpp"
#include "k2/chargertest/session_store.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/core/remote.hpp"
#include "k2/ui/ui.hpp"
#include "k2/core/version.hpp"

extern "C" std::uint32_t k2_stack_free_bytes();
extern "C" std::uint32_t k2_msp_free_bytes();

namespace k2::dbg_hw {
namespace {

dbg::Mux g_mux;
Remote* g_remote = nullptr;
Ui* g_ui = nullptr;
std::uint16_t g_boot_len = 0;

std::uint16_t clamp16(std::int32_t v) { return static_cast<std::uint16_t>(std::clamp<std::int32_t>(v, 0, 0xFFFE)); }

// A stack object per service() pass: it holds no state, so it adds nothing to .data/.bss.
struct HwProbe final : dbg::Probe {
    std::uint32_t now_ms() override { return systick::millis(); }

    bool power(dbg::PowerSample& s) override {
        s.ms = systick::millis();
        const auto pm = power::read_raw();
        s.valid = pm.has_value();
        if (pm) {
            s.vbus = clamp16(pm->vbus_mv);
            s.ibus = static_cast<std::int16_t>(std::clamp<std::int32_t>(pm->current_ma, -32768, 32767));
        }
        if (const auto a = adc::read()) {   // nullopt while the UART owns CC1/CC2
            s.cc1 = clamp16(a->cc1_mv);
            s.cc2 = clamp16(a->cc2_mv);
            s.dp = clamp16(a->dp_mv);
            s.dm = clamp16(a->dm_mv);
        }
        return true;
    }

    void beat(dbg::BeatInfo& b) override {
        b.stack_free = static_cast<std::uint16_t>(std::min<std::uint32_t>(k2_stack_free_bytes(), 0xFFFF));
        b.msp_free = static_cast<std::uint16_t>(std::min<std::uint32_t>(k2_msp_free_bytes(), 0xFFFF));
        b.page = g_ui ? g_ui->page_index() : 0;
        b.usb_dropped = usb::stats_snapshot().log_dropped;
    }

    std::size_t boot_len() override { return g_boot_len; }
    void boot_read(std::size_t off, std::span<std::uint8_t> out) override {
        if (g_remote) g_remote->boot_text_read(static_cast<std::uint16_t>(off), out);
    }
    std::string_view hash() override { return kGitHash; }
    void boot(dbg::BootInfo& b) override {
        b.boots = session::boots();
        b.reset_flags = sysreset::reset_flags();
    }
};

}  // namespace

void init(Remote& remote, Ui& ui) {
    g_remote = &remote;
    g_ui = &ui;
}

void start(std::uint8_t mask, std::uint16_t hz, bool history) {
    g_boot_len = g_remote ? g_remote->boot_text(true) : 0;
    g_mux.start(mask, hz, history, pdtrace::live_ring(), usb::log_bin_ring(), runev::live(), systick::millis());
}

dbg::Stats stop() {
    g_mux.stop();
    return g_mux.stats();
}

bool running() { return g_mux.running(); }
std::uint8_t stream_mask() { return g_mux.mask(); }
dbg::Stats stats() { return g_mux.stats(); }

unsigned service(std::size_t room, void (*write)(std::span<const std::uint8_t>)) {
    HwProbe p;
    return g_mux.service(pdtrace::live_ring(), usb::log_bin_ring(), runev::live(), p, room,
                         [write](std::span<const std::uint8_t> f) { write(f); });
}

}  // namespace k2::dbg_hw
