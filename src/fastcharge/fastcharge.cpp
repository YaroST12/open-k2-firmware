// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/log.hpp"
#include "k2/fastcharge/fastcharge.hpp"

#include "k2/drivers/adc.hpp"
#include "k2/drivers/dpdm.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/pd/link.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::fastcharge {

namespace {
Ring g_ring;
Protocol g_verdict = Protocol::Pending;
// Any D+/D- handshake pattern seen since the current source attached (the ring is cleared when VBUS
// drops below presence, so the ring's lifetime is the attach). Fed to classify() as cc_attach_quiet.
bool g_handshake_seen = false;
SagGuard g_sag;   // holds the last verdict through an output sag or cut (see SagGuard)

// Active DCP check (see dcp_check_result): once per attach, only for a quiet pair. The check plays the three
// dpdm::kDcpProbe steps one per poll (about 60 ms in all): Clear, Ref (D- read on the next poll as the reference),
// Pull (D- read on the next poll as the probed level), then restores the passive idle; nothing waits in a loop.
std::size_t g_dcp_step = 0;     // probe steps applied so far; 0 = no check running
bool g_dcp_done = false;        // the check ran for this attach
bool g_dcp_proof = false;       // it found D- following D+: a real short
int g_dcp_ref_dm_mv = 0;        // D- in the Ref step
int g_ring_skip = 0;            // polls to keep out of the ring after the probe (the averaged ADC reading lags)

void dcp_reset() {
    if (g_dcp_step) dpdm::dcp_probe_end(g_dcp_step - 1);
    g_dcp_step = 0;
    g_dcp_done = g_dcp_proof = false;
    g_ring_skip = 0;
}

bool engine_idle() {
    return fctrig::status().state == fctrig::State::Idle && !fctrig::scan_running() && !fctrig::ufcs_probe_running() &&
           !dpdm::reset_busy() && !dpsniff::armed() && !fctrig::svooc_running();
}

void dcp_abort(bool have, bool idle, bool ours) {
    if (ours) dpdm::dcp_probe_end(g_dcp_step - 1);
    g_dcp_step = 0;
    g_ring_skip = 2;
    K2LOG("[FC] dcp check aborted have=%d idle=%d ours=%d", have, idle, ours);
}

// One poll of a running check: read D- from the newest sweeps, then apply the next step, or restore the passive
// idle and decide after the last one. The probe's samples stay out of the ring, and so do the next two after it
// (the averaged ADC reading still holds the driven level).
void dcp_advance() {
    g_dcp_done = true;
    std::int32_t dp = 0, dm = 0;
    const bool have = adc::latest_dpdm(dp, dm);
    const bool idle = engine_idle();
    const std::size_t last = g_dcp_step - 1;
    const bool ours = dpdm::drive() == dpdm::kDcpProbe[last];   // false when another owner rewrote the lines meanwhile
    if (!have || !idle || !ours) return dcp_abort(have, idle, ours);
    if (last == 1) g_dcp_ref_dm_mv = dm;                         // D- in the Ref step
    if (g_dcp_step < dpdm::kDcpProbe.size()) {
        dpdm::dcp_probe_step(g_dcp_step);
        g_dcp_step++;
        return;
    }
    dpdm::dcp_probe_end(last);
    g_dcp_step = 0;
    g_ring_skip = 2;
    const DcpCheck r = dcp_check_result(g_dcp_ref_dm_mv, dm);
    g_dcp_proof = r == DcpCheck::Shorted;
    K2LOG("[FC] dcp check dm_ref=%d dm_pulled=%d dp_pulled=%d result=%d (0 invalid, 1 open, 2 short)",
          static_cast<int>(g_dcp_ref_dm_mv), static_cast<int>(dm), static_cast<int>(dp), static_cast<int>(r));
}

// First step: when every gate holds, apply the Clear step and let the next polls run the rest.
void dcp_begin(const Ring& ring, int vbus_mv, const link::Probe& probe, bool cc_source_no_pd) {
    DcpCheckGate g;
    g.vbus_mv = vbus_mv;
    g.not_inline = link::inline_now() == link::Reason::None;
    g.pd_seen = probe.pd_active;
    g.pd_likely = link::pd_likely(probe);
    g.cc_source_no_pd = cc_source_no_pd;
    g.engine_idle = engine_idle();
    g.lines_free = dpdm::mismatch(dpdm::drive()) == 0;
    g.done_this_attach = g_dcp_done;
    if (!dcp_check_allowed(g, ring)) return;
    if (dpdm::dcp_probe_step(0)) g_dcp_step = 1;
}

// Inline VOOC monitor (dpsniff): the lines float while a far sink draws through the K2 (with or without a PD contract)
// and no engine, scan, probe, DCP check or reset hold owns them. A Black Box arm is passive too and does not stop it.
void monitor() {
    const bool lines_free = fctrig::status().state == fctrig::State::Idle && !fctrig::scan_running() &&
                            !fctrig::ufcs_probe_running() && !dpdm::reset_busy() && g_dcp_step == 0 &&
                            !fctrig::svooc_running();
    dpsniff::monitor_poll(vooc::monitor_wanted(dpsniff::monitor_enabled(), link::far_sink_now(), lines_free), lines_free);
}
}  // namespace

