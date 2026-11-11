// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HID-cable loop guard, hardware binding: see k2/core/loop_guard.hpp for the rules and thresholds.
#include "k2/core/loop_guard.hpp"

#include "k2/core/log.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::loopguard {

namespace {
Detector g_det;
Enforcer g_enf;
bool g_pending_logged = false;

Sample sample() {
    Sample s;
    if (const auto r = adc::read()) s.aux_mv = r->aux_mv;
    if (const auto v = power::vbus_fast_mv()) s.vbus_mv = *v;
    s.host = usb::host_active();
    return s;
}

// The enforcer's step. The 5 V order itself is mostly done by the lock: the PD sink's lock edge releases a contract above 5 V
// (pd::Sink::set_hv_lock), the fast-charge engine aborts a raised session at its next poll (fc::Env::hv_locked), the charger
// test fails the row in flight (Reason::Loop), PD Stress and the twin proof are cancelled by the Ui. What is left here is a
// VBUS that is still (or again) high: ask the sink for 5 V, then the Hard Reset, then the CC detach.
void act(Action a, std::int32_t vbus_mv) {
    if (a == Action::None) return;
    K2LOG("[LOOP] %s vbus=%d\n", action_name(a), int(vbus_mv));
    switch (a) {
        case Action::Order5V:
            if (vbus_mv > kSafeMv) fusb302::sink_release();   // idempotent: a release already under way is left alone
            break;
        case Action::HardReset:
            fusb302::sink_hard_reset(false);   // logs its own refusal (no CC / inline)
            break;
        case Action::Detach:
            if (!fusb302::cc_detach()) K2LOG("[LOOP] detach refused (inline or pinned Rd): VBUS stays %d mV\n", int(vbus_mv));
            break;
        case Action::None:
            break;
    }
}
}  // namespace

void poll(std::uint32_t now_ms) {
    const Sample s = sample();
    if (g_det.update(now_ms, s)) {
        if (g_det.locked())
            K2LOG("[LOOP] lock %s aux=%d vbus=%d host=%d n=%u\n", reason_name(g_det.reason()), int(s.aux_mv), int(s.vbus_mv),
                  int(s.host), unsigned(g_det.events()));
        else
            K2LOG("[LOOP] clear aux=%d vbus=%d host=%d\n", int(s.aux_mv), int(s.vbus_mv), int(s.host));
    }
    if (g_det.pending() != g_pending_logged) {
        g_pending_logged = g_det.pending();
        if (g_pending_logged)
            K2LOG("[LOOP] suspect aux=%d vbus=%d, no USB host: lock in %u ms\n", int(s.aux_mv), int(s.vbus_mv), unsigned(kSuspectMs));
        else if (!g_det.locked())
            K2LOG("[LOOP] suspect over aux=%d vbus=%d host=%d\n", int(s.aux_mv), int(s.vbus_mv), int(s.host));
    }
    fusb302::sink_set_hv_lock(g_det.locked());   // acts on the edges only
    act(g_enf.update(now_ms, g_det.locked(), s.vbus_mv), s.vbus_mv);
}

bool locked() { return g_det.locked(); }
Reason reason() { return g_det.reason(); }
bool pending() { return g_det.pending(); }
std::uint16_t events() { return g_det.events(); }

}  // namespace k2::loopguard
