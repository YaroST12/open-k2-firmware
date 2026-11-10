// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/log.hpp"
#include "k2/fastcharge/fastcharge.hpp"

#include "k2/drivers/adc.hpp"
#include "k2/drivers/dpdm.hpp"
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
    return bad;
}

void poll() {
    const auto line = adc::read();
    const auto pwr = power::read();
    const int vbus = pwr ? pwr->vbus_mv : 0;
    if (!line) {
        g_verdict = vbus < 4000 ? Protocol::NoInput : Protocol::Pending;
        return;
    }
    if (vbus < 4000) { g_ring.clear(); g_handshake_seen = false; }   // a new source starts a fresh window
    else {
        g_ring.push(line->dp_mv, line->dm_mv);
        if (handshake_pattern(g_ring, line->dp_mv, line->dm_mv)) g_handshake_seen = true;
    }
    const link::Probe probe = link::probe_now();
    // CC attached with no decoded Source_Capabilities is a default-USB data host (Rp on CC):
    // its D+/D- pair carries data, so the classifier must not read a charger handshake into it.
    const bool cc_source_no_pd = probe.cc_active && !probe.pd_active;
    g_verdict = classify(g_ring, line->dp_mv, line->dm_mv, vbus, probe.pd_active, link::pd_likely(probe),
                         cc_source_no_pd, !g_handshake_seen);
}

Protocol read() { return g_verdict; }

}  // namespace k2::fastcharge