unsigned log_idle() {
    const dpdm::Pair d = dpdm::drive();
    const unsigned bad = dpdm::mismatch(d);
    K2LOG("[FC] idle dp=%s dm=%s mismatch=0x%X", dpdm::name(d.dp), dpdm::name(d.dm), bad);
    return bad;
}

unsigned init() {
    const unsigned bad = log_idle();
    g_ring.clear();
    g_verdict = Protocol::Pending;
    g_handshake_seen = false;
    g_sag.reset();
    dcp_reset();
    return bad;
}

void poll() {
    const auto line = adc::read();
    const auto pwr = power::read();
    const int vbus = pwr ? pwr->vbus_mv : 0;
    if (!line) {
        dcp_reset();   // the ADC lost the lines (another driver owns them): never leave a probe level behind
        g_sag.reset();
        g_verdict = vbus < 4000 ? Protocol::NoInput : Protocol::Pending;
        monitor();
        return;
    }
    const link::Probe probe = link::probe_now();
    // CC attached with no decoded Source_Capabilities is a default-USB data host (Rp on CC):
    // its D+/D- pair carries data, so the classifier must not read a charger handshake into it.
    const bool cc_source_no_pd = probe.cc_active && !probe.pd_active;
    // The inline VOOC monitor owns the lines (Sniff inputs, a data device inline): their levels are no handshake, so they neither fill the
    // window nor set the latch, and the window starts fresh when the monitor lets go.
    const bool floating = dpsniff::monitoring();
    if (vbus < 4000) {   // a new source starts a fresh window
        g_ring.clear();
        g_handshake_seen = false;
        dcp_reset();
    } else if (floating) {
        g_ring.clear();
    } else {
        bool skip = false;                    // the probe's own samples are not the source's lines
        if (g_dcp_step) { dcp_advance(); skip = true; }
        else if (g_ring_skip > 0) { g_ring_skip--; skip = true; }
        if (!skip) {
            g_ring.push(line->dp_mv, line->dm_mv);
            g_handshake_seen = update_handshake_latch(g_handshake_seen, g_ring, line->dp_mv, line->dm_mv);
            dcp_begin(g_ring, vbus, probe, cc_source_no_pd);
        }
    }
    // A sagging or cut output drags the lines with it: the guard holds the last verdict until the supply and the window
    // are good again (hardware 2026-11-11: APPLE 2.4A -> USB DATA -> NO INPUT on a port that cuts VBUS every ~6.5 s).
    g_verdict = g_sag.apply(classify(g_ring, line->dp_mv, line->dm_mv, vbus, probe.pd_active, link::pd_likely(probe),
                                     cc_source_no_pd, !g_handshake_seen, g_dcp_proof, floating),
                            vbus, equal_pair_moving(g_ring));
    monitor();
}

Protocol read() { return g_verdict; }

Protocol label() { return report_label(g_verdict, dpsniff::monitor_live().seen); }

vooc::Live vooc_live() { return dpsniff::monitor_live(); }

}  // namespace k2::fastcharge
