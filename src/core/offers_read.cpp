// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Charger tab's automatic offers read, see k2/core/offers_read.hpp. The plan and its gating are pure (k2/pd/pd_offers.hpp); this
// file maps each action to its sender and reads the answer back. Never a request above 5 V, never a VDM, never D+/D-.
#include "k2/core/offers_read.hpp"

#include "k2/core/log.hpp"
#include "k2/core/survey.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/power_source.hpp"

namespace k2::offers {
namespace {

using pd::OrAct;
using pd::OrStep;
using pd::SvOutcome;

constexpr std::uint32_t kWantMs = 600;       // the page refreshes the want faster than this while the tab shows
constexpr std::uint32_t kEprGateMs = 500;    // how long the sink's gate may refuse EPR_Get_Source_Cap before the list is NoAnswer
constexpr std::uint32_t kSettleMs = 5000;    // after the release the wait classifier needs a moment: still the same presence

enum class Wait : std::uint8_t { None, Attach, Epr, Release };

pd::OffersRead g_r;
pd::SourceCaps g_held;                       // the attach's Source_Capabilities, kept past the release
std::uint32_t g_epr[pd::kMaxPdos];           // the EPR list's raw PDOs, kept past the release
std::uint8_t g_epr_n = 0;
Wait g_wait = Wait::None;
std::uint32_t g_want_until = 0, g_act_at = 0, g_released_at = 0;
bool g_wanted = false, g_released = false;

bool source_epr_capable(const pd::SourceCaps& c) {
    return c.valid && c.spec_rev == 2 && c.num_pdos && c.pdos[0].type == pd::PdoType::Fixed && (c.pdos[0].raw & (1u << 23));
}

bool inline_now() { return fusb302::inline_mode() || fusb302::sink_downstream(); }

bool self_supplied() { return fusb302::self_powered() || power_source::bus_powered(); }

pd::OrCtx context(bool present, bool ports_busy) {
    const pd::SinkStatus& ss = fusb302::sink_status();
    const bool quiet = ss.state == pd::SinkState::Idle || ss.state == pd::SinkState::Ready || ss.state == pd::SinkState::Failed;
    pd::OrCtx c;
    c.present = present;
    c.inline_sink = inline_now();
    c.busy = ports_busy || fusb302::run_owner() || survey::running() || rescan::active() || fusb302::interrogate_running() ||
             fusb302::reread_status().state == pd::RereadState::Running || fusb302::xiaomi_chain_active() || !quiet;
    // A K2 that lives on this port, or is a sink there already, keeps its Rd and contract: the list is read on it, nothing is given back.
    c.contract = ss.state == pd::SinkState::Ready || self_supplied();
    c.epr_capable = source_epr_capable(fusb302::caps());
    return c;
}

void keep_epr() {
    const pd::SourceCaps& l = fusb302::epr_info_list();
    g_epr_n = 0;
    for (std::size_t i = 0; l.valid && i < l.num_pdos && i < pd::kMaxPdos; i++) g_epr[g_epr_n++] = l.pdos[i].raw;
}

// Ends the attach and gives it back. Not reread_abort(): that refuses inline and clears the forced line and Ra.
void stand_down(std::uint32_t now) {
    fusb302::reread_stand_down();
    g_wait = Wait::None;
    g_released = true;
    g_released_at = now;
}

void begin(OrAct a, std::uint32_t now) {
    g_act_at = now;
    switch (a) {
    case OrAct::Attach:
        if (fusb302::reread_start(0)) g_wait = Wait::Attach;
        else g_r.attached(false, false, false);   // refused: nothing was taken
        break;
    case OrAct::Epr: {
        g_wait = Wait::Epr;
        const pd::SinkStatus& ss = fusb302::sink_status();
        if (ss.epr_info == pd::EprInfo::None && !ss.epr_mode) (void)fusb302::epr_info_request(false);   // retried in service() while the gate is busy
        break;
    }
    case OrAct::Release: g_wait = Wait::Release; break;
    case OrAct::Abort: stand_down(now); break;
    default: break;
    }
}

void service(std::uint32_t now) {
    switch (g_wait) {
    case Wait::Attach: {
        if (fusb302::reread_status().state == pd::RereadState::Running) break;
        const pd::SourceCaps& c = fusb302::caps();
        if (c.valid) g_held = c;
        g_wait = Wait::None;
        g_r.attached(c.valid, source_epr_capable(c), fusb302::reread_attach_held());
        break;
    }
    case Wait::Epr: {
        const pd::SinkStatus& ss = fusb302::sink_status();
        SvOutcome o = SvOutcome::Silent;
        bool known = true;
        if (ss.epr_mode || ss.epr_info == pd::EprInfo::Got) o = SvOutcome::Answered;
        else if (ss.epr_info == pd::EprInfo::Refused) o = SvOutcome::Refused;
        else if (ss.epr_info == pd::EprInfo::Timeout) o = SvOutcome::Silent;
        else known = ss.epr_info == pd::EprInfo::None && fusb302::epr_info_request(false) != pd::Err::Ok &&
                     std::int32_t(now - g_act_at) >= std::int32_t(kEprGateMs);   // the gate was busy: ask again for a moment
        if (!known) break;
        if (o == SvOutcome::Answered) keep_epr();
        g_wait = Wait::None;
        g_r.epr_done(o);
        break;
    }
    case Wait::Release:
        // reread_release_now() is refused while the run's log line is pending; the attach is given back once the sink is quiet.
        if (fusb302::reread_attach_held()) { (void)fusb302::reread_release_now(); break; }
        g_wait = Wait::None;
        g_released = true;
        g_released_at = now;
        g_r.released();
        break;
    default: break;
    }
}

}  // namespace

void want(std::uint32_t now) { g_want_until = now + kWantMs; g_wanted = true; }

void on_refresh(rescan::Result r) {
    if (r == rescan::Result::Busy) return;
    g_r.mark_read(systick::millis());
}

void poll(bool ports_busy, bool present) {
    const std::uint32_t now = systick::millis();
    const bool settling = g_released && std::int32_t(now - g_released_at) < std::int32_t(kSettleMs);
    if (g_r.presence(present || g_r.running() || settling, now)) {   // the charger is gone for good: what was held is not its
        g_held.valid = false;
        g_epr_n = 0;
        g_released = false;
    }
    if (g_r.running()) {
        if (ports_busy || fusb302::run_owner()) {
            K2LOG("[OFFERS] a run took the port: read ended\n");
            g_r.cancel();
            g_wait = Wait::None;
            return;
        }
        if (inline_now()) {   // a device appeared behind the K2: stop talking and hand the port back at once
            K2LOG("[OFFERS] inline: read ended, port restored\n");
            g_r.cancel();
            stand_down(now);
            return;
        }
        const OrAct a = g_r.poll(now);
        if (a != OrAct::None) begin(a, now);
        service(now);
        if (!g_r.running()) K2LOG("[OFFERS] done: caps %u, EPR %u PDOs\n", unsigned(g_held.valid ? g_held.num_pdos : 0), unsigned(g_epr_n));
        return;
    }
    if (!g_wanted || std::int32_t(now - g_want_until) >= 0) { g_wanted = false; return; }
    if (!present || !g_r.due(now)) return;
    if (g_r.start(context(present, ports_busy), now)) {
        g_held.valid = false;
        g_epr_n = 0;
        g_released = false;
        K2LOG("[OFFERS] start\n");
    }
}

bool running() { return g_r.running(); }
bool showing_held() { return g_held.valid && !pd::is_fake() && !pd::active_caps().valid; }
pd::OrWhy why() { return g_r.step() == OrStep::Refused ? g_r.why() : pd::OrWhy::None; }

const pd::SourceCaps& shown_caps() {
    const pd::SourceCaps& live = pd::active_caps();
    return !live.valid && g_held.valid && !pd::is_fake() ? g_held : live;
}

std::span<const std::uint32_t> epr_raw() {
    static std::uint32_t live[pd::kMaxPdos];
    const pd::SourceCaps& l = fusb302::epr_info_list();
    if (l.valid && l.num_pdos > 0) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < l.num_pdos && i < pd::kMaxPdos; i++) live[n++] = l.pdos[i].raw;
        return {live, n};
    }
    return {g_epr, g_epr_n};
}

}  // namespace k2::offers
