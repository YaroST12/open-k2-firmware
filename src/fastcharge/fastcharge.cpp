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

// Active DCP check (see dcp_check_result): once per attach, only for the bias pair. The check spans two
// polls (about 20 ms): poll N drives D+ to 0 V, poll N+1 reads D- from the newest ADC sweeps and restores
// the passive idle; nothing waits in a loop.
bool g_dcp_probing = false;
bool g_dcp_done = false;        // the check ran for this attach
bool g_dcp_proof = false;       // it found D- following D+: a real short
int g_dcp_idle_dm_mv = 0;       // D- before the drive
int g_ring_skip = 0;            // polls to keep out of the ring after the probe (the averaged ADC reading lags)

void dcp_reset() {
    if (g_dcp_probing) dpdm::dcp_probe_end();
    g_dcp_probing = g_dcp_done = g_dcp_proof = false;
    g_ring_skip = 0;
}

bool engine_idle() {
    return fctrig::status().state == fctrig::State::Idle && !fctrig::scan_running() && !fctrig::ufcs_probe_running() &&
           !dpdm::reset_busy() && !dpsniff::armed();
}

// Second step: read D- from the newest sweeps, restore the passive idle, decide. The probe tick's sample
// stays out of the ring, and so do the next two (the averaged ADC reading still holds the driven level).
void dcp_finish() {
    g_dcp_probing = false;
    g_dcp_done = true;
    g_ring_skip = 2;
    std::int32_t dp = 0, dm = 0;
    const bool have = adc::latest_dpdm(dp, dm);
    const bool idle = engine_idle();
    const bool ours = dpdm::dcp_probe_end();     // false when another owner rewrote the pins meanwhile
    if (!have || !idle || !ours) {
        K2LOG("[FC] dcp check aborted have=%d idle=%d ours=%d", have, idle, ours);
        return;
    }
    const DcpCheck r = dcp_check_result(g_dcp_idle_dm_mv, dm);
    g_dcp_proof = r == DcpCheck::Shorted;
    K2LOG("[FC] dcp check dm_idle=%d dm_pulled=%d dp_pulled=%d result=%d (0 invalid, 1 open, 2 short)",
          static_cast<int>(g_dcp_idle_dm_mv), static_cast<int>(dm), static_cast<int>(dp), static_cast<int>(r));
}

// First step: when every gate holds, drive D+ to 0 V and let the next poll read D-.
void dcp_begin(const Ring& ring, int vbus_mv, int dm_idle_mv, const link::Probe& probe, bool cc_source_no_pd) {
    DcpCheckGate g;
    g.vbus_mv = vbus_mv;
    g.not_inline = link::inline_now() == link::Reason::None;
    g.pd_seen = probe.pd_active;
    g.pd_likely = link::pd_likely(probe);
    g.cc_source_no_pd = cc_source_no_pd;
    g.engine_idle = engine_idle();
    g.lines_free = dpdm::mismatch(dpdm::read()) == 0;
    g.done_this_attach = g_dcp_done;
    if (!dcp_check_allowed(g, ring)) return;
    g_dcp_idle_dm_mv = dm_idle_mv;
    dpdm::dcp_probe_begin();
    g_dcp_probing = true;
}

// Inline VOOC monitor (dpsniff): the lines float while a data device charges through the K2 and no engine, scan,
// probe, DCP check or reset hold owns them. A Black Box arm is passive too and does not stop it.
void monitor(bool pd_active) {
    const bool lines_free = fctrig::status().state == fctrig::State::Idle && !fctrig::scan_running() &&
                            !fctrig::ufcs_probe_running() && !dpdm::reset_busy() && !g_dcp_probing;
    const bool inline_data = link::inline_now() == link::Reason::DataDevice;
    dpsniff::monitor_poll(vooc::monitor_wanted(dpsniff::monitor_enabled(), inline_data, pd_active, lines_free), lines_free);
}
}  // namespace

unsigned log_idle() {
    const auto sw = dpdm::read();
    const bool pa7 = sw.pa7, pb0 = sw.pb0, pb1 = sw.sw1, pb2 = sw.sw2;
    const unsigned bad = idle_mismatch(pa7, pb0, pb1, pb2);
    K2LOG("[FC] idle PA7=%d PB0=%d PB1=%d PB2=%d mismatch=0x%X", pa7, pb0, pb1, pb2, bad);
    return bad;
}

unsigned init() {
    const unsigned bad = log_idle();
    g_ring.clear();
    g_verdict = Protocol::Pending;
    g_handshake_seen = false;
    dcp_reset();
    return bad;
}

void poll() {
    const auto line = adc::read();
    const auto pwr = power::read();
    const int vbus = pwr ? pwr->vbus_mv : 0;
    if (!line) {
        dcp_reset();   // the ADC lost the lines (another driver owns them): never leave a probe level behind
        g_verdict = vbus < 4000 ? Protocol::NoInput : Protocol::Pending;
        monitor(link::probe_now().pd_active);
        return;
    }
    const link::Probe probe = link::probe_now();
    // CC attached with no decoded Source_Capabilities is a default-USB data host (Rp on CC):
    // its D+/D- pair carries data, so the classifier must not read a charger handshake into it.
    const bool cc_source_no_pd = probe.cc_active && !probe.pd_active;
    // The inline VOOC monitor floats the lines (no 0.6 V bias): their levels are no handshake, so they neither fill the
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
        if (g_dcp_probing) { dcp_finish(); skip = true; }
        else if (g_ring_skip > 0) { g_ring_skip--; skip = true; }
        if (!skip) {
            g_ring.push(line->dp_mv, line->dm_mv);
            g_handshake_seen = update_handshake_latch(g_handshake_seen, g_ring, line->dp_mv, line->dm_mv);
            dcp_begin(g_ring, vbus, line->dm_mv, probe, cc_source_no_pd);
        }
    }
    g_verdict = classify(g_ring, line->dp_mv, line->dm_mv, vbus, probe.pd_active, link::pd_likely(probe),
                         cc_source_no_pd, !g_handshake_seen, g_dcp_proof, floating);
    monitor(probe.pd_active);
}

Protocol read() { return g_verdict; }

vooc::Live vooc_live() { return dpsniff::monitor_live(); }

}  // namespace k2::fastcharge
