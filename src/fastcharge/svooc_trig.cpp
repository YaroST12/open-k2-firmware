// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Guarded SuperVOOC trigger glue, see k2/fastcharge/svooc_trig.hpp.
#include "k2/fastcharge/svooc_trig.hpp"

#include <array>
#include <new>

#include "k2/core/critical.hpp"
#include "k2/core/log.hpp"
#include "k2/core/stage_meter.hpp"
#include "k2/drivers/dpdm.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/mode_ram.hpp"
#include "k2/drivers/oneshot.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include "k2/fastcharge/fc_seq.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/pd/link.hpp"
#include "k2/core/strpack.hpp"

namespace k2::svooc_trig {

namespace {

using svooc::Action;
using svooc::Stage;
using svooc::Next;

alignas(svooc::Run) std::uint8_t g_run_raw[sizeof(svooc::Run)];
bool g_run_inited;
#ifdef K2_WORLD_SVOOC
alignas(svooc::Wire) std::uint8_t g_wire_raw[sizeof(svooc::Wire)];
std::array<std::uint16_t, svooc::kLogPairs> g_pair_vbus{};
std::uint8_t* g_wire_base = g_wire_raw;
std::uint16_t* g_vbus_base = g_pair_vbus.data();
#else
static_assert(alignof(svooc::Wire) <= sizeof(std::uint32_t));
static_assert(sizeof(svooc::Wire) % sizeof(std::uint16_t) == 0);
static_assert(sizeof(svooc::Wire) + svooc::kLogPairs * sizeof(std::uint16_t) <= dpsniff::kHookTailBytes);
std::uint8_t* g_wire_base;
std::uint16_t* g_vbus_base;
std::uint32_t g_wire_gen;
#endif
bool g_wire_inited;
bool g_exit_pending;
std::uint32_t g_wake_ms;   // systick ms of the latest wake (the wire's log clock starts there)
bool g_vbus_owned;   // the sink is told this run owns VBUS (fusb302::sink_set_vbus_external)

svooc::Run& run_obj() {
    if (!g_run_inited) {
        new (g_run_raw) svooc::Run();
        g_run_inited = true;
    }
    return *std::launder(reinterpret_cast<svooc::Run*>(g_run_raw));
}

bool wire_valid() {
#ifdef K2_WORLD_SVOOC
    return g_wire_inited;
#else
    return g_wire_inited && g_wire_gen == dpsniff::hook_buf_gen();
#endif
}

svooc::Wire& wire() {
#ifdef K2_WORLD_SVOOC
    if (!g_wire_inited) {
        new (g_wire_raw) svooc::Wire();
        g_wire_inited = true;
    }
#endif
    return *std::launder(reinterpret_cast<svooc::Wire*>(g_wire_base));
}

// The run owns VBUS: the adapter raises it outside PD while the K2 keeps its own 5 V contract silent, as a phone does. The sink
// must not abort that contract because VBUS rose; the run has its own 12 V ceiling. Handed back once the run is over.
void own_vbus(bool on) {
    if (on == g_vbus_owned) return;
    g_vbus_owned = on;
    fusb302::sink_set_vbus_external(on);
}

// The wake: the phone's burst of the 2026-11-11 capture (reverse-engineered), the one fc_engine's vooc_wake plays on odd
// tries: D+ 40 x (3.3 V 300 us, 0 V 300 us), 24 ms, ending low; D- released. The adapter clocked its first ident
// ~540 ms after it. Built at runtime via fc::fill_vooc_burst to avoid duplicating the 640 B table in flash.

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

// The wire's own clock (Wire::deadline): a one-shot timer interrupt runs tick() at each deadline, so D- goes kHoldUs after a
// reply's last rise however long the main loop is away (a full repaint, a flash write). The timer and the EXTI edge ISR share
// the default NVIC priority: neither preempts the other. g_timer_live keeps a late shot off the lines once release() has
// given them back. The main loop still ticks too (service()): a second caller of an idempotent tick, the fallback if the
// timer ever did not fire. Who let D- go after a reply is counted for the diag line.
volatile bool g_timer_live = false;
volatile std::uint32_t g_rel_timer = 0, g_rel_loop = 0, g_rel_edge = 0;

void on_timer();

void arm_timer() {   // interrupt context or interrupts masked
    if (!g_timer_live) return;
    const auto d = wire().deadline();
    if (!d) return;
    const std::int32_t left = std::int32_t(*d - dpdm::cycles());
    oneshot::arm_us(left <= 0 ? 0u : (std::uint32_t(left) + svooc::kCyclesPerUs - 1) / svooc::kCyclesPerUs, &on_timer);
}

void on_timer() {
    if (!g_timer_live) return;
    const Action a = wire().tick(dpdm::cycles());
    if (a.kind == Action::Kind::Release) g_rel_timer = g_rel_timer + 1;
    apply(a);
    arm_timer();
}

void on_edge(std::uint32_t word) {
    const Action a = wire().edge(word);
    if (a.kind == Action::Kind::Release) g_rel_edge = g_rel_edge + 1;   // the next frame's first edge came first
    apply(a);
    arm_timer();   // every edge may move the deadline
}

stage::Worst g_stall;   // the main loop's worst stage from the wake to the release (k2/core/stage_meter.hpp)

// A line not at the passive idle, or one of the other D+/D- users running. The passive VOOC monitor is not one of them: it
// only listens, so start() stops it (dpsniff::monitor_yield) once the other checks pass. A Black Box arm or stream is: it
// keeps its recording, so the start refuses.
// The monitor's own state is a line mismatch too (both lines Sniff, floating): it is no obstacle (svooc::lines_off_idle).
// One bit per owner, so a Busy refusal's log line names who held the lines: 0x01 the fast-charge engine, 0x02 its scan,
// 0x04 the UFCS probe, 0x08 the release's 0 V hold (dpdm::reset_busy), 0x10 the UFCS exit, 0x20 a line off the passive idle,
// 0x40 a Black Box arm, 0x80 a stream, 0x100 the edge hook.
unsigned lines_owners() {
    const dpdm::Pair d = dpdm::drive();
    const bool monitor_sniff = dpsniff::monitoring() && d.dp == dpdm::Drive::Sniff && d.dm == dpdm::Drive::Sniff;
    return (fctrig::status().state != fc::State::Idle ? 0x01u : 0u) | (fctrig::scan_running() ? 0x02u : 0u) |
           (fctrig::ufcs_probe_running() ? 0x04u : 0u) | (dpdm::reset_busy() ? 0x08u : 0u) | (fctrig::ufcs_exiting() ? 0x10u : 0u) |
           (svooc::lines_off_idle(dpdm::mismatch(d), monitor_sniff) ? 0x20u : 0u) | (dpsniff::armed() ? 0x40u : 0u) |
           (dpsniff::streaming() ? 0x80u : 0u) | (dpsniff::hooked() ? 0x100u : 0u);
}

// VBUS (monitor sample) when each logged pair closed, parallel to the wire's pair list; 0 = no sample. The wire counts
// pairs in the ISR, so the main loop stamps the new ones on its next pass (poll, and once more at the release).
std::size_t g_pair_vbus_n = 0;

void stamp_pair_vbus() {
    if (!wire_valid()) return;
    std::size_t n;
    {
        Critical cs;
        n = wire().log().n;
    }
    if (g_pair_vbus_n >= n) return;
    const auto v = power::vbus_fast_mv();
    const std::uint16_t mv = v && *v > 0 ? std::uint16_t(*v > 0xFFFF ? 0xFFFF : *v) : std::uint16_t(0);
    while (g_pair_vbus_n < n && g_pair_vbus_n < svooc::kLogPairs) g_vbus_base[g_pair_vbus_n++] = mv;
}

struct Live {
    bool host_ok = false, vbus_known = false, current_known = false;
    std::int32_t vbus_mv = 0, ibus_ma = 0;
    bool far_sink = false, inline_mode = false;
    pd::InlineCause inline_cause = pd::InlineCause::None;
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
    l.inline_cause = p.sticky_inline ? p.inline_cause : pd::InlineCause::None;
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
    {
        Critical cs;   // the last wake's wire gets no more shots: the burst drives D+ itself
        g_timer_live = false;
        oneshot::cancel();
    }
    dpsniff::hook_mute();
    dpdm::set_dm(dpdm::Drive::HiZ);
    std::array<dpdm::Step, fc::kVoocBurstSteps> w;
    fc::fill_vooc_burst(w);
    dpdm::play(w);
    dpdm::set_dp(dpdm::Drive::Sniff);
    dpdm::set_dm(dpdm::Drive::Sniff);
    {
        Critical cs;
        wire().begin(dpsniff::line_word());
        g_pair_vbus_n = 0;   // the list restarts with the wire
        g_timer_live = true;
        g_rel_timer = 0;
        g_rel_loop = 0;
        g_rel_edge = 0;
        stage::meter.reset(dpdm::cycles(), systick::millis());   // the stall diag counts from the wake
        g_wake_ms = systick::millis();
    }
    dpsniff::hook_listen(&on_edge);
    K2LOG("[SVOOC] wake %u\n", unsigned(run_obj().report().tries));
}

void release() {
    dpsniff::hook_end();   // the ISR no longer calls the wire; the recording is stored
    {
        Critical cs;
        g_timer_live = false;   // no shot touches the lines after this
        oneshot::cancel();
        wire().stop();
    }
    stamp_pair_vbus();
    dpdm::idle();          // both lines to the passive idle
    const svooc::Report& r = run_obj().report();
    const svooc::Log& lg = wire().log();
    K2LOG("[SVOOC] release: %s t=%ums cmds=%u replies=%u missed=%u cut=%u accepted=%u vmax_held=%dmV imax=%dmA contract=%s>%s inline=%s esc=%s\n",
          svooc::name(r.abort), unsigned(r.elapsed_ms), unsigned(lg.commands), unsigned(lg.replies), unsigned(lg.missed),
          unsigned(lg.cut), r.accepted ? 1u : 0u, int(r.vbus_max_mv), int(r.ibus_max_ma), svooc::name(r.contract_start),
          svooc::name(r.contract_end), pd::inline_cause_name(r.inline_cause), svooc::name(wire().responder().escape()));
    for (const svooc::Responder::Ask& a : wire().responder().asks())
        K2LOG("[SVOOC] ask 86>%03X (%dmA) at %ums, %u polls kept\n", unsigned(a.word), int(svooc::ask_ma(a.word)),
              unsigned(a.t_ms), unsigned(a.after));
    K2LOG("[SVOOC] ceiling %03X (%dmA asked, session kept)\n", unsigned(wire().responder().ceiling_word()),
          int(svooc::ask_ma(wire().responder().ceiling_word())));
    const dpsniff::Status rec = dpsniff::status();
    K2LOG("[SVOOC] diag: edges=%u last_edge=%ums poll_gap_max=%ums rec=%u end=%u\n", unsigned(lg.edges),
          unsigned(lg.last_edge_ms), unsigned(r.poll_gap_max_ms), unsigned(rec.edges), unsigned(rec.reason));
    g_stall = stage::meter.worst();
    K2LOG("[SVOOC] stall: %ums at=%ums stage=%s ticked=%ums release timer=%u loop=%u edge=%u shots=%u\n",
          unsigned(g_stall.us / 1000), unsigned(g_stall.at_ms), stage::name(g_stall.id), unsigned(g_stall.ticked_ms),
          unsigned(g_rel_timer), unsigned(g_rel_loop), unsigned(g_rel_edge), unsigned(oneshot::fired()));
}

void act(Next s) {
    switch (s) {
    case Next::Wake: wake(); break;
    case Next::Rest: K2LOG("[SVOOC] no command after wake %u\n", unsigned(run_obj().report().tries)); break;
    case Next::Release: release(); break;
    case Next::None: break;
    }
}

}  // namespace

svooc::Refusal start(bool confirmed, bool ports_busy, std::uint32_t cap_s, bool usb_a, const svooc::Params& params) {
    if (running()) return svooc::Refusal::Busy;
    const Live l = live();
    svooc::StartFacts f;
    f.confirmed = confirmed;
    f.usb_a = usb_a;
    const unsigned owners = lines_owners();
    f.ports_busy = ports_busy || owners != 0;
    f.host_ok = l.host_ok;
    f.vbus_known = l.vbus_known;
    f.current_known = l.current_known;
    f.vbus_mv = l.vbus_mv;
    f.ibus_ma = l.ibus_ma;
    f.far_sink = l.far_sink;
    f.inline_mode = l.inline_mode;
    f.inline_cause = l.inline_cause;
    f.contract = l.contract;
    const mode_ram::Tenant owner = mode_ram::owner();
    f.ram_free = owner == mode_ram::Tenant::None || owner == mode_ram::Tenant::Sniff;
    bool claimed = false;
    if (svooc::refusal(f) == svooc::Refusal::None) {   // everything else holds: take the RAM and the ISR now
        // The passive monitor listens on the same lines (and hook_claim refuses while it runs): the trigger takes over. It
        // cannot re-arm while the hook is claimed, and fastcharge::poll keeps lines_free false while running().
        dpsniff::monitor_yield();
        switch (dpsniff::hook_claim()) {
        case dpsniff::ArmResult::Ok: claimed = true; break;
        case dpsniff::ArmResult::NoRam: f.ram_free = false; break;
        case dpsniff::ArmResult::Busy: f.ports_busy = true; break;
        }
    }
    const std::uint32_t cap_ms = cap_s == 0 || cap_s * 1000u > svooc::kCapMs ? svooc::kCapMs : cap_s * 1000u;
    const svooc::Refusal r = run_obj().start(f, systick::millis(), cap_ms);
    if (r != svooc::Refusal::None) {
        if (claimed) dpsniff::hook_end();
        K2LOG("[SVOOC] refused: %s vbus=%dmV ibus=%dmA contract=%s inline=%s owners=0x%x%s\n", svooc::name(r), int(f.vbus_mv),
              int(f.ibus_ma), svooc::name(f.contract), pd::inline_cause_name(f.inline_cause), owners,
              usb_a ? K2_P(" (USB-A waiver asked)") : "");
        return r;
    }
    g_exit_pending = true;
    own_vbus(true);
    {
        Critical cs;
#ifndef K2_WORLD_SVOOC
        const auto tail = dpsniff::hook_tail(g_wire_gen);
        g_wire_base = tail.data();
        g_vbus_base = reinterpret_cast<std::uint16_t*>(g_wire_base + sizeof(svooc::Wire));
        new (g_wire_base) svooc::Wire();
        g_wire_inited = true;
#endif
        // The last run's wire is stopped (release() stops it) and holds its log: a fresh one now, so the status shows
        // nothing of it before the wake re-begins the wire (svooc::Run ignores the wire's facts until then too).
        wire().begin(dpsniff::line_word());
        g_pair_vbus_n = 0;
        wire().set_params(params);   // the ISR is not listening yet (wake() starts it); begin() keeps them
        wire().set_vbus_mv(0);
    }
    const svooc::Params& ep = wire().responder().params();
    K2LOG("[SVOOC] start (no load) cap=%ums vbus=%dmV ibus=%dmA contract=%s inline=%s%s i86=%03X/%03X r82=%03X vtarget=%umV esc=%us profile=%s\n",
          unsigned(run_obj().report().cap_ms), int(f.vbus_mv), int(f.ibus_ma), svooc::name(f.contract),
          pd::inline_cause_name(f.inline_cause), usb_a ? K2_P(" far-sink check waived (USB-A)") : "",
          // i86 = the run's first and last 0x86 answer (svooc::ask_word): Probe prints 00F/06D, not the Fixed default 005/005
          unsigned(svooc::ask_word(ep, 0)), unsigned(svooc::ask_word(ep, svooc::ask_count(ep) - 1)), unsigned(ep.r82),
          unsigned(ep.vtarget_mv), unsigned(ep.esc_at_s),
          svooc::name(ep.profile));
    return r;
}

void stop() {
    if (!g_run_inited) return;
    act(run_obj().stop(svooc::Abort::Stopped, systick::millis()));
}

void service() {
    if (!lines_held()) return;
    Critical cs;
    const Action a = wire().tick(dpdm::cycles());
    if (a.kind == Action::Kind::Release) g_rel_loop = g_rel_loop + 1;
    apply(a);
    arm_timer();
}

void poll() {
    if (!g_run_inited) {
        own_vbus(false);
        return;
    }
    const Stage st = run_obj().stage();
    if (st == Stage::Idle || st == Stage::Done) {
        own_vbus(false);   // every exit path ends in Done (or never left Idle): the sink judges VBUS again
        return;
    }
    service();
    stamp_pair_vbus();
    const Live l = live();
    // The closed-loop 0x82 word follows the latest sample: 0 = no reading (the wire then answers the constant word).
    wire().set_vbus_mv(l.vbus_known && l.vbus_mv > 0 ? std::uint16_t(l.vbus_mv > 0xFFFF ? 0xFFFF : l.vbus_mv) : std::uint16_t(0));
    svooc::Sample s;
    s.now_ms = systick::millis();
    s.host_ok = l.host_ok;
    s.vbus_known = l.vbus_known;
    s.current_known = l.current_known;
    s.vbus_mv = l.vbus_mv;
    s.ibus_ma = l.ibus_ma;
    s.far_sink = l.far_sink;
    s.inline_mode = l.inline_mode;
    s.inline_cause = l.inline_cause;
    s.contract = l.contract;
    s.storm = dpsniff::hook_storm();
    {
        Critical cs;
        s.off_script = wire().stopped();
        s.commands = wire().log().commands;
        s.accepted = wire().responder().accepted();
    }
    act(run_obj().poll(s));
    if (run_obj().stage() == Stage::Done && g_exit_pending) {
        g_exit_pending = false;
        const svooc::Report& r = run_obj().report();
        // vmax: the run's one highest VBUS (Report::vbus_max_mv, the exit watch included), the number the charger test records;
        // the release line's vmax_held is the same field read when the lines were let go.
        K2LOG("[SVOOC] exit vbus=%dmV %s vmax=%dmV\n", int(r.vbus_exit_mv), svooc::exit_word(r), int(r.vbus_max_mv));
    }
}

bool running() { return g_run_inited && run_obj().running(); }
bool lines_free() { return !running() && lines_owners() == 0; }
bool lines_held() { return g_run_inited && run_obj().lines_held(); }

Snapshot snapshot() {
    Snapshot s;
    if (g_run_inited) {
        s.stage = run_obj().stage();
        s.report = run_obj().report();
    }
    if (!wire_valid()) return s;
    Critical cs;
    s.phase = wire().responder().phase();
    s.off_run = wire().responder().off_run();
    s.driving = wire().driving();
    s.log = wire().log();
    s.params = wire().responder().params();
    s.escape = wire().responder().escape();
    s.escape_t_ms = wire().responder().escape_t_ms();
    const std::span<const svooc::Responder::Ask> asks = wire().responder().asks();
    s.n_asks = static_cast<std::uint8_t>(asks.size());
    for (std::size_t i = 0; i < asks.size(); ++i) s.asks[i] = asks[i];
    s.ceiling_word = wire().responder().ceiling_word();
    const dpsniff::Status rec = dpsniff::status();
    s.rec_edges = rec.edges;
    s.rec_end = static_cast<std::uint8_t>(rec.reason);
    s.stall = lines_held() ? stage::meter.worst() : g_stall;   // frozen at the release
    s.rel_timer = g_rel_timer;
    s.rel_loop = g_rel_loop;
    s.rel_edge = g_rel_edge;
    if (s.stage == Stage::Answer) {   // the adapter has spoken in this wake: how long since its last edge
        const std::uint32_t since_wake = systick::millis() - g_wake_ms;
        s.quiet_ms = since_wake > s.log.last_edge_ms ? since_wake - s.log.last_edge_ms : 0;
    }
    return s;
}

std::size_t read_pairs(std::uint16_t from, std::span<svooc::PairRow> out) {
    if (!wire_valid()) return 0;
    Critical cs;
    const std::size_t n = wire().log().n;
    std::size_t k = 0;
    for (; k < out.size() && from + k < n; ++k) {
        const svooc::LogPair& p = wire().pair(from + k);
        out[k] = {p.cmd, p.word, p.t_ms, from + k < g_pair_vbus_n ? g_vbus_base[from + k] : std::uint16_t(0)};
    }
    return k;
}

}  // namespace k2::svooc_trig
