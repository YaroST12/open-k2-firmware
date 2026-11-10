// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The phase-1 lab hand-back binder: the pure sequence (include/k2/core/handback.hpp) over the volume, the FUSB302
// and the main loop. Armed only by the remote `pd handback --confirm` (Cmd::PdHandback, src/core/remote.cpp) after
// pd::handback_verdict allowed it; never from the UI, the shell or the charger test.
#include "k2/core/handback.hpp"
#include "k2/core/log.hpp"
#include "k2/core/rd_intent.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/fusb302.hpp"

namespace k2::handback {
namespace {
Seq g_seq;
std::uint32_t g_session = 0;
}  // namespace

void arm(std::uint32_t now) {
    g_session = fusb302::session();
    g_seq.arm(now, fusb302::contract().requests);
    K2LOG("[LINK] handback armed: waiting for /rd.rec to be gone (present=%d)\n",
          fs::instance().exists(rdintent::kPath) ? 1 : 0);
}

bool done() { return g_seq.done && fusb302::session() == g_session; }

void poll(std::uint32_t now) {
    if (g_seq.step == Step::Idle) return;
    if (fusb302::session() != g_session) {   // a new attach: one hand-back per attach
        g_seq.step = Step::Idle;
        g_seq.done = false;
        return;
    }
    const bool present = fs::instance().exists(rdintent::kPath);
    const pd::Contract& c = fusb302::contract();
    const Ev ev = g_seq.poll(now, present, c.requests, c.mv,
                            [] { fusb302::rd_for_run(false); },              // (2) drop the Rd (inline already holds SWITCHES0 at no-Rd)
                            [] { return fusb302::sink_hard_reset(true); }); // (3) the one Hard Reset, lab-inline driver path
    switch (ev) {
    case Ev::RecordGone:
        K2LOG("[LINK] handback: /rd.rec gone after %u ms, Rd dropped, hard reset sent\n", unsigned(now - g_seq.t0));
        break;
    case Ev::ResetRefused:
        K2LOG("[LINK] handback: the driver refused the hard reset\n");
        break;
    case Ev::RecordTimeout:
        K2LOG("[LINK] handback aborted: /rd.rec still present after %u ms, no reset sent\n", unsigned(now - g_seq.t0));
        break;
    case Ev::Result:
        if (g_seq.far_max_mv)
            K2LOG("[LINK] handback result: far sink highest Request %u mV\n", unsigned(g_seq.far_max_mv));
        else
            K2LOG("[LINK] handback result: no Request: Type-C current\n");
        break;
    case Ev::None:
        break;
    }
}

}  // namespace k2::handback
