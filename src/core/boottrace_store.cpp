// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// See k2/core/boottrace_store.hpp. Backup registers (BPR at 0x40006C00): DR n for n = 1..10 at 4 * n,
// DR11.. at 0x40 + 4 * (n - 11). Kept apart from the PD session record's DR22..DR37: DR4..DR10, DR19, DR20,
// DR38..DR42 = 14 halfwords.
#include "k2/core/log.hpp"
#include "k2/core/boottrace_store.hpp"

#include <array>

#include "k2/drivers/adc.hpp"
#include "k2/core/boottrace.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::boottrace {
namespace {

constexpr std::uint32_t kBpr = 0x40006C00;
constexpr std::uint32_t kPwr = 0x40007000;
constexpr std::uint32_t kApb1enPwr = 1u << 28, kApb1enBpr = 1u << 27;
constexpr std::uint32_t kPwrDbp = 1u << 8;

constexpr std::array<std::uint16_t, kWords> kOffsets = {
    0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28,    // DR4..DR10
    0x60, 0x64,                                  // DR19, DR20
    0xAC, 0xB0, 0xB4, 0xB8, 0xBC,                // DR38..DR42
};

constexpr std::size_t kMaxSamples = kSpanMs / kPeriodMs + 1;
constexpr std::uint32_t kSensorPeriodMs = 10;

volatile std::uint32_t& bpr(std::size_t i) { return *reinterpret_cast<volatile std::uint32_t*>(kBpr + kOffsets[i]); }

std::array<std::uint16_t, kWords> load() {
    std::array<std::uint16_t, kWords> w{};
    for (std::size_t i = 0; i < kWords; i++) w[i] = std::uint16_t(bpr(i));
    return w;
}

void store(const Trace& t) {
    const auto w = encode(t);
    for (std::size_t i = 0; i < kWords; i++) bpr(i) = w[i];
}

std::optional<Trace> g_prev;
Trace g_trace;
bootmode::Mode g_mode = bootmode::Mode::Plain;
std::array<Sample, kMaxSamples> g_samples;    // this boot, one per period
std::size_t g_taken, g_printed;
bool g_active, g_busy, g_move_logged, g_done_logged;
std::uint8_t g_moved_to;                       // the line the key mode's single Rd moved to (0 = it stayed)
bootinfo::Move g_move;                         // the rule's inputs at the move, for `bootinfo`
std::uint32_t g_t0, g_next_sample_ms, g_next_sensor_ms;
bool g_vbus_seen;                              // power monitor answered at least once

Sample measure() {
    Sample s;
    if (const auto pm = power::read()) {
        g_vbus_seen = true;
        s.vbus_mv = std::uint16_t(pm->vbus_mv < 0 ? 0 : pm->vbus_mv > 65535 ? 65535 : pm->vbus_mv);
    }
    if (const auto a = adc::read()) {
        auto cl = [](std::int32_t v) { return std::uint16_t(v < 0 ? 0 : v > 65535 ? 65535 : v); };
        s.aux_mv = cl(a->aux_mv);
        s.cc1_mv = cl(a->cc1_mv);
        s.cc2_mv = cl(a->cc2_mv);
    }
    s.plus = !pins::key_plus.read();
    return s;
}

}  // namespace

void init(bootmode::Mode mode) {
    g_mode = mode;
    reg::crm().APB1EN |= kApb1enPwr | kApb1enBpr;
    *reinterpret_cast<volatile std::uint32_t*>(kPwr) |= kPwrDbp;
    g_prev = decode(load());
    g_trace = Trace{};
    g_trace.mode = static_cast<std::uint8_t>(mode);
    store(g_trace);

    adc::init();
    power::init();
    g_t0 = systick::millis();
    g_next_sample_ms = 0;
    g_next_sensor_ms = 0;
    g_active = true;
    systick::set_delay_hook(poll);
    poll();
}

void poll() {
    if (!g_active || g_busy) return;
    const std::uint32_t t = systick::millis() - g_t0;
    if (t >= g_next_sensor_ms) {
        g_next_sensor_ms = t + kSensorPeriodMs;
        g_busy = true;
        if (power::chip() != power::Chip::None) power::poll();   // never the probe-and-retry path (it waits)
        adc::poll();
        g_busy = false;
    }
    if (t < g_next_sample_ms) return;
    g_busy = true;
    g_next_sample_ms += kPeriodMs;
    const Sample s = measure();
    if (g_taken < kMaxSamples) g_samples[g_taken++] = s;
    g_trace.push(s);
    store(g_trace);
    // A key mode asks for VBUS with ONE Rd (PLUS: CC2, OK: CC1). No VBUS after kMoveAfterMs, no Rp on that line and an Rp on the
    // other: the charger sits on the other orientation, so the Rd moves there, once. Never Rd on both lines.
    if (g_moved_to == 0 && g_mode != bootmode::Mode::Plain) {
        const std::int32_t vbus = g_vbus_seen ? std::int32_t(s.vbus_mv) : -1;
        const std::uint8_t pin = fusb302::pin_rd();
        const std::uint8_t to = bootmode::moved_pin(pin, t, vbus, s.cc1_mv, s.cc2_mv);
        if (to != pin && fusb302::self_power_move(to)) {
            g_moved_to = to;
            g_move = {true, std::uint16_t(t), pin, to, vbus, s.cc1_mv, s.cc2_mv};
        }
    }
    fusb302::boot_readback_service();   // SWITCHES0 read back at ~50 / ~600 ms, even before the main loop runs
    if (t >= kSpanMs) {
        g_active = false;
        systick::set_delay_hook(nullptr);
    }
    g_busy = false;
}

void flush_log() {
    for (int n = 0; n < 2 && g_printed < g_taken; n++, g_printed++) {
        char line[80];
        format_sample(line, sizeof line, g_samples[g_printed], std::uint32_t(g_printed) * kPeriodMs);
        K2LOG("[BOOT] %s\n", line);
    }
    if (g_moved_to != 0 && !g_move_logged) {
        g_move_logged = true;
        K2LOG("[BOOT] no VBUS after %u ms, Rp only on the other line: Rd moved to CC%d (one Rd, never both)\n",
              static_cast<unsigned>(bootmode::kMoveAfterMs), static_cast<int>(g_moved_to));
    }
    if (!g_active && g_printed == g_taken && g_taken != 0 && !g_done_logged) {
        g_done_logged = true;        // printed once; the samples stay for `bootinfo`
        K2LOG("[BOOT] trace done: %d samples\n", static_cast<int>(g_taken));
    }
}

std::size_t sample_count() { return g_taken; }

Sample sample_at(std::size_t i) { return i < g_taken ? g_samples[i] : Sample{}; }

bootinfo::Move move_record() { return g_move; }

void report_previous() {
    if (!g_prev) {
        K2LOG("[SESS] no previous boot trace\n");
        return;
    }
    const std::size_t n = g_prev->stored();
    K2LOG("[SESS] last boot trace: mode=%d samples=%u, newest last (t = sample * 50 ms)\n", g_prev->mode,
                 static_cast<unsigned>(g_prev->count));
    for (std::size_t j = 0; j < n; j++) {
        const std::size_t i = n - 1 - j;                              // oldest first
        const std::uint32_t idx = std::uint32_t(g_prev->count) - 1 - i;
        char line[80];
        format_sample(line, sizeof line, unpack(g_prev->at(i)), idx * kPeriodMs);
        K2LOG("[SESS] %s\n", line);
    }
}

}  // namespace k2::boottrace
