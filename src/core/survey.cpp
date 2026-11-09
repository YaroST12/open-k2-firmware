// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// "Who's attached" on the real hardware, see k2/core/survey.hpp. The plan, the gating and the status table are pure
// (k2/pd/pd_survey.hpp); this file only maps each action to its sender and reads the answer back.
#include "k2/core/survey.hpp"

#include "k2/core/log.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::survey {
namespace {

using pd::SvAct;
using pd::SvField;
using pd::SvOutcome;

constexpr std::uint32_t kEprGateMs = 500;   // how long the sink's gate may refuse EPR_Get_Source_Cap before the field is NoAnswer

pd::Survey g_s;
std::uint32_t g_session = 0;      // the attach the view belongs to
std::uint32_t g_caps_base = 0;    // caps_count() when the Refresh asked
std::uint32_t g_act_at = 0;       // when the action in flight began
bool g_wire_started = false;
bool g_logged = true;

constexpr SvField kWireFields[] = {SvField::Identity, SvField::Svids, SvField::Modes, SvField::CapsExt, SvField::MfrInfo};

bool source_epr_capable(const pd::SourceCaps& c) {
    return c.valid && c.spec_rev == 2 && c.num_pdos && c.pdos[0].type == pd::PdoType::Fixed && (c.pdos[0].raw & (1u << 23));
}

void begin(SvAct a, std::uint32_t now) {
    g_act_at = now;
    switch (a) {
    case SvAct::Caps: break;   // the Refresh already asked; service() waits for rescan to finish
    case SvAct::Wire:
        g_wire_started = fusb302::survey_wire_start(g_s.wire_mask());
        if (!g_wire_started) {
            K2LOG("[SURVEY] wire refused\n");
            g_s.finish(now);
        }
        break;
    case SvAct::Epr: {
        const pd::SinkStatus& ss = fusb302::sink_status();
        if (ss.epr_info == pd::EprInfo::None && !ss.epr_mode) (void)fusb302::epr_info_request(false);   // retried in service() while the gate is busy
        break;
    }
    case SvAct::Cable:
        g_s.set(SvField::Cable, fusb302::emarker().valid ? SvOutcome::Answered : SvOutcome::Silent);
        g_s.finish(now);
        break;
    default: break;
    }
}

void service(std::uint32_t now) {
    switch (g_s.current()) {
    case SvAct::Caps:
        if (rescan::active()) break;
        if (fusb302::caps_count() != g_caps_base) {
            g_s.set(SvField::Caps, SvOutcome::Answered);
            g_s.finish(now);
        } else {   // no answer: rescan fell back to its CC cycle, the contract is going: the rest cannot be asked
            g_s.set(SvField::Caps, SvOutcome::Silent);
            g_s.finish(now);
            g_s.abandon();
        }
        break;
    case SvAct::Wire:
        if (!g_wire_started || fusb302::reread_status().state == pd::RereadState::Running) break;
        for (SvField f : kWireFields) g_s.set(f, pd::wire_outcome(fusb302::reread_status(), f));
        g_wire_started = false;
        g_s.finish(now);
        break;
    case SvAct::Epr: {
        const pd::SinkStatus& ss = fusb302::sink_status();
        if (ss.epr_mode || ss.epr_info == pd::EprInfo::Got) g_s.set(SvField::EprList, SvOutcome::Answered);   // EPR mode: the live caps are the list
        else if (ss.epr_info == pd::EprInfo::Refused) g_s.set(SvField::EprList, SvOutcome::Refused);
        else if (ss.epr_info == pd::EprInfo::Timeout) g_s.set(SvField::EprList, SvOutcome::Silent);
        else if (ss.epr_info == pd::EprInfo::None) {   // the gate was busy: ask again for a moment
            if (fusb302::epr_info_request(false) != pd::Err::Ok && std::int32_t(now - g_act_at) >= std::int32_t(kEprGateMs))
                g_s.set(SvField::EprList, SvOutcome::Silent);
        }
        if (g_s.entry(SvField::EprList).state != pd::SvState::Pending) g_s.finish(now);
        break;
    }
    default: break;
    }
}

}  // namespace

void on_refresh(rescan::Result r, bool charger_test) {
    if (r == rescan::Result::Busy) return;   // nothing happened: keep what is shown
    const std::uint32_t now = systick::millis();
    g_s.clear();
    g_wire_started = false;
    g_session = fusb302::session();
    g_caps_base = fusb302::caps_count();
    const pd::SinkStatus& ss = fusb302::sink_status();
    const pd::SourceCaps& caps = fusb302::caps();
    pd::SvCtx c;
    c.inline_sink = fusb302::inline_mode() || fusb302::sink_downstream() || fusb302::sink_cc_far();   // possibly inline counts
    // A sink inline blocks every field with its reason; otherwise only a live contract (the Refresh's Get_Source_Cap went into the
    // sink's gate) is asked. A CC cycle or a PD-silent session leaves every field NotAsked.
    c.contract = c.inline_sink || (r == rescan::Result::Asked && ss.state == pd::SinkState::Ready && caps.valid);
    c.charger_test = charger_test;
    c.xiaomi_chain = fusb302::xiaomi_chain_active();
    c.pd20 = caps.valid && caps.spec_rev < 2;
    c.epr_capable = source_epr_capable(caps);
    g_s.start(c, now);
    g_logged = !g_s.running() && !g_s.ran();
    K2LOG("[SURVEY] refresh r=%u contract=%d inline=%d test=%d chain=%d pd20=%d epr=%d planned=%u blocked=%u\n", unsigned(r), int(c.contract),
          int(c.inline_sink), int(c.charger_test), int(c.xiaomi_chain), int(c.pd20), int(c.epr_capable), g_s.planned(), g_s.blocked());
}

void poll(bool charger_test) {
    const std::uint32_t now = systick::millis();
    if (fusb302::session() != g_session) {   // a detach or a new source: what was read belongs to the old one
        if (g_s.planned() || g_s.blocked()) K2LOG("[SURVEY] attach changed: view cleared\n");
        g_s.clear();
        g_wire_started = false;
        g_session = fusb302::session();
        g_logged = true;
        return;
    }
    if (g_s.running()) {
        if (charger_test) {   // never over a charger test: end the run, keep what was read
            K2LOG("[SURVEY] charger test: run abandoned\n");
            g_s.abandon();
        } else {
            const pd::SinkStatus& ss = fusb302::sink_status();
            const bool quiet = ss.state == pd::SinkState::Ready && !fusb302::xiaomi_chain_active();
            const SvAct a = g_s.poll(now, quiet, true);   // the wire's own idle-line gate is the Reread's
            if (a == SvAct::Abort) g_wire_started = false;   // the bound ended the run; the Reread has its own bound
            else if (a != SvAct::None) begin(a, now);
            service(now);
        }
    }
    if (!g_logged && !g_s.running()) {
        g_logged = true;
        K2LOG("[SURVEY] done: %u got, %u not supported, %u no answer, %u blocked\n", g_s.got(), g_s.refused(), g_s.silent(), g_s.blocked());
    }
}

const pd::Survey& state() { return g_s; }
bool running() { return g_s.running(); }

}  // namespace k2::survey
