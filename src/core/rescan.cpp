// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Connection page's Refresh on the real hardware, see k2/core/rescan.hpp. The decisions are pure (k2/core/rescan_bridge.hpp).
#include "k2/core/rescan.hpp"

#include "k2/core/log.hpp"
#include "k2/core/rd_comeback.hpp"
#include "k2/core/rescan_bridge.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::rescan {
namespace {

rescanbridge::Flow g_flow;

rescanbridge::Facts facts() {
    const pd::SinkStatus& s = fusb302::sink_status();
    rescanbridge::Facts f;
    f.legacy_only = fusb302::legacy_only_active();
    f.inline_mode = fusb302::inline_mode() || fusb302::sink_downstream() || fusb302::sink_cc_far();   // a sink behind the K2: never open the CC lines
    f.contract_live = s.state == pd::SinkState::Ready && fusb302::caps().valid;
    f.negotiating = s.state != pd::SinkState::Idle && s.state != pd::SinkState::Ready && s.state != pd::SinkState::Failed;
    f.epr_mode = s.epr_mode;
    f.pin = fusb302::pin_rd();
    f.host = usb::configured();
    return f;
}

// The CC cycle. Bridged = the K2 lives on the charger's VBUS: the source drops it when the lines open, the K2 dies and comes back
// cold, so the Rescan marker is on the volume BEFORE the lines open and the comeback boot keeps its Rd. A marker that cannot be
// written (or read back) refuses the cycle: the old behaviour, a plain boot without Rd, is not repeated knowingly.
bool cycle(bool bridged) {
    if (bridged) {
        if (!rdcomeback::begin_rescan()) {
            K2LOG("[RESCAN] marker not written: no CC cycle\n");
            return false;
        }
        K2LOG("[RESCAN] bridged cycle: Rescan marker written, opening CC\n");
    }
    fusb302::rescan();
    if (bridged) {
        // Still here: a USB host or a bench supply fed the K2 through the gap. The marker is the follower's again.
        rdcomeback::end_reattach();
        K2LOG("[RESCAN] K2 survived the gap: marker handed back\n");
    }
    return true;
}

}  // namespace

Result request() {
    using rescanbridge::Way;
    const std::uint32_t now = systick::millis();
    switch (g_flow.start(facts(), now, fusb302::caps_count())) {
    case Way::Passive:
        fusb302::rescan();   // inline / PD-silent: the driver does nothing on the wire
        return Result::Passive;
    case Way::Busy: K2LOG("[RESCAN] busy: a negotiation or a rescan is in flight\n"); return Result::Busy;
    case Way::Cycle: return cycle(false) ? Result::Cycled : Result::Busy;
    case Way::CycleBridged: return cycle(true) ? Result::Cycled : Result::Busy;
    case Way::GetSrcCap:
        K2LOG("[RESCAN] live contract: Get_Source_Cap instead of a CC cycle\n");
        poll();   // try the sink's gate now; the main loop keeps trying until it takes the message or the queue window ends
        return Result::Asked;
    }
    return Result::Busy;
}

bool active() { return g_flow.active(); }

void poll() {
    if (!g_flow.active()) return;
    const std::uint32_t now = systick::millis();
    switch (g_flow.tick(now, fusb302::caps_count())) {
    case rescanbridge::Flow::Act::Wait: break;
    case rescanbridge::Flow::Act::Send:
        if (fusb302::refresh_source_caps() == pd::Err::Ok) g_flow.sent(now);
        break;
    case rescanbridge::Flow::Act::Refreshed: K2LOG("[RESCAN] source capabilities refreshed, no CC change\n"); break;
    case rescanbridge::Flow::Act::GiveUp: K2LOG("[RESCAN] sink gate busy: nothing sent, ask again\n"); break;
    case rescanbridge::Flow::Act::Fallback: {
        // The source never answered: the old rescan, on the facts of now.
        const rescanbridge::Facts f = facts();
        K2LOG("[RESCAN] no answer in %u ms: CC cycle\n", static_cast<unsigned>(rescanbridge::kAnswerMs));
        if (f.inline_mode || f.legacy_only) fusb302::rescan();
        else cycle(rescanbridge::cycle_way(f) == rescanbridge::Way::CycleBridged);
        break;
    }
    }
}

}  // namespace k2::rescan
