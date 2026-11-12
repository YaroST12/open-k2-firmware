// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Guarded SuperVOOC trigger glue, see k2/fastcharge/svooc_trig.hpp.
#include "k2/fastcharge/svooc_trig.hpp"

#include <array>

#include "k2/core/critical.hpp"
#include "k2/core/log.hpp"
#include "k2/drivers/dpdm.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/mode_ram.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/pd/link.hpp"

namespace k2::svooc_trig {

namespace {

using svooc::Action;
using svooc::Stage;
using svooc::Next;

svooc::Run g_run;
svooc::Wire g_wire;      // fed by the ISR; the main loop reaches it with interrupts masked
bool g_exit_logged = true;
bool g_vbus_owned = false;   // the sink is told this run owns VBUS (fusb302::sink_set_vbus_external)

// The run owns VBUS: the adapter raises it outside PD while the K2 keeps its own 5 V contract silent, as a phone does. The sink
// must not abort that contract because VBUS rose; the run has its own 12 V ceiling. Handed back once the run is over.
void own_vbus(bool on) {
    if (on == g_vbus_owned) return;
    g_vbus_owned = on;
    fusb302::sink_set_vbus_external(on);
}

// The wake: the phone's burst of the 2026-11-11 capture (reverse-engineered), the one fc_engine's vooc_wake plays on odd
// tries: D+ 40 x (3.3 V 300 us, 0 V 300 us), 24 ms, ending low; D- released. The adapter clocked its first ident
// ~540 ms after it. In flash, not on the stack.
constexpr std::size_t kBurstSteps = 80;
constexpr auto kBurst = [] {
    std::array<dpdm::Step, kBurstSteps> w{};
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = {i % 2 == 0 ? dpdm::Drive::V3p3 : dpdm::Drive::V0, dpdm::Drive::HiZ, 300};
    return w;
}();

// The ISR's thin hardware part (also called from the main loop with interrupts masked). Bit: wait from now to `at`
// (the D+ rise + 50 us; never longer than that, since `at` is the edge's stamp + 50 us), then set D-.
void apply(const Action& a) {
    switch (a.kind) {
    case Action::Kind::Take: dpdm::set_dm(dpdm::Drive::V3p3); break;
    case Action::Kind::Bit: {
        constexpr std::int32_t kMax = std::int32_t(svooc::kBitDelayUs * svooc::kCyclesPerUs);
        const std::int32_t left = std::int32_t(a.at - dpdm::cycles());
        if (left > 0 && left <= kMax)
            while (std::int32_t(a.at - dpdm::cycles()) > 0) {}
        dpdm::set_dm(a.level ? dpdm::Drive::V3p3 : dpdm::Drive::V0);
        break;
    }
    case Action::Kind::Release: dpdm::set_dm(dpdm::Drive::Sniff); break;   // floating: the line idles high on its own
    case Action::Kind::None: break;
    }
}

void on_edge(std::uint32_t word) { apply(g_wire.edge(word)); }

// A line not at the passive idle, or one of the other D+/D- users running.
bool lines_taken() {
    return fctrig::status().state != fc::State::Idle || fctrig::scan_running() || fctrig::ufcs_probe_running() ||
           dpdm::reset_busy() || dpdm::mismatch(dpdm::drive()) != 0 || dpsniff::armed() || dpsniff::streaming() ||
           dpsniff::monitoring() || dpsniff::hooked();
}

struct Live {
    bool host_ok = false, vbus_known = false, current_known = false;
    std::int32_t vbus_mv = 0, ibus_ma = 0;
    bool far_sink = false, inline_mode = false;
    svooc::Contract contract = svooc::Contract::None;
};

// VBUS and current of the latest monitor sample (no average, no display filter).
Live live() {
    Live l;
    l.host_ok = usb::configured();
    if (const auto v = power::vbus_fast_mv()) { l.vbus_known = true; l.vbus_mv = *v; }
    if (const auto m = power::read_raw()) { l.current_known = true; l.ibus_ma = m->current_ma; }
    const link::Probe p = link::probe_now();
    l.inline_mode = p.sticky_inline;
    l.far_sink = fusb302::over_load_facts().far_sink;
    // The contract: the K2's own 5 V fixed one (PDO 1 as sink) is allowed, anything else is not (svooc::classify).
    const pd::SinkStatus& ss = fusb302::sink_status();
    svooc::ContractFacts c;
    c.pd_active = p.pd_active;
    c.held = ss.held;
    c.ready = ss.state == pd::SinkState::Ready;
    c.epr_mode = ss.epr_mode;
    c.inline_mode = l.inline_mode;
    c.far_sink = l.far_sink;
    c.pos = ss.pos;
    c.mv = ss.mv;
    l.contract = svooc::classify(c);
    return l;
}

void wake() {
    dpsniff::hook_mute();
    dpdm::set_dm(dpdm::Drive::HiZ);
    dpdm::play(kBurst);
    dpdm::set_dp(dpdm::Drive::Sniff);
    dpdm::set_dm(dpdm::Drive::Sniff);
    {
        Critical cs;
        g_wire.begin(dpsniff::line_word());
    }
    dpsniff::hook_listen(&on_edge);
    K2LOG("[SVOOC] wake %u\n", unsigned(g_run.report().tries));
}

void release() {
    dpsniff::hook_end();   // the ISR no longer calls the wire; the recording is stored
    {
        Critical cs;
        g_wire.stop();
    }
    dpdm::idle();          // both lines to the passive idle
    const svooc::Report& r = g_run.report();
    const svooc::Log& lg = g_wire.log();
    K2LOG("[SVOOC] release: %s t=%ums cmds=%u replies=%u missed=%u cut=%u accepted=%u vmax=%dmV imax=%dmA contract=%s>%s\n",
          svooc::name(r.abort), unsigned(r.elapsed_ms), unsigned(lg.commands), unsigned(lg.replies), unsigned(lg.missed),
          unsigned(lg.cut), r.accepted ? 1u : 0u, int(r.vbus_max_mv), int(r.ibus_max_ma), svooc::name(r.contract_start),
          svooc::name(r.contract_end));
}

void act(Next s) {
    switch (s) {
    case Next::Wake: wake(); break;
    case Next::Rest: K2LOG("[SVOOC] no command after wake %u\n", unsigned(g_run.report().tries)); break;
    case Next::Release: release(); break;
    case Next::None: break;
    }
}

}  // namespace

svooc::Refusal start(bool confirmed, bool ports_busy, std::uint32_t cap_s) {
    if (g_run.running()) return svooc::Refusal::Busy;
    const Live l = live();
    svooc::StartFacts f;
    f.confirmed = confirmed;
    f.ports_busy = ports_busy || lines_taken();
    f.host_ok = l.host_ok;
    f.vbus_known = l.vbus_known;
    f.current_known = l.current_known;
    f.vbus_mv = l.vbus_mv;
    f.ibus_ma = l.ibus_ma;
    f.far_sink = l.far_sink;
    f.inline_mode = l.inline_mode;
    f.contract = l.contract;
    const mode_ram::Tenant owner = mode_ram::owner();
    f.ram_free = owner == mode_ram::Tenant::None || owner == mode_ram::Tenant::Sniff;
    bool claimed = false;
    if (svooc::refusal(f) == svooc::Refusal::None) {   // everything else holds: take the RAM and the ISR now
        switch (dpsniff::hook_claim()) {
        case dpsniff::ArmResult::Ok: claimed = true; break;
        case dpsniff::ArmResult::NoRam: f.ram_free = false; break;
        case dpsniff::ArmResult::Busy: f.ports_busy = true; break;
        }
    }
    const std::uint32_t cap_ms = cap_s == 0 || cap_s * 1000u > svooc::kCapMs ? svooc::kCapMs : cap_s * 1000u;
    const svooc::Refusal r = g_run.start(f, systick::millis(), cap_ms);
    if (r != svooc::Refusal::None) {
        if (claimed) dpsniff::hook_end();
        K2LOG("[SVOOC] refused: %s vbus=%dmV ibus=%dmA contract=%s\n", svooc::name(r), int(f.vbus_mv), int(f.ibus_ma),
              svooc::name(f.contract));
        return r;
    }
    g_exit_logged = false;
    own_vbus(true);
    K2LOG("[SVOOC] start (no load) cap=%ums vbus=%dmV ibus=%dmA contract=%s\n", unsigned(g_run.report().cap_ms),
          int(f.vbus_mv), int(f.ibus_ma), svooc::name(f.contract));
    return r;
}

void stop() { act(g_run.stop(svooc::Abort::Stopped, systick::millis())); }

void service() {
    if (!g_run.lines_held()) return;
    Critical cs;
    apply(g_wire.tick(dpdm::cycles()));
}

void poll() {
    const Stage st = g_run.stage();
    if (st == Stage::Idle || st == Stage::Done) {
        own_vbus(false);   // every exit path ends in Done (or never left Idle): the sink judges VBUS again
        return;
    }
    service();
    const Live l = live();
    svooc::Sample s;
    s.now_ms = systick::millis();
    s.host_ok = l.host_ok;
    s.vbus_known = l.vbus_known;
    s.current_known = l.current_known;
    s.vbus_mv = l.vbus_mv;
    s.ibus_ma = l.ibus_ma;
    s.far_sink = l.far_sink;
    s.inline_mode = l.inline_mode;
    s.contract = l.contract;
    s.storm = dpsniff::hook_storm();
    {
        Critical cs;
        s.off_script = g_wire.stopped();
        s.commands = g_wire.log().commands;
        s.accepted = g_wire.responder().accepted();
    }
    act(g_run.poll(s));
    if (g_run.stage() == Stage::Done && !g_exit_logged) {
        g_exit_logged = true;
        const svooc::Report& r = g_run.report();
        K2LOG("[SVOOC] exit vbus=%dmV %s\n", int(r.vbus_exit_mv), r.exit_ok ? "back at 5 V" : "STILL HIGH");
    }
}

bool running() { return g_run.running(); }
bool lines_held() { return g_run.lines_held(); }

Snapshot snapshot() {
    Snapshot s;
    s.stage = g_run.stage();
    s.report = g_run.report();
    Critical cs;
    s.phase = g_wire.responder().phase();
    s.off_run = g_wire.responder().off_run();
    s.driving = g_wire.driving();
    s.log = g_wire.log();
    return s;
}

}  // namespace k2::svooc_trig
