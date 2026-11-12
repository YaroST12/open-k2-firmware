// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The SuperVOOC world, see tests/world_svooc.h.
#include "world_svooc.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "ui_stubs.h"
#include "world_sim.h"
#include "k2/drivers/dpdm.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/mode_ram.hpp"
#include "k2/drivers/oneshot.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/fastcharge/edges.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/fastcharge/svooc_trig.hpp"
#include "k2/pd/link.hpp"

namespace wsvooc {
namespace {

// Hardware run 8 (2026-11-12, runs/svooc-trigger-hw/run8_long.log.txt): the commands up to the 0x86 and their start, ms after
// the wake; then 0x82 every 44 ms.
struct Cmd { std::uint8_t cmd; double t_ms; };
constexpr std::array<Cmd, 34> kRun8{{
    {0xA8, 536}, {0x98, 580}, {0x98, 603}, {0x9E, 626}, {0x80, 650}, {0x94, 674}, {0x98, 698}, {0x8A, 743}, {0x98, 767},
    {0x8E, 791}, {0x9A, 835}, {0x82, 870}, {0x82, 905}, {0x84, 941}, {0x84, 976}, {0x86, 1011}, {0x86, 1046}, {0x88, 1081},
    {0x88, 1116}, {0x8A, 1151}, {0x8A, 1187}, {0x8C, 1222}, {0x8C, 1257}, {0x88, 1292}, {0x94, 1336}, {0x84, 1411},
    {0x84, 1445}, {0x84, 1479}, {0x84, 1514}, {0x84, 1548}, {0x84, 1582}, {0x84, 1616}, {0x84, 1650}, {0x86, 1693},
}};
constexpr double kPollFirstMs = 1737, kPollEveryMs = 44, kScriptEndMs = 12000;
constexpr double kSlotAfterUs = 9000, kPulseUs = 30, kBitAfterFallUs = 28, kSampleBeforeFallUs = 10, kLastSampleUs = 300;
constexpr double kVbus5 = 5270;

enum class Ev : std::uint8_t { Dp, Dm, Sample, Check, LastRise };
struct Event { double t; Ev k; int v; int idx; };

Config g_cfg;
Result g_res;
std::vector<Event> g_q;   // sorted by time
std::size_t g_qi = 0;
double g_t0_world = 0;    // world ms at firmware 0

// Lines: -1 = not driven.
int g_k2_dp = -1, g_k2_dm = -1, g_a_dp = 1, g_a_dm = -1;
bool g_listening = false, g_owned = false, g_pending = false;
double g_pend_t = 0;
k2::dpsniff::Hook g_hook = nullptr;
k2::dpdm::Pair g_drive = k2::dpdm::kIdle;
double g_timer_at = -1;
k2::oneshot::Handler g_timer_h = nullptr;
double g_last_rise = -1;   // the latest slot's last rise, until the K2 lets D- go
int g_burst_toggles = 0;
k2::pd::InlineCause g_inline_cause = k2::pd::InlineCause::None;
bool g_stalled = false;

double now_us() { return world::fw_ms() * 1000.0; }
int dp_level() { return g_k2_dp >= 0 ? g_k2_dp : g_a_dp; }
int dm_level() { return g_k2_dm >= 0 ? g_k2_dm : g_a_dm >= 0 ? g_a_dm : 1; }

// A level change on either line while the trigger listens: one EXTI pending (the first one's time).
template <class F> void change(double t, F&& f) {
    const int dp = dp_level(), dm = dm_level();
    f();
    if (g_k2_dm >= 0 && g_a_dm >= 0 && g_k2_dm != g_a_dm) g_res.contention++;
    if ((dp != dp_level() || dm != dm_level()) && g_listening && !g_pending) { g_pending = true; g_pend_t = t; }
}

void end_session(double t, const char* why) {
    if (g_res.ended) return;
    g_res.ended = true;
    g_res.ended_ms = t / 1000.0 - g_res.wake_end_ms;
    g_res.why = why;
    g_q.resize(g_qi);   // no more clock
    change(t, [] { g_a_dp = 1; g_a_dm = -1; });
}

void add(double t, Ev k, int v = 0, int idx = 0) { g_q.push_back({t, k, v, idx}); }

// The adapter's whole script from the wake burst's end `t0` (firmware us).
void schedule(double t0) {
    g_q.clear();
    g_qi = 0;
    change(t0, [] { g_a_dp = 0; });   // D+ held low until the first ident's rise (the burst ended low: no edge)
    std::vector<Cmd> cmds(kRun8.begin(), kRun8.end());
    for (double t = kPollFirstMs; t < kScriptEndMs; t += kPollEveryMs) cmds.push_back({0x82, t});
    g_res.pairs.clear();
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        const double s = t0 + cmds[i].t_ms * 1000.0;
        g_res.pairs.push_back({cmds[i].cmd, 0, cmds[i].t_ms});
        double f = i == 0 ? s - kPulseUs : s;   // the first ident: D+ already low, pulse 1 is only its rise
        if (i > 0) add(s - 1, Ev::Check);
        double rise = 0;
        for (int k = 0; k < 9; ++k) {
            if (k > 0 || i > 0) add(f, Ev::Dp, 0);
            add(f + kBitAfterFallUs, Ev::Dm, k < 8 ? (cmds[i].cmd >> (7 - k)) & 1 : 0);
            rise = f + kPulseUs;
            add(rise, Ev::Dp, 1);
            f += k % 2 == 0 ? 460 : 540;
        }
        add(rise + 200, Ev::Dm, -1);
        double g = s + kSlotAfterUs;
        for (int k = 0; k < 9; ++k) {
            add(g, Ev::Dp, 0);
            add(g + kPulseUs, Ev::Dp, 1);
            const double next = g + (k % 2 == 0 ? 460 : 540);
            if (k < 8) add(next - kSampleBeforeFallUs, Ev::Sample, 0, int(i));
            else {
                add(g + kPulseUs, Ev::LastRise, 0, int(i));
                add(g + kPulseUs + kLastSampleUs, Ev::Sample, 0, int(i));
            }
            g = next;
        }
    }
    std::stable_sort(g_q.begin(), g_q.end(), [](const Event& a, const Event& b) { return a.t < b.t; });
}

void process(const Event& e) {
    switch (e.k) {
    case Ev::Dp: change(e.t, [&] { g_a_dp = e.v; }); break;
    case Ev::Dm: change(e.t, [&] { g_a_dm = e.v; }); break;
    case Ev::Sample: {
        Pair& p = g_res.pairs[std::size_t(e.idx)];
        p.word = std::uint16_t(p.word << 1 | (dm_level() ? 1 : 0));
        break;
    }
    case Ev::Check:
        if (g_k2_dm >= 0) end_session(e.t, "the K2 still drove D- at the next command");
        break;
    case Ev::LastRise: g_last_rise = e.t; break;
    }
}

struct Source final : world::IrqSource {
    double next_us() override {
        double n = -1;
        auto take = [&](double t) { if (t >= 0 && (n < 0 || t < n)) n = t; };
        if (g_pending && g_listening) take(g_pend_t);
        take(g_timer_at);
        if (g_qi < g_q.size()) take(g_q[g_qi].t);
        return n;
    }
    void world_to(double t) override {
        while (g_qi < g_q.size() && g_q[g_qi].t <= t + 1e-6) process(g_q[g_qi++]);
    }
    void fire() override {
        const double now = now_us();
        if (g_timer_at >= 0 && g_timer_at <= now + 1e-6) {
            g_timer_at = -1;
            g_res.timer_shots++;
            world::spend_us(g_cfg.isr_entry_us);
            if (g_timer_h) g_timer_h();
            return;
        }
        if (g_pending && g_listening && g_pend_t <= now + 1e-6) {
            g_pending = false;   // the ISR clears both lines' pending bits
            world::spend_us(g_cfg.isr_entry_us);
            const std::uint32_t c = k2::dpdm::cycles();
            const std::uint32_t w = k2::edges::pack(c, dm_level() != 0, dp_level() != 0);
            g_res.edges++;
            if (g_hook) g_hook(w);
        }
    }
} g_src;

double vbus_model(double world_ms) {
    const double fw = world_ms - g_t0_world;
    if (g_res.wake_end_ms < 0) return kVbus5;
    if (g_res.ended && fw - g_res.wake_end_ms > g_res.ended_ms + 300) return kVbus5;
    const double a = fw - g_res.wake_end_ms;   // ms after the wake
    struct P { double t, mv; };
    static constexpr P kP[] = {{0, kVbus5}, {1340, kVbus5}, {1400, 7980}, {1650, 8520}, {1740, 8520}, {2410, 11100}};
    if (a <= kP[0].t) return kP[0].mv;
    for (std::size_t i = 1; i < std::size(kP); ++i)
        if (a <= kP[i].t) return kP[i - 1].mv + (kP[i].mv - kP[i - 1].mv) * (a - kP[i - 1].t) / (kP[i].t - kP[i - 1].t);
    return kP[std::size(kP) - 1].mv;
}

int level_of(k2::dpdm::Drive d) {
    using D = k2::dpdm::Drive;
    switch (d) {
    case D::V3p3: case D::V2p0: return 1;
    case D::V0: case D::V0p6: return 0;
    default: return -1;   // HiZ, Sniff, Listen, RxPulldown: released
    }
}

}  // namespace

