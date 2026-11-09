// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// What the Connection page's Refresh does, as pure rules (host-tested in tests/test_rescan_bridge.cpp); core/rescan.cpp binds them to
// the sink, the FUSB302 and the marker.
//
// The problem. A rescan used to be one thing: open both CC lines for ~150 ms so the source re-announces. A source reads that as a
// detach and cuts VBUS, which a K2 living on that charger's VBUS (a pinned Rd, no USB host) does not outlive: it came back cold,
// without Rd, and the user had to hold PLUS again. Two layers now:
//   1. A live explicit contract is refreshed without any CC change: the sink asks the source for its capabilities (Get_Source_Cap,
//      through the sink's one message gate, so no second message goes out while a Request is outstanding). The source answers
//      with Source_Capabilities, which (solicited or not) starts the source's sender-response timer: the sink answers it with a
//      Request like any announce, the same RDO when the list is unchanged (Accept + PS_RDY, no voltage change).
//   2. No contract, EPR mode, or a source that does not answer: the CC cycle, bridged by an Rd marker (IntentKind::Rescan) written
//      BEFORE the lines open when the K2 lives on the charger, so the comeback boot keeps its Rd and lands on the page.
// Inline (a real sink attached) stays passive: no transmit, no CC change.
#pragma once
#include <cstdint>

#include "k2/core/rd_intent.hpp"

namespace k2::rescanbridge {

// A source must answer a Get_Source_Cap within tSenderResponse (30 ms at most), after our message has been acknowledged (GoodCRC; the
// PHY retries its own transmission up to twice). 150 ms = five times the limit: room for the retries and a busy main loop, short
// enough that a silent source costs the user a fraction of a second before the fallback cycle starts.
inline constexpr std::uint32_t kAnswerMs = 150;
// How long the sink's message gate may stay busy (a Request or another message outstanding) before the rescan gives up and says
// "Busy". Nothing was sent by then and nothing is cycled: a CC cycle on top of a negotiation is the dark this flow exists to avoid.
inline constexpr std::uint32_t kQueueMs = 250;

struct Facts {
    bool legacy_only = false;    // PD-silent session (a legacy charger unit): the rescan changes nothing
    bool inline_mode = false;    // a real sink is attached: passive
    bool contract_live = false;  // the sink holds source capabilities and an explicit contract (Ready)
    bool negotiating = false;    // the sink is between caps and contract (neither Idle, Ready nor Failed)
    bool epr_mode = false;       // EPR mode is on: an unsolicited Source_Capabilities there is a hard reset
    std::uint8_t pin = 0;        // fusb302::pin_rd(): 1 / 2 = Rd pinned on that line, 0 = none
    bool host = false;           // a USB host is configured (it feeds this K2)
};

enum class Way : std::uint8_t {
    Passive,       // fusb302::rescan() as before: inline / legacy, nothing on the wire
    GetSrcCap,     // layer 1
    Cycle,         // layer 2, the K2 outlives the dark (a USB host or a bench supply feeds it)
    CycleBridged,  // layer 2 with the marker: the K2 lives on the charger's VBUS and comes back cold
    Busy,          // a negotiation is in flight: ask again
};

// The K2 lives on the charger's VBUS: a pinned Rd and no USB host (the Rd follower's own definition, core/rd_intent.hpp Follower).
constexpr bool self_powered(const Facts& f) { return (f.pin == 1 || f.pin == 2) && !f.host; }

// The cycle, decided on the facts of the moment it starts (a Get_Source_Cap that went unanswered takes fresh facts).
constexpr Way cycle_way(const Facts& f) { return self_powered(f) ? Way::CycleBridged : Way::Cycle; }

constexpr Way decide(const Facts& f) {
    if (f.legacy_only || f.inline_mode) return Way::Passive;
    if (f.negotiating) return Way::Busy;
    if (f.contract_live && !f.epr_mode) return Way::GetSrcCap;
    return cycle_way(f);
}

// The Get_Source_Cap flow: start() decides; for GetSrcCap the owner calls tick() every pass. Send = try the sink's gate now
// (sent() when it took the message); Refreshed = a Source_Capabilities arrived (`caps_rx` moved); Fallback = no answer within
// kAnswerMs (the owner cycles, cycle_way() on fresh facts); GiveUp = the gate stayed busy for kQueueMs (nothing was sent).
class Flow {
public:
    enum class Act : std::uint8_t { Wait, Send, Refreshed, Fallback, GiveUp };

    // One flow at a time: a start while one runs answers Busy and leaves it alone.
    Way start(const Facts& f, std::uint32_t now, std::uint32_t caps_rx) {
        if (active_) return Way::Busy;
        const Way w = decide(f);
        if (w != Way::GetSrcCap) return w;
        active_ = true;
        sent_ = false;
        t0_ = now;
        base_ = caps_rx;
        return w;
    }
    bool active() const { return active_; }
    void sent(std::uint32_t now) { sent_ = true; t0_ = now; }

    Act tick(std::uint32_t now, std::uint32_t caps_rx) {
        if (!active_) return Act::Wait;
        if (caps_rx != base_) { active_ = false; return Act::Refreshed; }   // before the deadline: a last-tick answer counts
        if (!sent_) {
            if (now - t0_ >= kQueueMs) { active_ = false; return Act::GiveUp; }
            return Act::Send;
        }
        if (now - t0_ >= kAnswerMs) { active_ = false; return Act::Fallback; }
        return Act::Wait;
    }

private:
    bool active_ = false, sent_ = false;
    std::uint32_t t0_ = 0, base_ = 0;
};

// The comeback boot: a vouched Rescan lands on Connection -> Source with "Rescanned (rebooted)"; a released one is an ordinary
// plain boot (nothing served it), and the other kinds never land there.
constexpr bool lands_on_page(rdintent::IntentKind kind, rdintent::Verdict verdict) {
    return kind == rdintent::IntentKind::Rescan && verdict == rdintent::Verdict::Vouched;
}

// Which marker kinds say "a charger-test RUN was bridged": the only ones that may make the boot treat its low CC1 (its own Ra) as
// expected during a journaled run. A Rescan never was a run (the run resumes from its journal alone), so it never does.
constexpr bool supports_run_resume(rdintent::IntentKind kind) {
    return kind == rdintent::IntentKind::Session || kind == rdintent::IntentKind::Reattach;
}

}  // namespace k2::rescanbridge