void install(const Config& c) {
    g_cfg = c;
    g_res = {};
    g_q.clear();
    g_qi = 0;
    g_t0_world = world::world_ms() - world::fw_ms();
    g_k2_dp = g_k2_dm = -1;
    g_a_dp = 1;
    g_a_dm = -1;
    g_listening = g_owned = g_pending = false;
    g_hook = nullptr;
    g_drive = k2::dpdm::kIdle;
    g_timer_at = -1;
    g_timer_h = nullptr;
    g_last_rise = -1;
    g_stalled = false;
    g_stub.usb_configured = true;   // the HID host: the run's guard wants it
    world::set_irq_source(&g_src);
    world::set_vbus_model(vbus_model);
}

void uninstall() { world::set_irq_source(nullptr); }
const Result& result() { return g_res; }
double wake_ms() { return g_res.wake_end_ms; }

}  // namespace wsvooc

// ---- the firmware's view: the parts of the D+/D- side the trigger glue reaches ----
namespace k2 {
using namespace wsvooc;

namespace dpdm {
void set_dp(Drive d) {
    const int v = level_of(d);
    if (v >= 0 && g_k2_dp >= 0 && v != g_k2_dp) ++g_burst_toggles;
    change(now_us(), [&] { g_k2_dp = v; });
    g_drive.dp = d;
}
void set_dm(Drive d) {
    const int v = level_of(d);
    const double t = now_us();
    if (g_k2_dm >= 0 && v < 0 && g_last_rise >= 0) {   // the K2 lets D- go after a slot
        const double hold = t - g_last_rise;
        if (hold > g_res.hold_max_us) { g_res.hold_max_us = hold; g_res.hold_max_at_ms = t / 1000.0 - g_res.wake_end_ms; }
        g_last_rise = -1;
    }
    change(t, [&] { g_k2_dm = v; });
    g_drive.dm = d;
}
Pair drive() { return g_drive; }
void idle() { set_dp(Drive::HiZ); set_dm(Drive::HiZ); }
bool reset_busy() { return false; }
// The burst runs with interrupts masked (src/drivers/dpdm.cpp play); a burst of ten or more D+ toggles is the wake the
// adapter answers.
void play(std::span<const Step> steps) {
    g_burst_toggles = 0;
    for (const Step& s : steps) {
        set_dp(s.dp);
        set_dm(s.dm);
        world::spend_masked_us(s.us);
    }
    if (g_burst_toggles >= 10 && g_res.wake_end_ms < 0) {
        g_res.wake_end_ms = world::fw_ms();
        schedule(now_us());
    }
}
}  // namespace dpdm

namespace dpsniff {
ArmResult hook_claim() {
    if (g_owned) return ArmResult::Busy;
    g_owned = true;
    return ArmResult::Ok;
}
void hook_listen(Hook h) {
    dpdm::set_dp(dpdm::Drive::Sniff);
    dpdm::set_dm(dpdm::Drive::Sniff);
    g_hook = h;
    g_listening = true;
    g_pending = false;
}
void hook_mute() { g_hook = nullptr; g_listening = false; g_pending = false; }
void hook_end() { g_hook = nullptr; g_listening = false; g_pending = false; g_owned = false; }
bool hooked() { return g_owned; }
bool hook_storm() { return false; }
std::uint32_t line_word() { return edges::pack(dpdm::cycles(), dm_level() != 0, dp_level() != 0); }
bool monitoring() { return false; }
bool armed() { return false; }
bool streaming() { return false; }
bool monitor_yield() { return false; }
Status status() {
    Status s;
    s.edges = std::uint16_t(std::min<unsigned>(g_res.edges, kCapacity));
    s.reason = g_owned ? edges::End::None : edges::End::Cancel;
    return s;
}
}  // namespace dpsniff

namespace oneshot {
void arm_us(std::uint32_t us, Handler h) {
    if (!g_cfg.timer) return;   // the firmware before the timer
    us = std::clamp<std::uint32_t>(us, kMinUs, kMaxUs);
    g_timer_at = now_us() + us;
    g_timer_h = h;
}
void cancel() { g_timer_at = -1; g_timer_h = nullptr; }
std::uint32_t fired() { return g_res.timer_shots; }
}  // namespace oneshot

namespace fctrig {
void poll() { svooc_trig::poll(); }
bool svooc_running() { return svooc_trig::running(); }
void service_ufcs() { svooc_trig::service(); }
void service_hs() {}
}  // namespace fctrig

namespace mode_ram {
Tenant owner() { return Tenant::None; }
}  // namespace mode_ram

namespace link {
void set_inline_mode(bool on, pd::InlineCause cause) {
    g_stub.link_inline = on;
    g_inline_cause = on ? cause : pd::InlineCause::None;
}
Probe probe_now() {
    Probe p;
    if (const auto m = power::read()) {
        p.vbus_known = p.current_known = true;
        p.vbus_mv = m->vbus_mv;
        p.current_ma = m->current_ma;
    }
    p.pd_active = fusb302::caps().valid && fusb302::caps().active_cc != 0;
    p.cc_active = fusb302::caps().active_cc != 0;
    p.sticky_inline = g_stub.link_inline;
    p.inline_cause = g_inline_cause;
    return p;
}
}  // namespace link

// The experiment's main-loop stall, in a poll every pass makes (the stage meter names it "shell").
namespace shell_hw {
void poll() {
    if (g_stalled || g_cfg.stall_at_ms < 0 || g_res.wake_end_ms < 0) return;
    if (world::fw_ms() - g_res.wake_end_ms < g_cfg.stall_at_ms) return;
    g_stalled = true;
    if (g_cfg.stall_masked) world::spend_masked_us(g_cfg.stall_ms * 1000.0);
    else world::spend_us(g_cfg.stall_ms * 1000.0);
}
}  // namespace shell_hw

}  // namespace k2
