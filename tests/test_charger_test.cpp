// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "test_util.h"
#include "k2/chargertest/charger_test.hpp"
#include "k2/chargertest/charger_journal.hpp"
#include "k2/chargertest/step_label.hpp"
#include "k2/chargertest/result_layout.hpp"
#include "k2/core/rd_intent.hpp"
#include "k2/pd/transcap_reason.hpp"
#include "ramdisk.hpp"
#include "ui_stubs.h"
#include "k2/core/remote_proto.hpp"
#include <algorithm>
#include <string>
#include <vector>

using namespace k2;
namespace ct = k2::charger_test;
namespace cj = k2::charger_journal;
using A = ct::Algorithm;
static constexpr ct::Options kPd{.selected = ct::bit(A::Pd3Fixed) | ct::bit(A::Pps)};
struct Fake final : ct::Env {
    ct::Input in;
    pd::SourceCaps caps;
    int pd_requests = 0, fc_requests = 0, retargets = 0, releases = 0, fc_releases = 0;
    std::vector<bool> retarget_log;   // the retarget flag of each request_fc call, in order
    // A bank that browns out: the n-th fc request drops VBUS to 4.45 V with a new attach session until low_until.
    int drop_at = 0, glitch_at = 0, pd_drop_at = 0, pd_drop_times = 1; uint32_t low_until = 0; bool gone = false;
    // pattern usb-a-cuts-on-low-dpdm: an A-to-C cable's Rp is fed from VBUS, so CC comes back (a new attach session) only some ms
    // AFTER the output is back at 5 V. late_session_ms = that delay after the end of the drop (0 = no second session).
    uint32_t late_session_ms = 0, late_session_at = 0;
    int low_mv_for_3v3 = 0;   // a source whose output lands here (not at the target) on a 3.3 V programmable step
    int pd_small_step_mv = -1;   // see request_pd: the output move of a +20 mV PPS request (-1 = follow the target)
    int last_pps_mv = 0, last_pd_vbus = 0;
    // walk-trend script (ROADMAP "PPS step walk judged by total + trend"): the output moves of the successive +20 mV PPS
    // requests of one walk, in mV (a repeat of the last entry past the end); empty = off (pd_small_step_mv rules).
    // walk_step_i counts the +20 mV steps since the last non-step PPS request (the base level).
    std::vector<int> walk_deltas;
    unsigned walk_step_i = 0;
    // walk-settle script (ROADMAP "the PPS step walk fails a good source..."): a PPS request at walk_settle_mv arms the
    // output at walk_settle_from_mv, settling linearly to walk_settle_to_mv over walk_settle_ms (the UGREEN shape: the
    // 13.0 V base reads 13007 mV, then settles to 12990). While settling the output follows the script, not the target.
    int walk_settle_mv = -1, walk_settle_from_mv = 0, walk_settle_to_mv = 0;
    unsigned walk_settle_ms = 0;
    uint32_t settle_t0 = 0; bool settle_on = false;
    int walk_ripple_mv = 0;      // alternating ripple added to VBUS every tick: never within the steady span
    int noise_mv = 0; bool noise_flip = false;   // a symmetric +-noise_mv swing of every sample around the level (see tick)
    // pattern latch-off-low-pps: after a request at latch_mv the source stops regulating latch_delay_ms later, drains to 0 and
    // stays dead (no caps, no session, a release changes nothing) until the test calls revive().
    int latch_mv = 0; uint32_t latch_delay_ms = 0, latch_at = 0; bool latched = false;
    void revive() { latch_mv = 0; latched = false; in.vbus_mv = 5000; caps.valid = true; ++in.session; }
    // The real C135 (2026-11-03): the cut is not a latch. recover_after_ms later the output returns by itself with a Hard Reset
    // and the source list stays invalid for caps_after_ms more (the sink must not ask for a PDO before it is announced again).
    uint32_t recover_after_ms = 0, caps_after_ms = 0, latched_at = 0, caps_back_at = 0;
    bool release_ok = true, request_ok = true;
    bool release_auto_hold = false;   // the 5 V lands as the driver's own PDO 1 answer (held + auto_hold), as after a Hard Reset's re-request
    std::vector<ct::Step> records;
    std::vector<pd::Choice> choices;
    std::vector<int> fc_mas;   // the current each fc request carried
    // The engine's fc::Caps (fctrig::caps()): a SCP / UFCS request makes the charger's capabilities readable, as its session does.
    const fc::Caps* serve_caps = nullptr;
    std::vector<fc::Proto> protocols;
    fc::Proto no_response_proto = fc::Proto::Count;   // a protocol whose engine fails with NoResponse on the first raised request
    // A QC-only bank asked for a protocol it does not speak: the engine ends every request of it as Unsupported (it never got an
    // answer, whatever VBUS did) and the abort puts the lines back to 0 V, so the output is back at 5 V.
    fc::Proto unsupported_proto = fc::Proto::Count;
    // A port that speaks no QC (JT-P18 USB-C, 2026-11-07): every request of the protocol ends in the engine's NoHandshake, VBUS stays at 5 V.
    fc::Proto no_handshake_proto = fc::Proto::Count;
    int fc_offset_mv = 0;                             // the output lands this far from the requested voltage (above 5 V)
    int fc_max_mv = 0;                                // a Class A charger: the output stops at this level however high the request (0 = no limit)
    // pattern output-capped-below-advertised (Baseus FH22, 2026-11-08): a PD source accepts every request of its list (PS_RDY, the
    // contract is live) but the converter stops at this level (0 = no cap).
    int pd_max_mv = 0;
    // The FH22 shape (2026-11-11): a Request at out_script_mv starts a scripted output (out_script: {ms since the Request, VBUS mV},
    // piecewise linear, the last point holds) and the sink's PPS keep-alive: ka_period_ms after the Request (and after every
    // PS_RDY) it re-requests the same level, so the sink is Requesting (not ready) for the next ka_windows entry (cycled) ms.
    int out_script_mv = 0; std::vector<std::pair<uint32_t, int>> out_script; uint32_t out_t0 = 0; bool out_on = false;
    uint32_t ka_period_ms = 0, ka_at = 0, ka_end = 0; std::vector<uint32_t> ka_windows; size_t ka_i = 0;
    unsigned step_ms = 100;   // the poll period of tick()
    // Item G (PS_RDY timing quirks): the Accept -> PS_RDY time the sink status stamps (ps_rdy_delay_ms, only for a request at
    // ps_rdy_delay_mv when that is set), and a PS_RDY the sink took as inferred (ps_rdy_inferred_fake).
    uint32_t ps_rdy_delay_ms = 0; int ps_rdy_delay_mv = 0; bool ps_rdy_inferred_fake = false;
    // A scripted PD output shape for the sweep tests (0 = follow the request): pd_offset_mv lands the output this far
    // from it above 5 V; at/above pd_fold_from_mv the output folds back to pd_fold_to_mv (steady - the FH22 shape);
    // pd_drop_at_mv hard-resets the source on the request at that level (a lost source mid-sweep).
    int pd_offset_mv = 0;
    unsigned pd_fold_from_mv = 0, pd_fold_to_mv = 0, pd_drop_at_mv = 0;
    // pattern pps-off-at-no-load (splitter run, 2026-11-10): the source takes a Request at a level of collapse_on's PDO type, then
    // drops its output while the port draws collapse_ma: VBUS 3.5 V, 1.8 V, then the port tears the session down (one new session,
    // VBUS stays at 1.8 V until the port restarts by itself 2.5 s after the Request). collapse_after_ready: the Request is answered with PS_RDY first (the contract is
    // live), the collapse starts 200 ms later. Unknown = no collapse.
    pd::PdoType collapse_on = pd::PdoType::Unknown; int collapse_ma = 0; bool collapse_after_ready = false; uint32_t collapse_t0 = 0;
    bool qc2_no_20v = false;                         // pattern qc3-20v-without-qc2-20v: QC2's discrete 20 V level stays at 5 V, QC3 still ramps to 20 V
    // pattern qc-redetect-after-reset (power strip USB-A, 2026-11-07): the HVDCP detection needs a quiet gap after the lines' last
    // reset. A fresh QC2 / QC3 handshake that starts less than this long after the previous session's release gets no answer; a
    // retarget in the live session never does.
    uint32_t redetect_gap_ms = 0, fc_released_at = 0; bool fc_released = false; int redetect_refused = 0;
    std::vector<int> fc_mvs;                          // the level each fc request asked for
    int fc_5v_mv = 0;                                 // a 5 V request lands here instead (a charger idling at 5.12 V); 0 = at 5 V
    int fc_low_mv = 0;                                // a request below 5 V lands here (an unloaded output cannot sink down); 0 = at the request
    int scp_adapter_mv = 0;                           // the SCP adapter's own voltage readback while its session is up (0 = not read)
    // EPR entry model: a pos 0 request enters EPR (or is refused) and swaps in the EPR source list.
    bool epr_refuse = false; std::uint8_t epr_refuse_code = 0; pd::SourceCaps epr_caps;
    // A release in EPR mode ends with EPR_Mode Exit: the source announces its SPR list again (Baseus 140W, 2026-11-04).
    pd::SourceCaps spr_caps;
    // SPR-mode EPR_Get_Source_Cap model (informational, no entry): info_mode 0 = the sink cannot ask (default, the old behaviour),
    // 1 = the source answers with info_list 60 ms after the ask, 2 = it refuses (Not_Supported), 3 = it stays silent until the
    // sink gives up (1.5 s). info_busy = the first n asks find the line occupied.
    int info_mode = 0, info_asks = 0, info_busy = 0; uint32_t info_at = 0; pd::SourceCaps info_list, info_shown;
    pd::Err epr_info_ask() override {
        ++info_asks;
        if (!info_mode) return pd::Err::NoCaps;
        if (info_busy > 0) { --info_busy; return pd::Err::Busy; }
        in.pd.epr_info = pd::EprInfo::Pending; info_at = in.now + (info_mode == 3 ? pd::kEprInfoMs : 60);
        return pd::Err::Ok;
    }
    // A charger that hard-resets in the middle of the EPR entry (Baseus 140W, 2026-11-05: Enter_Succeeded, chunk 0, our chunk-1
    // request acknowledged, then a Hard Reset ~75 ms later). The next n entries from the SPR contract die that way (entry_resets_row:
    // an EPR row asked from SPR; entry_resets_base: the baseline pos-0 entry): the list goes away for reset_dark_ms, VBUS sags to
    // 3 V, the SPR list comes back and the sink re-contracts 5 V by itself. The attach session does not change (Rd stays on).
    // Until the reset the live list is the PARTIAL EPR one (chunk 0 only: PDOs 1..6, epr + epr_partial, as the PD engine decodes it):
    // that window lasts 8 ms on a healthy source and until the Hard Reset (75 ms) on this one.
    int entry_resets_row = 0, entry_resets_base = 0; uint32_t reset_dark_ms = 1500, dip_until = 0, reset_at = 0, partial_ms = 150;   // 150: the tick is 100 ms, one poll must see the window
    std::vector<uint32_t> entry_times;   // `now` of every EPR entry attempt
    int reject_mv = 0; std::vector<uint32_t> reject_times;   // a SPR request at reject_mv is Rejected (the 6 A over-offer probe)
    // A Xiaomi-style source (SVID 0x2717 listed): the auth chain completes at auth_at (0 = never); until then the sink reports
    // xiaomi_svid without xiaomi_authed. Every hunt request notes its time and whether the chain was done when it went out.
    bool xiaomi_svid = false; uint32_t auth_at = 0;
    std::vector<uint32_t> hunt_times; std::vector<bool> hunt_authed;
    std::vector<unsigned> hunt_apdo_ma;   // the advertised limit of the slot each hunt Request referred to (the list in force at that moment)
    // CUKTECH-style unlock: the moment the chain finishes (auth_at) the source announces auth_caps (PPS APDOs at 6 A) as its new list.
    bool auth_caps_on = false; pd::SourceCaps auth_caps; uint32_t auth_caps_delay_ms = 0;   // ... that long after the chain finished
    // A source that does not offer the 6 A of a hunt row: it Rejects the request (reject_hunts) or never answers it (silent_hunts).
    bool reject_hunts = false, silent_hunts = false;
    // pattern reject-then-reduced-list: reannounce_ms after the source Rejected a hunt Request it announces reannounce_caps (the
    // same list or a shorter one). 100 = before the runner has looked at the Reject, 600 = while the hunt row is being released.
    uint32_t reannounce_ms = 0, reannounce_at = 0; pd::SourceCaps reannounce_caps;
    // A Xiaomi bank that answers the 6 A hunt Request with a Hard Reset (Xiaomi 20K, 2026-11-07): the Hard Reset comes 100 ms after
    // the Request, the list is dark for reset_dark_ms, then comes back as the fixed PDOs only (hr_short_ms long, the chain re-runs
    // meanwhile: listed, not authed) and then in full again, authed.
    // The sink forgets the SVID on a Hard Reset and learns it again with the list (svid_back_at).
    bool hunt_hr = false; uint32_t hr_short_ms = 0, hr_full_at = 0, svid_back_at = 0; pd::SourceCaps hr_full;
    // The sink's trace of the chain and of the last Request, as a scripted source and sink would leave it (in.xm points here; a test
    // sets in.xm = nullptr for a page that has no trace). A hunt Request is recorded like the real sink does at queue time.
    pd::XiaomiTrace xm;
    bool xm_reannounce = false, xm_in_chain = false, hold_hunts = false;   // caps re-announced after the auth / Request left inside the chain / the chain guard never lets it go
    uint8_t xm_last_step = 0;
    std::vector<ct::HuntEvidence> evidence_seen;   // what hunt_evidence() was called with, in call order
    void hunt_evidence(const ct::Step&, const ct::HuntEvidence& e) override { evidence_seen.push_back(e); }
    void trace_hunt_request(const pd::Choice& c) {
        if (hold_hunts) { xm.req_held = true; return; }
        const pd::Pdo& d = caps.pdos[c.pos - 1];
        ++xm.req_n; xm.req_held = false; xm.req_pos = c.pos; xm.req_pdo = d.raw; xm.req_type = d.type; xm.req_mv = c.mv; xm.req_ma = c.ma;
        xm.req_caps = caps.num_pdos; xm.req_at = in.now; xm.req_in_chain = xm_in_chain;
        xm.answer = reject_hunts ? pd::ReqAnswer::Reject : pd::ReqAnswer::None; xm.answer_at = reject_hunts ? in.now + 30 : 0;
    }
    void begin_cut_entry() {
        spr_caps = caps; caps = epr_caps; caps.epr_partial = true; caps.num_pdos = 6;
        for (unsigned n = 6; n < 11; ++n) caps.pdos[n] = {};
        in.pd.epr_mode = true; in.pd.held = true; in.pd.state = pd::SinkState::EprWaitCaps; in.pd.result = pd::SinkResult::None;
        reset_at = in.now + partial_ms;
    }
    void source_hard_reset() {
        ++in.pd.source_hard_resets; if (spr_caps.valid) caps = spr_caps;   // the source sent it: the sink counts it apart from its own (hard_resets)
        caps.valid = false; caps_back_at = in.now + reset_dark_ms; dip_until = in.now + reset_dark_ms; in.vbus_mv = 3000;
        if (hunt_hr && hr_short_ms) {   // the list comes back short and grows to the full one hr_short_ms later
            hr_full = caps; hr_full.valid = true; hr_full_at = caps_back_at + hr_short_ms;
            caps.num_pdos = 5; caps.pdos[5] = caps.pdos[6] = {};
            if (xiaomi_svid) auth_at = hr_full_at;   // the chain re-runs: listed, authed only with the full list
        }
        if (hunt_hr) { svid_back_at = caps_back_at; in.pd.xiaomi_svid = in.pd.xiaomi_authed = false; }   // the sink clears the chain with the reset
        in.pd.state = pd::SinkState::Idle; in.pd.result = pd::SinkResult::HardReset; in.pd.held = false; in.pd.epr_mode = false; in.pd.mv = 5000;
    }
    // Power-loss injection: every call the runner makes into its environment is an event. Event number cut_event never happens
    // (nor does anything after it): the K2 lost power just before it. The journal and the charger keep what the earlier
    // events did. k2_rebooted() then brings the same charger back in front of a fresh K2.
    size_t events = 0, cut_event = SIZE_MAX; bool dead = false;
    bool alive() { if (dead) return false; if (events++ == cut_event) { dead = true; return false; } return true; }
    void k2_rebooted() {
        dead = false; cut_event = SIZE_MAX;
        // The charger kept its power: it is back at its 5 V default with its list; the K2 has no contract and a new attach.
        if (in.pd.epr_mode && spr_caps.valid) caps = spr_caps;
        if (!caps.valid && saved.valid) caps = saved;
        caps.valid = true; caps.active_cc = 1; rd_on = true; back_done = true; handed = false; low_until = 0; dip_until = 0; reset_at = 0; latched = false;
        in.vbus_mv = 5000; in.pd.held = in.pd.epr_mode = false; in.pd.mv = 5000; in.pd.pos = 1; in.pd.state = pd::SinkState::Ready;
        in.pd.result = pd::SinkResult::Ok; in.fc_state = fc::State::Idle; in.pd_quiet_ms = 0; ++in.session;
    }
    std::vector<uint32_t> req_times;   // `now` of every request_pd call, in order (the settle-window tests read it)
    pd::Err request_pd(pd::Choice c) override {
        if (!alive()) return pd::Err::Busy;
        ++pd_requests; choices.push_back(c); req_times.push_back(in.now);
        out_on = false; ka_at = ka_end = 0;   // a new Request ends the scripted output and the keep-alive of the old one
        if (c.hunt) { hunt_times.push_back(in.now); hunt_authed.push_back(in.pd.xiaomi_authed);
                      hunt_apdo_ma.push_back(c.pos >= 1 && c.pos <= caps.num_pdos ? caps.pdos[c.pos - 1].max_ma : 0u); if (c.pos >= 1 && c.pos <= caps.num_pdos) trace_hunt_request(c); }
        if (c.pos == 0 || (c.pos >= 8 && !in.pd.epr_mode && epr_caps.valid)) {
            entry_times.push_back(in.now);
            int& resets = c.pos == 0 ? entry_resets_base : entry_resets_row;
            if (resets > 0) { --resets; begin_cut_entry(); return pd::Err::Ok; }
        }
        if (c.pos == 0) {
            if (epr_refuse) {
                in.pd.state = pd::SinkState::Failed; in.pd.result = pd::SinkResult::EprFailed; in.pd.epr_fail = epr_refuse_code;
                return pd::Err::Ok;
            }
            spr_caps = caps; caps = epr_caps; in.pd.epr_mode = true; in.pd.held = false;
            in.pd.state = pd::SinkState::Ready; in.pd.result = pd::SinkResult::Ok; in.pd.mv = 5000;
            return pd::Err::Ok;
        }
        if (!request_ok) return pd::Err::Busy;
        if (!caps.valid) return pd::Err::NoCaps;
        if (hunt_hr && c.hunt) {   // the Request is taken, then the source resets
            reset_at = in.now + 100; in.pd.state = pd::SinkState::Requesting; in.pd.result = pd::SinkResult::None; in.pd.held = true;
            return pd::Err::Ok;
        }
        if (silent_hunts && c.hunt) { in.pd.state = pd::SinkState::Requesting; in.pd.result = pd::SinkResult::None; in.pd.held = true; return pd::Err::Ok; }
        if (reject_hunts && c.hunt) {
            reject_times.push_back(in.now); in.pd.state = pd::SinkState::Ready; in.pd.result = pd::SinkResult::Rejected;
            if (reannounce_ms) reannounce_at = in.now + reannounce_ms;
            return pd::Err::Ok;
        }
        if (reject_mv && int(c.mv) == reject_mv && c.pos < 8) {   // the source Rejects the request and keeps the old contract
            reject_times.push_back(in.now); in.pd.state = pd::SinkState::Ready; in.pd.result = pd::SinkResult::Rejected;
            return pd::Err::Ok;
        }
        if (pd_drop_at && pd_requests >= pd_drop_at && pd_requests < pd_drop_at + pd_drop_times) {
            // The source hard-resets and re-attaches: a new session, VBUS gone for 2.5 s, the contract is lost.
            ++in.session; low_until = in.now + 2500; in.vbus_mv = 0;
            in.pd.state = pd::SinkState::Idle; in.pd.held = false; in.pd.mv = 5000;
            return pd::Err::Ok;
        }
        if (pd_drop_at_mv && unsigned(c.mv) == pd_drop_at_mv) {
            ++in.session; low_until = in.now + 2500; in.vbus_mv = 0;
            in.pd.state = pd::SinkState::Idle; in.pd.held = false; in.pd.mv = 5000;
            return pd::Err::Ok;
        }
        if (collapse_on != pd::PdoType::Unknown && c.pos <= caps.num_pdos && caps.pdos[c.pos - 1].type == collapse_on) {
            if (c.mv > 5000) collapse_t0 = in.now + (collapse_after_ready ? 200 : 0);
            if (c.mv > 5000 && !collapse_after_ready) {   // Accept, no PS_RDY: the sink waits in Transition
                in.pd.state = pd::SinkState::Transition; in.pd.result = pd::SinkResult::None; in.pd.held = true;
                in.pd.pos = c.pos; in.pd.mv = c.mv;
                return pd::Err::Ok;
            }
        }
        in.pd.state = pd::SinkState::Ready; in.pd.result = pd::SinkResult::Ok;
        in.pd.accept_at = in.now;
        in.pd.ps_rdy_at = in.now + (!ps_rdy_delay_mv || int(c.mv) == ps_rdy_delay_mv ? ps_rdy_delay_ms : 0u);
        in.pd.ps_rdy_inferred = ps_rdy_inferred_fake;
        if (c.pos >= 8 && !in.pd.epr_mode && epr_caps.valid) { spr_caps = caps; caps = epr_caps; }   // the sink enters EPR first
        in.pd.held = true; in.pd.epr_mode = in.pd.epr_mode || c.pos >= 8;
        if (latch_mv && int(c.mv) == latch_mv) latch_at = in.now + latch_delay_ms + 1;
        in.pd.pos = c.pos; in.pd.mv = c.mv;
        {   // the output level of a PD request; pd_small_step_mv scripts a PPS request exactly +20 mV above the
            // previous one (the step-walk row's steps): the output moves by that many mV instead of 20 (-1 = follow)
            const bool pps_req = c.pos >= 1 && c.pos <= caps.num_pdos && caps.pdos[c.pos - 1].type == pd::PdoType::Pps;
            if (pps_req && walk_settle_mv >= 0 && int(c.mv) == walk_settle_mv) { settle_on = true; settle_t0 = in.now; }
            if (settle_on) {
                const uint32_t t = in.now - settle_t0;
                if (t >= walk_settle_ms) { settle_on = false; in.vbus_mv = walk_settle_to_mv; }
                else in.vbus_mv = walk_settle_from_mv + (walk_settle_to_mv - walk_settle_from_mv) * int(t) / int(walk_settle_ms);
            } else if (pps_req && last_pps_mv && int(c.mv) - last_pps_mv == 20 && !walk_deltas.empty()) {
                in.vbus_mv = last_pd_vbus + walk_deltas[std::min<size_t>(walk_step_i, walk_deltas.size() - 1)];
                ++walk_step_i;
            } else if (pps_req && last_pps_mv && int(c.mv) - last_pps_mv == 20 && pd_small_step_mv >= 0)
                in.vbus_mv = last_pd_vbus + pd_small_step_mv;
            else
                in.vbus_mv = low_mv_for_3v3 && c.mv == 3300 ? low_mv_for_3v3 : int(c.mv);
            if (pps_req && !(last_pps_mv && int(c.mv) - last_pps_mv == 20)) walk_step_i = 0;   // a base level: the walk restarts
            if (pps_req) { last_pps_mv = int(c.mv); last_pd_vbus = in.vbus_mv; }
        }
        if (c.mv >= 5000) {   // the scripted output shape of the sweep tests
            if (pd_fold_from_mv && unsigned(c.mv) >= pd_fold_from_mv) in.vbus_mv = int(pd_fold_to_mv);
            else if (pd_offset_mv) in.vbus_mv = int(c.mv) + pd_offset_mv;
        }
        if (pd_max_mv && int(c.mv) > pd_max_mv) in.vbus_mv = pd_max_mv;
        if (out_script_mv && int(c.mv) == out_script_mv && !out_script.empty()) {
            out_on = true; out_t0 = in.now; in.vbus_mv = out_script.front().second;
            if (ka_period_ms) ka_at = in.now + ka_period_ms;
        }
        return pd::Err::Ok;
    }
    fc::Fail request_fc(fc::Proto p, int mv, bool retarget, int ma) override {
        if (!alive()) return fc::Fail::NoHandshake;
        ++fc_requests; retargets += retarget; retarget_log.push_back(retarget); protocols.push_back(p); fc_mas.push_back(ma); fc_mvs.push_back(mv);
        if (ra_dead_legacy && ra_on) { ++ra_dead_fc; return fc::Fail::NoHandshake; }
        if (serve_caps && (p == fc::Proto::Scp || p == fc::Proto::Ufcs)) in.fc_caps = serve_caps;
        if (glitch_at == fc_requests) ++in.session;   // CC blip: new session, VBUS unchanged
        if (no_response_proto == p && mv > 5000) {    // the engine gave up: the output never left 5 V
            in.fc_state = fc::State::Failed; in.fc_fail = fc::Fail::NoResponse; in.vbus_mv = 5000;
            return fc::Fail::None;
        }
        if (redetect_gap_ms && !retarget && (p == fc::Proto::Qc2 || p == fc::Proto::Qc3) && fc_released && in.now - fc_released_at < redetect_gap_ms) {
            ++redetect_refused; in.fc_state = fc::State::Failed; in.fc_fail = fc::Fail::NoHandshake; in.vbus_mv = 5000; return fc::Fail::None;
        }
        if (no_handshake_proto == p) { in.fc_state = fc::State::Failed; in.fc_fail = fc::Fail::NoHandshake; in.vbus_mv = 5000; return fc::Fail::None; }
        if (unsupported_proto == p) { in.fc_state = fc::State::Failed; in.fc_fail = fc::Fail::Unsupported; in.vbus_mv = 5000; return fc::Fail::None; }
        if (drop_at == fc_requests) { ++in.session; low_until = in.now + (gone ? 1000000 : 4000); in.vbus_mv = gone ? 0 : 4450; return fc::Fail::None; }
        if (!request_ok) return fc::Fail::NoHandshake;
        if (contract_blocks_legacy && !handed && !retarget) { fc_blocked++; return fc::Fail::NoHandshake; }
        if (fussy && !retarget) { if (!fresh) { ++fussy_fails; return fc::Fail::NoHandshake; } fresh = false; }
        in.fc_state = fc::State::Active; in.vbus_mv = mv > 5000 ? mv + fc_offset_mv : fc_5v_mv ? fc_5v_mv : mv;
        if (mv < 5000 && fc_low_mv) in.vbus_mv = fc_low_mv;
        in.fc_adapter_mv = p == fc::Proto::Scp ? scp_adapter_mv : 0;
        if (fc_max_mv && mv > fc_max_mv) in.vbus_mv = fc_max_mv;   // a Class A charger answers the request and stays at its top level
        if (qc2_no_20v && p == fc::Proto::Qc2 && mv == 20000) in.vbus_mv = 5100;
        return fc::Fail::None;
    }
    void release(bool pd) override {
        if (!alive()) return;
        ++releases; fc_releases += !pd;
        collapse_t0 = 0; in.ibus_ma = 0; out_on = false; ka_at = ka_end = 0;
        if (!pd) { fc_released = true; fc_released_at = in.now; }
        if (!release_ok || latched) return;
        in.vbus_mv = 5000;
        if (pd && in.pd.epr_mode && spr_caps.valid) caps = spr_caps;
        if (pd) { in.pd.held = in.pd.epr_mode = false; in.pd.state = pd::SinkState::Ready; in.pd.mv = 5000; }
        if (pd && release_auto_hold) { in.pd.held = in.pd.auto_hold = true; in.pd.pos = 1; }
        else in.fc_state = fc::State::Idle;
    }
    // Journal hooks: what the UI Env does, so a test can cut the run anywhere and reboot into the journal.
    cj::Journal* jr = nullptr;
    const ct::Runner* rn = nullptr;
    std::vector<std::pair<ct::Phase, size_t>> phases;
    std::vector<std::pair<size_t, uint8_t>> attempts;   // journaled loss counters (step index, count)
    void phase(ct::Phase p, size_t i) override { if (!alive()) return; phases.push_back({p, i}); if (jr && rn) jr->phase(p, rn->steps(), i); }
    // The UI's journal_ready()/journal_waited(): a test sets journal_blocked to model section files still to be written.
    bool journal_blocked = false;
    std::vector<uint32_t> waited;   // wait-for-replan durations, one per request that was parked and then went
    bool journal_ready() override { return !journal_blocked; }
    void journal_waited(uint32_t ms) override { waited.push_back(ms); }
    void attempt(size_t i, uint8_t n) override { attempts.push_back({i, n}); if (jr) jr->attempt(i, n); }   // restore() time: not a run event
    std::vector<size_t> closings;   // records journaled before each closing() call
    void closing(ct::Reason r) override {   // the UI closes the journal here, before the skip rows are written
        if (!alive()) return;
        closings.push_back(records.size());
        if (jr) jr->finish(uint8_t(ct::State::Stopped), uint8_t(r));
    }
    ct::Options jopts{};   // the options the journaled run started with (the UI keeps them too)
    int plan_changes = 0;
    // The UI notes the change and pumps one file per tick; the Fake writes all of it at once (the journal's own tests cover the pieces).
    void plan_changed() override {
        ++plan_changes;
        if (jr && rn) { jr->note_plan(rn->steps()); jr->settle(rn->run_id(), jopts, rn->evidence(), rn->steps()); }
    }
    // pattern budget-reannounce-mid-run: (follow, withdrawn, back) of every mid-run re-plan, in call order.
    std::vector<std::array<unsigned, 3>> replans;
    void replan(unsigned follow, unsigned withdrawn, unsigned back) override { replans.push_back({follow, withdrawn, back}); }
    std::vector<bool> record_quiet;   // Runner::quiet() at the moment of each record() (a row's verdict is decided while it is in flight)
    void record(const ct::Step& s) override {
        if (!alive()) return;
        records.push_back(s);
        if (rn) record_quiet.push_back(rn->quiet());
        if (jr && rn) {
            jr->note(rn->steps(), size_t(&s - rn->steps().data()));
            jr->settle(rn->run_id(), jopts, rn->evidence(), rn->steps());
        }
    }
    // Unit lifecycle model (docs/CHARGER_TEST_FLOW.md): detach() = Rd off, the source drops its output and its caps; attach(mode) =
    // Rd on, back_ms later VBUS is 5 V with fresh caps and a new session. A Pd attach comes with a 5 V contract at once; a Legacy
    // attach is PD-silent: the charger either falls back (quiet from then on) or stays in PD (re-sends its caps, never quiet).
    int detaches = 0, attaches_pd = 0, attaches_legacy = 0, restores = 0, rescans = 0, handovers = 0;
    std::vector<char> seq;   // 'D' detach, 'P' Pd attach, 'L' Legacy attach, in call order
    bool stays_pd = false, handed = false;
    bool contract_blocks_legacy = false;  // a legacy trigger inside a PD contract gets no handshake (a resumed unit has no detach)
    int fc_blocked = 0;
    int vbus_after_handover = 5000;
    uint32_t chatter_until = 0;   // a charger that still sends caps until this time, then goes quiet
    // CJ21-style give-up: after the Legacy attach it cycles 4 s caps, ~1 s hard reset (VBUS low, ~0.8 s of quiet after
    // VBUS is back before caps resume) until it disables PD at give_up_ms after the attach.
    uint32_t give_up_ms = 0, handed_at = 0;
    // pattern silent-sink-output-off (GaN3 65W): in every PD-silent attach the source switches its output off silent_off_ms after the
    // attach (VBUS 0, the Rp gone: one new session) and keeps it off until the next PD attach.
    uint32_t silent_off_ms = 0, silent_off_seen = 0;
    // Rd off -> the source drops its output (vbus 2.5 V for dark_ms, then 0) and its caps; Rd on -> back_ms later VBUS is 5 V with
    // fresh caps, a new session and a 5 V contract (Pd mode), unless the source stays silent / dark / never lets go.
    bool rd_on = true, never_dark = false, silent_after_rd = false, no_vbus_after_rd = false;
    bool no_contract = false;          // a Pd attach whose 5 V contract never comes
    uint32_t contract_delay_ms = 0, contract_at = 0;   // ... or comes this long after the caps
    uint32_t dark_ms = 1500, back_ms = 700, rd_off_at = 0, rd_on_at = 0; bool back_done = false;
    // pattern shared-bus-split: the Pd attach's 5 V contract (PS_RDY at PDO 1) lands, but the source holds this
    // voltage on VBUS instead of 5 V (0 = 5 V). wrong_decay_rel / wrong_decay_mv: the reading falls to that level
    // this long after the contract landed (the row must record the highest VBUS of the wait, not the timeout reading).
    int attach_wrong_mv = 0; uint32_t wrong_decay_rel = 0, wrong_decay_at = 0; int wrong_decay_mv = 0;
    // pattern slow-rp-wake: the source's Rp drops rp_drop_rel ms after the attach's Rd went on (caps invalid, no
    // active CC, VBUS dark) and its standby pull-up answers with fresh caps rp_back_rel ms after the Rd.
    uint32_t rp_drop_rel = 0, rp_back_rel = 0; bool rp_dropped = false, rp_done = false;
    ct::Mode attach_mode = ct::Mode::Pd;
    int fc_at_off = -1, pd_at_off = -1;
    pd::SourceCaps saved;   // the list the charger announces again after an attach (a non-PD source has none)
    // A source that lists more PDOs once we answered its Get_Sink_Cap_Extended (UGREEN X865, 2026-11-04): after a Pd attach the
    // first list is `saved`; grow_after_ms later it announces `grown`.
    pd::SourceCaps grown; uint32_t grow_after_ms = 0, grow_at = 0;
    void detach() override {
        if (!alive()) return;
        if (caps.valid) saved = caps;
        ++detaches; seq.push_back('D'); rd_on = false; rd_off_at = in.now; fc_at_off = fc_requests; pd_at_off = pd_requests;
        handed = false; caps.valid = false; in.pd.state = pd::SinkState::Idle; in.pd.held = false; in.pd.epr_mode = false;
        in.pd.epr_info = pd::EprInfo::None; info_shown = {};
        in.pd.mv = 5000; in.fc_state = fc::State::Idle; low_now = false;
    }
    void attach(ct::Mode m) override {
        if (!alive()) return;
        (m == ct::Mode::Pd ? attaches_pd : attaches_legacy)++; seq.push_back(m == ct::Mode::Pd ? 'P' : 'L');
        ra_at_attach.push_back(ra_on ? 'R' : '-');
        rd_on = true; rd_on_at = in.now; back_done = false; attach_mode = m; fresh = true;
    }
    void legacy_only(bool on) override { if (!on) { ++restores; handed = false; } }
    // The cable identity (Ra mode + spoof) of the run: the Runner asks for it on at a Pd-mode attach and off at a Legacy one. A
    // charger that goes PD-deaf to the D+/D- handshakes while the Ra mode is on (Baseus 70W, 2026-11-07) fails every Legacy
    // request until identity(false). ra_log = the calls ('R' on, 'r' off); ra_at_attach = the Ra state each attach found.
    bool ra_on = false, ra_dead_legacy = false; int ra_dead_fc = 0;
    std::string ra_log, ra_at_attach;
    void identity(bool on) override { if (!alive()) return; ra_on = on; ra_log.push_back(on ? 'R' : 'r'); }
    // A self-powered K2 never opens CC: hold(mode) is the unit boundary that keeps the pinned Rd and VBUS. A Pd hold is a
    // Soft_Reset (the source announces its list again, a 5 V contract follows); a Legacy hold is PD-silent. VBUS stays up
    // the whole time (keep_vbus), the attach session still changes once the source has answered.
    int holds = 0, holds_pd = 0, holds_legacy = 0, min_vbus = 1 << 20; bool keep_vbus = false; uint32_t hold_ms = 400;
    void hold(ct::Mode m) override {
        if (!alive()) return;
        if (caps.valid) saved = caps;
        ++holds; (m == ct::Mode::Pd ? holds_pd : holds_legacy)++; seq.push_back(m == ct::Mode::Pd ? 'h' : 'l');
        keep_vbus = true; rd_on = true; rd_on_at = in.now; back_done = false; attach_mode = m; fresh = true;
        handed = false; caps.valid = false; in.pd.state = pd::SinkState::Idle; in.pd.held = false; in.pd.epr_mode = false;
        in.pd.epr_info = pd::EprInfo::None; info_shown = {}; in.pd.mv = 5000; in.fc_state = fc::State::Idle;
    }
    // A charger that answers one handshake per CC re-attach (stale after the first legacy session of an attach).
    bool fussy = false, fresh = false; int fussy_fails = 0;
    // Source probe model (the page asks the charger for its identity at 5 V): 0 = not available, 1 = the charger answers
    // after probe_answer_ms, 2 = it ignores the probe (the page never reports it done).
    int probe_mode = 0, probe_starts = 0, probe_stops = 0;
    uint32_t probe_at = 0, probe_answer_ms = 100; bool probing = false;
    bool probe_identity(bool on) override {
        if (on) { ++probe_starts; probe_at = in.now; probing = probe_mode != 0; return probe_mode != 0; }
        ++probe_stops; probing = false; return false;
    }
    bool rescan_low = false, low_now = false;   // a source that sags below vSafe5V after the attach and never settles
    void pd_rescan() override { ++rescans; }
    Fake() {
        in.vbus_mv = 5000; in.cap_mv = 36000; in.session = 1; in.xm = &xm;
        in.cable_mv = 50000; in.cable_ma = 5000; in.cable_observed = true;
        caps.valid = true; caps.spec_rev = 2; caps.active_cc = 1; caps.num_pdos = 7; caps.raw_header = 0x7181;
        const unsigned volts[] = {5000, 9000, 12000, 15000, 20000};
        for (unsigned n = 0; n < 5; ++n)
            caps.pdos[n] = pd::decode_pdo(((volts[n] / 50) << 10) | 325);
        caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (50u << 8) | 64);
        caps.pdos[6] = pd::decode_pdo(0xc0000000 | (210u << 17) | (50u << 8) | 64);
    }
    void tick(ct::Runner& r, unsigned ms) {
        for (unsigned n = 0; n < ms; n += step_ms) {
            in.now += step_ms;
            in.pd.xiaomi_svid = xiaomi_svid && in.now >= svid_back_at; in.pd.xiaomi_authed = in.pd.xiaomi_svid && auth_at && in.now >= auth_at;
            if (auth_caps_on && in.pd.xiaomi_authed && in.now >= auth_at + auth_caps_delay_ms) { auth_caps_on = false; caps = auth_caps; xm_reannounce = true; }
            xm.authed = in.pd.xiaomi_authed; xm.authed_at = xm.authed ? auth_at : 0; xm.reannounced = xm.authed && xm_reannounce;
            xm.last_step = xm_last_step; xm.opener_sent = xiaomi_svid;
            if (collapse_t0 && in.now > collapse_t0) {   // pattern pps-off-at-no-load: the output falls, the port then restarts
                const uint32_t t = in.now - collapse_t0;
                in.ibus_ma = collapse_ma;
                if (t == 100) in.vbus_mv = 3500;
                else if (t == 200) in.vbus_mv = 1800;
                else if (t == 300) { in.vbus_mv = 1800; ++in.session; in.pd.state = pd::SinkState::Idle; in.pd.held = false; in.pd.mv = 5000; }
                else if (t == 2500) {   // the port restarts by itself: 5 V, a contract at PDO 1
                    collapse_t0 = 0; in.ibus_ma = 0; in.vbus_mv = 5000;
                    in.pd.state = pd::SinkState::Ready; in.pd.result = pd::SinkResult::Ok; in.pd.pos = 1; in.pd.mv = 5000;
                }
            }
            if (!rd_on && !never_dark) in.vbus_mv = in.now - rd_off_at < dark_ms ? 2500 : 0;
            if (rd_on && !back_done && rd_on_at && !never_dark) {
                if (no_vbus_after_rd) in.vbus_mv = 0;
                else if (in.now - rd_on_at >= (keep_vbus ? hold_ms : back_ms)) {
                    back_done = true; keep_vbus = false; in.vbus_mv = attach_mode == ct::Mode::Legacy ? vbus_after_handover : 5000;
                    ++in.session; if (!silent_after_rd && saved.valid) { caps = saved; caps.valid = true; caps.active_cc = 1; }
                    if (grow_after_ms && attach_mode == ct::Mode::Pd && saved.valid) grow_at = in.now + grow_after_ms;
                    if (attach_mode == ct::Mode::Pd) {
                        if (!silent_after_rd && saved.valid && !no_contract) {
                            if (contract_delay_ms) { contract_at = in.now + contract_delay_ms; in.pd.state = pd::SinkState::Idle; }
                            else {
                                in.pd.state = pd::SinkState::Ready; in.pd.result = pd::SinkResult::Ok; in.pd.mv = 5000; in.pd.pos = 1; in.pd.held = false;
                                if (attach_wrong_mv) in.vbus_mv = attach_wrong_mv;
                                if (wrong_decay_rel) wrong_decay_at = in.now + wrong_decay_rel;
                            }
                        }
                    } else { ++handovers; handed = true; handed_at = in.now; in.pd_quiet_ms = saved.valid && !silent_after_rd ? 0 : 1000000; }
                } else if (!keep_vbus) in.vbus_mv = 0;
            }
            if (handed && give_up_ms) {
                const uint32_t t = in.now - handed_at, ph = t % 5000;
                if (t >= give_up_ms) { in.vbus_mv = 5100; in.pd_quiet_ms = std::min<uint32_t>(in.pd_quiet_ms + 100, 1000000); }
                else if (ph < 4000) { in.vbus_mv = 5100; in.pd_quiet_ms = 0; }
                else if (ph < 4200) { in.vbus_mv = 3500; in.pd_quiet_ms = ph - 4000; }
                else { in.vbus_mv = 5100; in.pd_quiet_ms = ph - 4000; }
            } else if (handed) {
                if (stays_pd || in.now < chatter_until) in.pd_quiet_ms = 0;
                else in.pd_quiet_ms = std::min<uint32_t>(in.pd_quiet_ms + 100, 1000000);
            }
            if (grow_at && in.now >= grow_at) { grow_at = 0; caps = grown; caps.valid = true; caps.active_cc = 1; }
            if (handed && silent_off_ms && in.now - handed_at >= silent_off_ms) {
                if (silent_off_seen != handed_at) { silent_off_seen = handed_at; ++in.session; }
                in.vbus_mv = 0; in.pd_quiet_ms = 1000000;
            }
            if (contract_at && in.now >= contract_at) { contract_at = 0; in.pd.state = pd::SinkState::Ready; in.pd.result = pd::SinkResult::Ok; in.pd.mv = 5000; in.pd.pos = 1; in.pd.held = false; }
            if (wrong_decay_at && in.now >= wrong_decay_at) { wrong_decay_at = 0; in.vbus_mv = wrong_decay_mv; }
            if (rp_drop_rel && rd_on && back_done && !rp_dropped && !rp_done && in.now - rd_on_at >= rp_drop_rel) {
                rp_dropped = true; caps.valid = false; caps.active_cc = 0; in.vbus_mv = 0;
                in.pd.state = pd::SinkState::Idle; in.pd.held = false; in.pd.mv = 5000;
            }
            if (rp_back_rel && rp_dropped && !rp_done && in.now - rd_on_at >= rp_back_rel) {
                rp_dropped = false; rp_done = true; if (saved.valid) { caps = saved; caps.valid = true; caps.active_cc = 1; }
                in.vbus_mv = 5000; in.pd.state = pd::SinkState::Ready; in.pd.result = pd::SinkResult::Ok;
                in.pd.mv = 5000; in.pd.pos = 1; in.pd.held = false; ++in.session;
            }
            if (low_now) in.vbus_mv = 4200;
            if (latch_at && in.now >= latch_at) { latch_at = 0; latched = true; latched_at = in.now; ++in.session; }
            if (latched && recover_after_ms && in.now - latched_at >= recover_after_ms) {
                latched = false; latch_mv = 0; recover_after_ms = 0; in.vbus_mv = 5000; ++in.session; ++in.pd.source_hard_resets;
                caps.valid = false; caps_back_at = in.now + caps_after_ms;
            }
            if (caps_back_at && in.now >= caps_back_at) {
                caps_back_at = 0; caps.valid = true; in.pd.state = pd::SinkState::Ready; in.pd.result = pd::SinkResult::Ok;
                in.pd.mv = 5000; in.pd.pos = 1; in.pd.held = false;
            }
            if (reset_at && in.now >= reset_at) { reset_at = 0; source_hard_reset(); }
            if (hr_full_at && in.now >= hr_full_at) { hr_full_at = 0; caps = hr_full; }
            if (reannounce_at && in.now >= reannounce_at) { reannounce_at = 0; caps = reannounce_caps; }
            if (dip_until) { if (in.now < dip_until) in.vbus_mv = 3000; else { dip_until = 0; in.vbus_mv = 5000; } }
            if (latched) { in.vbus_mv = 0; caps.valid = false; in.pd.state = pd::SinkState::Idle; in.pd.held = false; in.pd.mv = 5000; }
            if (low_until) { if (in.now < low_until) in.vbus_mv = gone ? 0 : 4450; else { low_until = 0; in.vbus_mv = 5000; if (late_session_ms) late_session_at = in.now + late_session_ms; } }
            if (late_session_at && in.now >= late_session_at) { late_session_at = 0; ++in.session; }
            in.probe_done = probing && probe_mode == 1 && in.now - probe_at >= probe_answer_ms;
            if (in.pd.epr_info == pd::EprInfo::Pending && in.now >= info_at) {
                in.pd.epr_info = info_mode == 1 ? pd::EprInfo::Got : info_mode == 2 ? pd::EprInfo::Refused : pd::EprInfo::Timeout;
                if (info_mode == 1) info_shown = info_list;
            }
            in.epr_info = &info_shown;
            if (settle_on) {   // the scripted settle keeps moving VBUS between polls
                const uint32_t t = in.now - settle_t0;
                if (t >= walk_settle_ms) { settle_on = false; in.vbus_mv = walk_settle_to_mv; }
                else in.vbus_mv = walk_settle_from_mv + (walk_settle_to_mv - walk_settle_from_mv) * int(t) / int(walk_settle_ms);
            }
            if (walk_ripple_mv && in.vbus_mv > 1000) in.vbus_mv += ((in.now / 100) % 2) ? walk_ripple_mv : -walk_ripple_mv;
            if (out_on) {   // the scripted output of out_script_mv's Request
                const uint32_t t = in.now - out_t0;
                int v = out_script.back().second;
                for (size_t k = 1; k < out_script.size(); ++k)
                    if (t < out_script[k].first) {
                        const auto a = out_script[k - 1], b = out_script[k];
                        v = a.second + int(int64_t(b.second - a.second) * int(t - a.first) / int(b.first - a.first));
                        break;
                    }
                in.vbus_mv = v;
            }
            if (ka_period_ms && ka_at && in.now >= ka_at && in.pd.state == pd::SinkState::Ready) {   // the sink's keep-alive Request
                ka_at = 0; in.pd.state = pd::SinkState::Requesting; in.pd.result = pd::SinkResult::None;
                ka_end = in.now + ka_windows[ka_i++ % ka_windows.size()];
            }
            if (ka_end && in.now >= ka_end) {   // PS_RDY
                ka_end = 0; in.pd.state = pd::SinkState::Ready; in.pd.result = pd::SinkResult::Ok; ka_at = in.now + ka_period_ms;
            }
            // noise_mv: the SAMPLE swings +-noise_mv around the level (burst-mode ripple); the level itself is untouched. noise_flip
            // flips the swing's phase with every PD request, so two levels read by one sample each sit on opposite sides.
            const int before = in.vbus_mv;
            int bump = 0;
            if (noise_mv && before > 1000) bump = (((in.now / 100) % 2) != 0) != (noise_flip && (pd_requests & 1)) ? noise_mv : -noise_mv;
            in.vbus_mv += bump;
            r.poll(in, caps);
            if (bump && in.vbus_mv == before + bump) in.vbus_mv = before;
            min_vbus = std::min(min_vbus, in.vbus_mv);
        }
    }
};
// Ticks until the predicate holds (100 ms steps, bounded); false when it never does.
template <class Pred>
static bool run_until(Fake& f, ct::Runner& r, Pred pred, unsigned max_ms = 30000) {
    for (unsigned n = 0; n < max_ms && !pred(); n += 100) f.tick(r, 100);
    return pred();
}
static void read_only(const ct::Runner& r, const Fake& f) {
    const auto revision = r.revision(), run_id = r.run_id();
    const auto state = r.state(); const auto index = r.index();
    const auto requests = f.pd_requests + f.fc_requests, releases = f.releases;
    const auto records = f.records.size();
    const auto snap = remote::charger_test_snapshot(r);
    CHECK_EQ(snap.steps.data() == r.steps().data(), true);
    std::array<uint8_t, remote::kMaxRspPayload> a{}, b{};
    for (size_t n = 0; n < r.steps().size(); ++n) {
        const auto len = remote::encode_charger_test(a, snap, n, true, 2);
        CHECK_EQ(len, remote::encode_charger_test(b, remote::charger_test_snapshot(r), n, true, 2));
        CHECK_EQ(a == b, true);
    }
    CHECK_EQ(r.state(), state); CHECK_EQ(r.index(), index);
    CHECK_EQ(r.revision(), revision); CHECK_EQ(r.run_id(), run_id);
    CHECK_EQ(f.pd_requests + f.fc_requests, requests); CHECK_EQ(f.releases, releases);
    CHECK_EQ(f.records.size(), records);
}
static void test_plan_and_run() {
    Fake f; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, {}, f.in, true), ct::Reason::None);
    read_only(r, f);
    unsigned fixed = 0, pps = 0, skips = 0;
    for (const auto& s : r.steps()) {
        fixed += s.algorithm == A::Pd3Fixed;
        skips += s.verdict == ct::Verdict::Skip;
        if (s.algorithm == A::Pps) {
            // The 6 A hunt probes carry ma = 6000; the full-current row of each APDO asks its advertised 3.2 A.
            ++pps; if (!s.hunt) CHECK_EQ(s.ma == 1000 || s.ma == 3200, true);
            CHECK_EQ(s.mv >= 5000 && s.mv <= (s.pos == 6 ? 11000 : 21000), true);
        }
    }
    CHECK_EQ(fixed, 5); CHECK_EQ(pps, 16); CHECK_EQ(skips, 3); CHECK_EQ(r.steps().size(), 65);   // 40 legacy rows (Class B included) and the vendor rows, the PD rows + 2 hunt rows (11V6 on the first APDO, 20V6 on the 5-21 V one) + 2 full-current rows (11V 3.2A, 20V 3.2A) + 2 step-walk rows (one per APDO)
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    read_only(r, f);
    // Every 5 V row of a legacy unit asks the engine (the attach's own 5 V proves nothing): QC3 / QC3+ ramps retarget four times each.
    // 17 = 19 - the 2 hunt rows: this source advertises its APDOs at 3.2 A, so no 6 A Request goes out (decision 2026-11-07).
    // + 12 for the two step-walk rows (six Requests each: the mid level, then +20 mV x 5): 29.
    CHECK_EQ(f.pd_requests, 29); CHECK_EQ(f.fc_requests, 41); CHECK_EQ(f.retargets, 24);   // + the six Class B rows (QC2B 20 V, the QC3B ramp); retargets: QC2 9/12/20 V, QC3 x4, QC3B x5 (all five go on in the QC3 session), QC3+ x4, UFCS 9/11/5/12/20 V, AFC 12 V, FCP 9/12 V
    // Seven vendor units share ONE PD-silent attach (a session boundary only where the session mode changes), then the Pd unit
    // (detach, Pd attach), then the standard protocols (QC2, QC3, QC3+, UFCS) in a second PD-silent attach; the run ends detached
    // and re-attaches in Pd.
    CHECK_EQ(std::string(f.seq.begin(), f.seq.end()) == "DLDPDLDP", true);
    CHECK_EQ(f.in.vbus_mv, 5000);
    for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Skip, true);
}
// The driver's own PDO 1 answer is no hold (hardware 2026-11-11): a release that lands on it is a release. Before the mark,
// a held=True 5 V contract nobody owned kept every row's release window open until its timeout.
static void test_an_auto_hold_counts_as_released() {
    // Unfixed, every PD release waited out its 8 s window ("release check not answering"), then carried on: same verdicts, a
    // much longer run. So the proof is the run time, against the same run with a plain release.
    auto run_ms = [](bool auto_hold) {
        Fake f; ct::Runner r(f);
        f.release_auto_hold = auto_hold;
        CHECK_EQ(r.start(f.caps, {}, f.in, true), ct::Reason::None);
        const uint32_t t0 = f.in.now;
        for (unsigned n = 0; n < 2000 && r.state() != ct::State::Done; n++) f.tick(r, 100);
        CHECK_EQ(r.state(), ct::State::Done);
        for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Skip, true);
        return f.in.now - t0;
    };
    const uint32_t plain = run_ms(false), with_auto = run_ms(true);
    CHECK_EQ(with_auto <= plain + 1000, true);
}
// The Runner never drains the journal: an above-5 V request that would start a contract waits in the Baseline (the contract at 5 V,
// polled again) until the journal has written the plan, and the wait is reported once when the request goes.
static void test_an_above_5v_request_waits_for_the_journal_without_blocking() {
    Fake f; ct::Runner r(f);
    ct::Options o{.selected = ct::bit(A::Pd3Fixed)}; o.units.add(ct::Unit::Pd);
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.journal_blocked = true;
    f.tick(r, 20000);
    // The 5 V row may run; nothing above 5 V is asked, the run is parked in the Baseline and the clock of its wait runs.
    CHECK_EQ(f.pd_requests <= 1, true);
    CHECK_EQ(r.state() == ct::State::Baseline || r.state() == ct::State::Holding || r.state() == ct::State::Requesting, true);
    CHECK_EQ(f.waited.empty(), true);                 // not reported while it still waits
    for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Running, false);
    CHECK_EQ(f.in.vbus_mv <= 5000, true);
    f.journal_blocked = false;                        // the last file went to flash
    f.tick(r, 5000);
    CHECK_EQ(f.waited.size() >= 1, true);             // the park was reported, with how long it lasted
    CHECK_EQ(f.waited[0] >= 10000, true);             // parked since the first 5 V row (the Baseline's own settling takes a few seconds)
    CHECK_EQ(f.pd_requests >= 2, true);               // and the run went on
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
}
// quiet() = a row is in flight (Requesting / Holding): the page paints and journals nothing heavy then. The gaps (Baseline,
// Releasing, the unit attach / detach states, the end of the run) are not quiet, and upcoming() names the row that comes next.
static void test_quiet_is_exactly_the_row_in_flight() {
    Fake f; ct::Runner r(f); f.rn = &r;
    CHECK_EQ(r.quiet(), false);                       // idle
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    CHECK_EQ(r.quiet(), false);                       // Baseline: the first row has not begun
    CHECK_EQ(r.upcoming() < r.steps().size(), true);
    CHECK_EQ(r.steps()[r.upcoming()].verdict, ct::Verdict::Pending);
    unsigned quiet_ticks = 0, releasing = 0, baseline = 0;
    for (unsigned n = 0; n < 4000 && r.state() != ct::State::Done; ++n) {
        f.tick(r, 100);
        const auto st = r.state();
        CHECK_EQ(r.quiet(), st == ct::State::Requesting || st == ct::State::Holding);
        quiet_ticks += r.quiet(); releasing += st == ct::State::Releasing; baseline += st == ct::State::Baseline;
    }
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(quiet_ticks > 0, true); CHECK_EQ(releasing > 0, true); CHECK_EQ(baseline > 0, true);
    CHECK_EQ(r.quiet(), false);                       // a finished run is not in flight
    CHECK_EQ(r.upcoming(), SIZE_MAX);                 // nothing left to run
    // Every verdict is decided while its row is in flight: that is exactly the moment the page must not write flash.
    CHECK_EQ(f.record_quiet.size(), f.records.size());
    unsigned in_flight = 0;
    for (const bool q : f.record_quiet) in_flight += q;
    CHECK_EQ(in_flight > 0, true);
}
// paint_blocked(): nothing may paint = a row is in flight AND the engine is inside a window only the main loop serves. A row in
// flight outside such a window (a hook-served UFCS exchange, a settle, a PD transition) may paint a bounded amount; the gaps and
// the end of the run are free whatever the engine says; an idle runner is never blocked.
static void test_paint_blocked_needs_flight_and_an_engine_window() {
    Fake f; ct::Runner r(f); f.rn = &r;
    CHECK_EQ(r.paint_blocked(true), false);           // idle
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    unsigned blocked = 0, flight = 0, gap_ticks = 0;
    for (unsigned n = 0; n < 4000 && r.state() != ct::State::Done; ++n) {
        f.tick(r, 100);
        CHECK_EQ(r.paint_blocked(false), false);      // no engine window: never blocked, in flight or not
        const bool q = r.quiet();
        CHECK_EQ(r.paint_blocked(true), q);           // an engine window blocks exactly while the row is in flight
        blocked += r.paint_blocked(true); flight += q; gap_ticks += !q;
    }
    CHECK_EQ(blocked, flight);
    CHECK_EQ(flight > 0, true); CHECK_EQ(gap_ticks > 0, true);
    CHECK_EQ(r.paint_blocked(true), false);           // a finished run
}
// The 6 A hunt rows of a plan, in plan order.
static std::vector<ct::Step> hunt_rows(const ct::Runner& r) {
    std::vector<ct::Step> out;
    for (const auto& s : r.steps()) if (s.hunt) out.push_back(s);
    return out;
}
// A CUKTECH-like list (hardware 2026-11-06, before the Xiaomi auth chain): 5/9/12/15 V 3 A, 20 V 5 A, PPS 5-11 V 5 A, PPS 5-20 V 5 A.
// `apdo_ma` = the PPS APDOs' advertised current: 5000 before the unlock, 6000 after it (the source re-announces them at 6 A). A 6 A
// hunt row only goes out against a list that advertises 6 A, so a test about the Request itself (Reject, Hard Reset, pass) uses 6000.
static pd::SourceCaps cuktech_caps(unsigned apdo_ma = 5000) {
    pd::SourceCaps c; c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.num_pdos = 7; c.raw_header = 0x7181;
    const unsigned volts[] = {5000, 9000, 12000, 15000, 20000};
    for (unsigned n = 0; n < 5; ++n) c.pdos[n] = pd::decode_pdo(((volts[n] / 50) << 10) | (n == 4 ? 500 : 300));
    c.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (50u << 8) | (apdo_ma / 50));
    c.pdos[6] = pd::decode_pdo(0xc0000000 | (200u << 17) | (50u << 8) | (apdo_ma / 50));
    return c;
}
static void test_hunt_runs_on_the_pps_apdo_that_covers_the_voltage() {
    Fake f; f.caps = cuktech_caps(); ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    const auto h = hunt_rows(r);
    // 11 V 6 A on the first APDO that covers it (pos 6), 20 V 6 A on the 5-20 V APDO (pos 7); the fixed 20 V PDO gets no 6 A row:
    // the fixed PDO never offers 6 A (the charger answers Reject) and the card would carry that as noise.
    CHECK_EQ(h.size(), 2);
    if (h.size() == 2) {
        CHECK_EQ(h[0].algorithm, A::Pps); CHECK_EQ(h[0].pos, 6); CHECK_EQ(h[0].mv, 11000); CHECK_EQ(h[0].ma, 6000);
        CHECK_EQ(h[1].algorithm, A::Pps); CHECK_EQ(h[1].pos, 7); CHECK_EQ(h[1].mv, 20000); CHECK_EQ(h[1].ma, 6000);
        CHECK_EQ(h[0].verdict, ct::Verdict::Pending); CHECK_EQ(h[1].verdict, ct::Verdict::Pending);
    }
    for (const auto& s : r.steps()) if (s.algorithm == A::Pd3Fixed) CHECK_EQ(s.ma != 6000, true);
}
// The preview (rows, runnable, estimate) counts what the run will do: a 6 A hunt row the list does not advertise is dropped at request
// time on a source that never showed the Xiaomi chain, so it is no row and no estimated time; with the chain seen it ends as a
// "not unlocked" skip (a row, but no time); a list that advertises 6 A keeps it runnable.
static void test_preview_does_not_count_hunt_rows_the_run_drops() {
    Fake f; f.caps = cuktech_caps(5000);
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    const auto planned = r.steps().size(), hunts = hunt_rows(r).size();
    CHECK_EQ(hunts, 2);
    const auto pv = ct::Runner::preview(f.caps, kPd, f.in);
    CHECK_EQ(pv.total, planned - hunts);   // the two dropped rows are not counted
    {   // the same list with 6 A advertised: the hunt rows run
        Fake g; g.caps = cuktech_caps(6000);
        ct::Runner rg(g);
        CHECK_EQ(rg.start(g.caps, kPd, g.in, true), ct::Reason::None);
        const auto pvg = ct::Runner::preview(g.caps, kPd, g.in);
        CHECK_EQ(pvg.total, rg.steps().size());
        CHECK_EQ(pvg.runnable > pv.runnable, true);
        CHECK_EQ(pvg.estimate_ms > pv.estimate_ms, true);
    }
    {   // the chain was seen: the unadvertised rows stay as skips, with no estimated time
        Fake x; x.caps = cuktech_caps(5000); x.in.pd.xiaomi_svid = true;
        const auto pvx = ct::Runner::preview(x.caps, kPd, x.in);
        CHECK_EQ(pvx.total, planned);
        CHECK_EQ(pvx.skipped, pv.skipped + hunts);
        CHECK_EQ(pvx.runnable, pv.runnable);
        CHECK_EQ(pvx.estimate_ms, pv.estimate_ms);
    }
}
static void test_fixed_only_source_keeps_the_fixed_20v_hunt() {
    Fake f; f.caps = cuktech_caps(); f.caps.num_pdos = 5; f.caps.pdos[5] = f.caps.pdos[6] = {};
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    const auto h = hunt_rows(r);
    CHECK_EQ(h.size(), 1);
    if (h.size() == 1) { CHECK_EQ(h[0].algorithm, A::Pd3Fixed); CHECK_EQ(h[0].pos, 5); CHECK_EQ(h[0].mv, 20000); CHECK_EQ(h[0].ma, 6000); }
}
static void test_fixed_20v_hunt_stays_when_no_apdo_reaches_20v() {
    Fake f; f.caps = cuktech_caps(); f.caps.num_pdos = 6; f.caps.pdos[6] = {};   // only the 5-11 V APDO
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    const auto h = hunt_rows(r);
    CHECK_EQ(h.size(), 2);
    if (h.size() == 2) {
        CHECK_EQ(h[0].algorithm, A::Pd3Fixed); CHECK_EQ(h[0].mv, 20000);
        CHECK_EQ(h[1].algorithm, A::Pps); CHECK_EQ(h[1].mv, 11000);
    }
}
static void test_fixed_20v_hunt_stays_when_the_pps_rows_are_not_selected() {
    Fake f; f.caps = cuktech_caps(); ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, {.selected = ct::bit(A::Pd3Fixed)}, f.in, true), ct::Reason::None);
    const auto h = hunt_rows(r);
    CHECK_EQ(h.size(), 1);
    if (h.size() == 1) { CHECK_EQ(h[0].algorithm, A::Pd3Fixed); CHECK_EQ(h[0].mv, 20000); }
}
// The full-current PPS row: the APDO's top voltage at its whole advertised current (clamped to the cable), after the APDO's
// other rows. A non-hunt row: it asks what the APDO offers, not the OEM 6 A.
static std::vector<ct::Step> full_rows(const ct::Runner& r) {
    std::vector<ct::Step> out;
    for (const auto& s : r.steps())
        if (s.algorithm == A::Pps && !s.hunt && s.ma > 1000 && s.verdict != ct::Verdict::Skip) out.push_back(s);
    return out;
}
// One PPS APDO: min_mv..max_mv at ma, behind a 5 V 3 A fixed PDO.
static pd::SourceCaps pps_caps(unsigned max_mv, unsigned ma) {
    pd::SourceCaps c; c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.num_pdos = 2; c.raw_header = 0x2181;
    c.pdos[0] = pd::decode_pdo(((5000 / 50) << 10) | 300);
    c.pdos[1] = pd::decode_pdo(0xc0000000 | ((max_mv / 100) << 17) | (50u << 8) | (ma / 50));
    return c;
}
static void test_pps_full_current_row_with_a_5a_cable() {
    Fake f; f.caps = pps_caps(11000, 5000); f.in.cable_mv = 20000; f.in.cable_ma = 5000; f.in.cable_observed = true;
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    const auto rows = full_rows(r);
    CHECK_EQ(rows.size(), 1);
    if (rows.size() == 1) { CHECK_EQ(rows[0].pos, 2); CHECK_EQ(rows[0].mv, 11000); CHECK_EQ(rows[0].ma, 5000); CHECK_EQ(rows[0].continuous, false); }
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    bool asked = false;
    for (const auto& c : f.choices) asked |= c.pos == 2 && c.mv == 11000 && c.ma == 5000 && !c.hunt;
    CHECK_EQ(asked, true);
}
static void test_pps_full_current_row_clamps_to_an_unread_cable() {
    Fake f; f.caps = pps_caps(11000, 5000); ct::Runner r(f);
    f.in.cable_mv = 20000; f.in.cable_ma = 3000; f.in.cable_observed = false;   // nothing read: the default cable is 3 A
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    const auto rows = full_rows(r);
    CHECK_EQ(rows.size(), 1);
    if (rows.size() == 1) { CHECK_EQ(rows[0].mv, 11000); CHECK_EQ(rows[0].ma, 3000); }
    f.tick(r, 200000);
    bool asked = false;
    for (const auto& c : f.choices) asked |= c.pos == 2 && c.mv == 11000 && c.ma == 3000 && !c.hunt;
    CHECK_EQ(asked, true);
    for (const auto& c : f.choices) CHECK_EQ(c.hunt || c.ma <= 3000, true);   // never above the cable
}
// A PPS APDO above 5 A (Motorola TurboPower: 5-20 V 6.25 A). No cable standard carries more than 5 A, so the full-current row asks
// the advertised current only for a cable the run vouches for above 5 A (a read VDO, or the spoofed identity); every other cable
// leaves the row a neutral CableLimit skip that still names the advertised current.
static std::vector<ct::Step> full_rows_any(const ct::Runner& r) {
    std::vector<ct::Step> out;
    for (const auto& s : r.steps()) if (s.algorithm == A::Pps && !s.hunt && s.ma > 5000) out.push_back(s);
    return out;
}
// The skip row of an APDO above 5 A behind a cable not vouched for above 5 A: neutral, kept apart from "every ordinary row passed".
static bool over5_cable_skip(const ct::Step& s) {
    return s.algorithm == A::Pps && !s.hunt && s.ma > 5000 && s.verdict == ct::Verdict::Skip && s.reason == ct::Reason::CableLimit;
}
static void test_pps_over_5a_apdo_with_a_5a_cable_is_a_cable_limit_skip() {
    Fake f; f.caps = pps_caps(20000, 6250); f.in.cable_mv = 20000; f.in.cable_ma = 5000; f.in.cable_observed = true;
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    const auto rows = full_rows_any(r);
    CHECK_EQ(rows.size(), 1);
    if (rows.size() == 1) {
        CHECK_EQ(rows[0].mv, 20000); CHECK_EQ(rows[0].ma, 6250);
        CHECK_EQ(rows[0].verdict, ct::Verdict::Skip); CHECK_EQ(rows[0].reason, ct::Reason::CableLimit);
    }
    f.tick(r, 200000);
    for (const auto& c : f.choices) CHECK_EQ(c.hunt || c.ma <= 5000, true);   // never above what the cable holds
    CHECK_STR(k2::step_label::range(5000, 20000, 6250).c_str(), "5-20V 6.25A");   // the chip keeps the advertised current
}
static void test_pps_over_5a_apdo_with_an_unread_cable_is_a_cable_limit_skip() {
    Fake f; f.caps = pps_caps(20000, 6250); f.in.cable_mv = 20000; f.in.cable_ma = 3000; f.in.cable_observed = false;
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    const auto rows = full_rows_any(r);
    CHECK_EQ(rows.size(), 1);
    if (rows.size() == 1) { CHECK_EQ(rows[0].verdict, ct::Verdict::Skip); CHECK_EQ(rows[0].reason, ct::Reason::CableLimit); }
}
static void test_pps_over_5a_apdo_with_a_spoofed_6a_cable_asks_the_advertised_current() {
    Fake f; f.caps = pps_caps(20000, 6250); f.in.cable_mv = 20000; f.in.cable_ma = 3000; f.in.cable_observed = false;
    f.in.spoof_on = true; f.in.spoof_mv = 20000; f.in.spoof_ma = 6000;
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    const auto rows = full_rows_any(r);
    CHECK_EQ(rows.size(), 1);
    if (rows.size() == 1) { CHECK_EQ(rows[0].mv, 20000); CHECK_EQ(rows[0].ma, 6250); CHECK_EQ(rows[0].verdict, ct::Verdict::Pending); }
    f.tick(r, 200000);
    bool asked = false;
    for (const auto& c : f.choices) { asked |= c.mv == 20000 && c.ma == 6250 && !c.hunt; CHECK_EQ(c.ma <= 6250, true); }   // never above the APDO
    CHECK_EQ(asked, true);
}
static void test_pps_full_current_row_of_a_2a_apdo() {
    Fake f; f.caps = pps_caps(20000, 2000); ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    const auto rows = full_rows(r);
    CHECK_EQ(rows.size(), 1);
    if (rows.size() == 1) { CHECK_EQ(rows[0].mv, 20000); CHECK_EQ(rows[0].ma, 2000); }
}
static void test_pps_full_current_row_follows_the_apdos_rows_and_hunt() {
    Fake f; f.caps = cuktech_caps(); f.in.cable_mv = 20000; f.in.cable_ma = 5000; f.in.cable_observed = true;
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    // Per APDO: the five 1 A targets, the 6 A hunt where it covers 11 / 20 V, then the full-current row last.
    const auto& st = r.steps();
    for (unsigned pos : {6u, 7u}) {
        int last = -1, hunt = -1, full = -1, top = -1;
        for (size_t i = 0; i < st.size(); ++i) {
            if (st[i].algorithm != A::Pps || st[i].pos != pos) continue;
            if (st[i].detail == ct::kDetailStepWalk) continue;   // the step-walk row is last but neither hunt nor full-current
            last = int(i);
            if (st[i].hunt) hunt = int(i); else if (st[i].ma > 1000) full = int(i); else top = int(i);
        }
        CHECK_EQ(full >= 0, true); CHECK_EQ(full, last); CHECK_EQ(hunt >= 0 && hunt < full, true); CHECK_EQ(top < full, true);
        if (full >= 0) { CHECK_EQ(st[full].mv, pos == 6 ? 11000 : 20000); CHECK_EQ(st[full].ma, 5000); }
    }
}
static void test_pps_full_current_row_is_not_a_duplicate_of_the_1a_row() {
    Fake f; f.caps = pps_caps(11000, 1000); ct::Runner r(f);   // a 1 A APDO: the 1 A row at the top is already the full current
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    CHECK_EQ(full_rows(r).size(), 0);
}
static void test_pps_full_current_row_off_when_pps_is_not_selected() {
    Fake f; f.caps = pps_caps(11000, 5000); ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, {.selected = ct::bit(A::Pd3Fixed)}, f.in, true), ct::Reason::None);
    CHECK_EQ(full_rows(r).size(), 0);
}
// Three PPS APDOs behind a legal list (SPR holds at most seven PDOs): every APDO adds one row, the plan still fits.
static void test_pps_full_current_rows_fit_the_plan_of_a_legal_list() {
    Fake f; f.caps = pps_caps(11000, 5000); f.caps.num_pdos = 7;
    for (unsigned n = 2; n < 7; ++n) f.caps.pdos[n] = pd::decode_pdo(0xc0000000 | ((3000u + 3000u * n) / 100 << 17) | (50u << 8) | 100);
    f.in.cable_mv = 20000; f.in.cable_ma = 5000; f.in.cable_observed = true;
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    CHECK_EQ(full_rows(r).size(), 6);
    CHECK_EQ(r.steps().size() <= ct::kMaxSteps, true);
}
// The worst legal list for the PPS rows: 7 PPS APDOs (a source never lists more SPR PDOs) all reaching beyond the voltage cap, so
// each APDO adds its five rows, the cap skip and the full-current row; every algorithm selected, EPR entries behind them. The
// plan keeps every row (a row past kMaxSteps would be dropped silently by Runner::add).
static void test_pps_full_current_rows_never_overflow_the_plan() {
    Fake many; many.in.cap_mv = 15000; many.caps.num_pdos = 11; many.caps.epr = true;
    many.caps.pdos[0] = pd::decode_pdo(((5000 / 50) << 10) | 300);   // PDO1 is always the fixed 5 V
    for (unsigned i = 1; i < 7; ++i)
        many.caps.pdos[i] = pd::decode_pdo(0xc0000000 | ((3000u + 3000u * i) / 100 << 17) | (50u << 8) | 100);
    for (unsigned i = 7; i < 11; ++i) many.caps.pdos[i] = pd::decode_pdo(0xD0000000 | (480u << 17) | (150u << 8) | 140);
    ct::Runner mr(many); const auto preview = ct::Runner::preview(many.caps, {}, many.in);
    mr.start(many.caps, {}, many.in, true);
    CHECK_EQ(mr.steps().size(), preview.total); CHECK_EQ(mr.steps().size(), ct::kMaxSteps);   // 113 planned: the last step-walk row is the one kMaxSteps drops (walk rows are emitted last per APDO)
    CHECK_EQ(mr.steps().size() <= ct::kMaxSteps, true);
    size_t full = 0;
    for (const auto& s : mr.steps()) full += s.algorithm == A::Pps && !s.hunt && s.ma > 1000 && s.verdict != ct::Verdict::Skip;
    CHECK_EQ(full, 6);
}
// One run of the CUKTECH-like source with the Xiaomi flags given; returns the Fake after the run ended.
// `apdo_ma`: what the PPS APDOs advertise (6000 = the source already offers the 6 A the hunt rows ask for).
static Fake* xiaomi_run(Fake& f, ct::Runner& r, bool svid, uint32_t auth_at, unsigned apdo_ma = 6000) {
    f.caps = cuktech_caps(apdo_ma); f.xiaomi_svid = svid; f.auth_at = auth_at;
    r.start(f.caps, kPd, f.in, true);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    return &f;
}
static void test_hunt_waits_for_the_xiaomi_auth_chain() {
    // Reference: no SVID 0x2717, the hunt rows go out as soon as their turn comes.
    Fake base; ct::Runner rb(base); xiaomi_run(base, rb, false, 0);
    CHECK_EQ(base.hunt_times.size(), 2);
    const uint32_t t0 = base.hunt_times[0];
    // The chain completes 1 s after the first hunt row's turn: the row waits and then goes out with the chain done.
    Fake a; ct::Runner ra(a); xiaomi_run(a, ra, true, t0 + 1000);
    CHECK_EQ(a.hunt_times.size(), 2);
    CHECK_EQ(a.hunt_authed[0], true); CHECK_EQ(a.hunt_authed[1], true);
    CHECK_EQ(a.hunt_times[0] >= t0 + 1000, true); CHECK_EQ(a.hunt_times[0] <= t0 + 1000 + 600, true);
    // Chain already done: no delay at all.
    Fake d; ct::Runner rd(d); xiaomi_run(d, rd, true, 1);
    CHECK_EQ(d.hunt_times.size(), 2); CHECK_EQ(d.hunt_times[0], t0);
    // A source that lists the SVID but never finishes the chain: the wait is bounded, the row still runs (a Reject is its verdict).
    Fake n; ct::Runner rn(n); xiaomi_run(n, rn, true, 0);
    CHECK_EQ(n.hunt_times.size(), 2); CHECK_EQ(n.hunt_authed[0], false);
    // The budget runs from the moment the row is next (its predecessor's release, ~0.5 s of steady 5 V before the row).
    CHECK_EQ(n.hunt_times[0] >= t0 + ct::kXiaomiAuthWaitMs - 1000, true); CHECK_EQ(n.hunt_times[0] <= t0 + ct::kXiaomiAuthWaitMs + 600, true);
    // The budget is per attach, not per hunt row: the second row follows its predecessors at the unwaited pace.
    CHECK_EQ(n.hunt_times[1] - n.hunt_times[0], base.hunt_times[1] - base.hunt_times[0]);
    for (const auto& s : rn.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Skip, true);
}
// A CUKTECH-like run whose 6 A hunt rows meet a source that does not offer 6 A. svid / auth_at: the Xiaomi chain; cable_ma_seen:
// the cable's current when it was read (0 = nobody read it, the run assumes 3 A); `silent`: no answer at all instead of a Reject.
// `apdo_ma` 6000 (default): the source advertises the 6 A, so the rows ask and are refused; 5000: it does not, the rows end unasked.
static Fake* locked_run(Fake& f, ct::Runner& r, bool svid, uint32_t auth_at, unsigned cable_ma_seen, bool silent = false,
                        unsigned apdo_ma = 6000) {
    f.reject_hunts = !silent; f.silent_hunts = silent;
    f.in.cable_observed = cable_ma_seen != 0; f.in.cable_ma = cable_ma_seen ? cable_ma_seen : 3000; f.in.cable_mv = 20000;
    return xiaomi_run(f, r, svid, auth_at, apdo_ma);
}
static void expect_not_unlocked(const ct::Runner& r, uint8_t detail, const char* words) {
    const auto h = hunt_rows(r);
    CHECK_EQ(h.size(), 2);
    for (const auto& s : h) {
        CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::NotUnlocked); CHECK_EQ(s.detail, detail);
        CHECK_STR(ct::explanation(s), words);
    }
}
static void test_hunt_the_source_does_not_offer_reads_not_unlocked() {
    // A charger with no SVID 0x2717 never had a chain: there is nothing to unlock.
    Fake a; ct::Runner ra(a); locked_run(a, ra, false, 0, 5000);
    expect_not_unlocked(ra, ct::kDetailNoChain, "not unlocked (no OEM chain)");
    // The chain was listed but never finished inside the wait.
    Fake b; ct::Runner rb(b); locked_run(b, rb, true, 0, 5000);
    expect_not_unlocked(rb, ct::kDetailChainUnfinished, "not unlocked (chain unfinished)");
    // The chain finished, the 6 A is still refused and the cable is not known to carry more than 3 A.
    Fake c; ct::Runner rc(c); locked_run(c, rc, true, 1, 0);
    expect_not_unlocked(rc, ct::kDetailCableCap, "not unlocked (3 A cable)");
    Fake d; ct::Runner rd(d); locked_run(d, rd, true, 1, 3000);
    expect_not_unlocked(rd, ct::kDetailCableCap, "not unlocked (3 A cable)");
    // No answer at all reads the same as a Reject.
    Fake e; ct::Runner re(e); locked_run(e, re, true, 1, 0, true);
    expect_not_unlocked(re, ct::kDetailCableCap, "not unlocked (3 A cable)");
    // The run is not a failure: every other row is untouched and the whole run completes.
    for (const auto& s : ra.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Skip, true);
}
// A source that only ASKS our SVIDs (a Discover SVIDs REQUEST, as the Lenovo 65 W laptop brick does) is not Xiaomi-family: the real sink
// leaves xiaomi_svid down, so the 6 A hunt rows are dropped from the card instead of reading "not unlocked (chain unfinished)"
// (hardware 2026-11-08). The sink's flag feeds the run the way the app does it.
static void test_source_that_only_asks_our_svids_drops_the_hunt_rows() {
    pd::SourceCaps caps = cuktech_caps(5000);
    pd::Sink sink; sink.bind(&caps); sink.set_cap_mv(36000);
    uint32_t now = 1000;
    auto pump = [&] { pd::TxMsg m; while (sink.take_tx(m)) sink.on_tx_result(true, now); };
    CHECK_EQ(sink.trigger({1, 5000, 1000}, now), pd::Err::Ok); pump();
    sink.on_message(pd::Sop::Sop, pd::make_header(pd::kCtlAccept, 0, 2) | (1u << 8), {}, now); pump();
    sink.on_message(pd::Sop::Sop, pd::make_header(pd::kCtlPsRdy, 0, 2) | (1u << 8), {}, now); pump();
    const uint8_t req[4] = {0x02, 0x80, 0x00, 0xFF};   // 0xFF008002 little-endian: structured REQ, cmd 2
    sink.on_message(pd::Sop::Sop, pd::make_header(pd::kMsgVendorDefined, 1, 2), req, now); pump();
    for (int i = 0; i < 100; ++i) { now += 10; sink.tick(now); pump(); }
    CHECK_EQ(sink.status().xiaomi_svid, false);
    Fake a; ct::Runner ra(a);
    locked_run(a, ra, sink.status().xiaomi_svid, 0, 5000, false, 5000);   // the list stays at 5 A: the rows end unasked
    CHECK_EQ(hunt_rows(ra).size(), 0);
    for (const auto& s : ra.steps()) CHECK_EQ(s.reason == ct::Reason::NotUnlocked, false);
}
// The K2 itself presented the cable (an armed spoof, the identity's cable VDO limits in Input::spoof_mv / spoof_ma): the cable limit
// the run used is that identity's, not the 3 A of a cable nobody read (CUKTECH C1, spoofed Kowsi 5 A clones, hardware 2026-11-07).
static Fake* spoofed_locked_run(Fake& f, ct::Runner& r, bool svid, uint32_t auth_at, unsigned spoof_ma, unsigned spoof_mv = 20000) {
    f.in.spoof_on = true; f.in.spoof_ma = uint16_t(spoof_ma); f.in.spoof_mv = uint16_t(spoof_mv);
    return locked_run(f, r, svid, auth_at, 0);   // nothing physical was read
}
static void test_hunt_with_a_spoofed_identity_uses_its_cable_limits() {
    // A spoofed 5 A identity, chain finished, 6 A still refused: the cable is not the cause, the charger did not accept the identity.
    Fake a; ct::Runner ra(a); spoofed_locked_run(a, ra, true, 1, 5000);
    expect_not_unlocked(ra, ct::kDetailVendorRefused, "not unlocked (vendor not accepted)");
    // The evidence names the identity's limits as the cable limit used.
    for (const auto& e : ra.hunt_evidence()) { CHECK_EQ(e.cable_ma, 5000); CHECK_EQ(e.flags & ct::HfCableRead, 0); }
    CHECK_EQ(ra.evidence().cable_ma, 5000); CHECK_EQ(ra.evidence().cable_mv, 20000);
    CHECK_EQ(ra.evidence().flags & ct::SpoofOn, ct::SpoofOn);
    // No answer at all reads the same.
    Fake b; ct::Runner rb(b); b.in.spoof_on = true; b.in.spoof_ma = 5000; b.in.spoof_mv = 20000;
    locked_run(b, rb, true, 1, 0, true);
    expect_not_unlocked(rb, ct::kDetailVendorRefused, "not unlocked (vendor not accepted)");
    // The chain links still come first: a spoofed 5 A identity with no chain, or a chain that did not finish.
    Fake c; ct::Runner rc(c); spoofed_locked_run(c, rc, false, 0, 5000);
    expect_not_unlocked(rc, ct::kDetailNoChain, "not unlocked (no OEM chain)");
    Fake d; ct::Runner rd(d); spoofed_locked_run(d, rd, true, 0, 5000);
    expect_not_unlocked(rd, ct::kDetailChainUnfinished, "not unlocked (chain unfinished)");
    // A spoofed 3 A identity really is a 3 A cable.
    Fake e; ct::Runner re(e); spoofed_locked_run(e, re, true, 1, 3000);
    expect_not_unlocked(re, ct::kDetailCableCap, "not unlocked (3 A cable)");
    for (const auto& ev : re.hunt_evidence()) CHECK_EQ(ev.cable_ma, 3000);
    // The voltage limit is the identity's too (a 50 V identity reads 50 V in the evidence).
    Fake g; ct::Runner rg(g); spoofed_locked_run(g, rg, true, 1, 5000, 50000);
    CHECK_EQ(rg.evidence().cable_mv, 50000);
    // A cable the K2 read wins over a spoof's limits (nothing presented it, the spoof cannot be what the source saw).
    Fake h; ct::Runner rh(h); h.in.spoof_on = true; h.in.spoof_ma = 5000; h.in.spoof_mv = 20000;
    locked_run(h, rh, true, 1, 3000);
    expect_not_unlocked(rh, ct::kDetailCableCap, "not unlocked (3 A cable)");
}
static void test_hunt_refused_with_a_proven_cable_and_chain_stays_a_failure() {
    // Chain done and a 5 A cable read: nothing explains the refusal, so it is the charger's refusal and a real FAIL.
    Fake f; ct::Runner r(f); locked_run(f, r, true, 1, 5000);
    const auto h = hunt_rows(r);
    CHECK_EQ(h.size(), 2);
    for (const auto& s : h) { CHECK_EQ(s.verdict, ct::Verdict::Fail); CHECK_EQ(s.reason, ct::Reason::Request); }
}
static void test_hunt_accepted_after_the_chain_passes() {
    Fake f; ct::Runner r(f); xiaomi_run(f, r, true, 1);   // chain done, the source offers the 6 A: both rows pass
    const auto h = hunt_rows(r);
    CHECK_EQ(h.size(), 2);
    for (const auto& s : h) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(s.reason, ct::Reason::None); }
}
// ---- a 6 A hunt row goes out only against a list that advertises 6 A (decision 2026-11-07) ----
// Hardware never produced a grant without an advertised 6 A: Reject, a shorter list or a Hard Reset. The one unlock is CUKTECH's
// SVID 0x2717 chain, after which the source re-announces its PPS APDOs at 6 A. So: (1) the source lists 0x2717: the chain runs and
// the row waits for it; (2) the list in hand advertises 6 A for the row: it asks; (3) otherwise it ends WITHOUT a Request.
static bool any_6a_request(const Fake& f) {
    for (const auto& c : f.choices) if (c.ma >= 6000) return true;
    return false;
}
static void test_hunt_without_the_chain_and_without_6a_sends_no_request_and_leaves_no_row() {   // test 1
    Fake f; ct::Runner r(f); xiaomi_run(f, r, false, 0, 5000);   // no SVID 0x2717, PPS APDOs at 5 A
    CHECK_EQ(any_6a_request(f), false);
    CHECK_EQ(f.hunt_times.size(), 0);
    CHECK_EQ(f.pd_requests > 0, true);   // the ordinary rows did run
    // Nothing to unlock here: the rows leave the plan quietly (no skip on the card, nothing in the counts), the rest passed.
    CHECK_EQ(hunt_rows(r).size(), 0);
    CHECK_EQ(r.hunt_evidence().size(), 0);
    for (const auto& s : r.steps()) CHECK_EQ(s.verdict, ct::Verdict::Pass);
    // The fixed-only source: its 20 V 6 A row (a fixed PDO never offers 6 A) leaves the same way.
    Fake g; g.caps = cuktech_caps(); g.caps.num_pdos = 5; g.caps.pdos[5] = g.caps.pdos[6] = {};
    ct::Runner rg(g); rg.start(g.caps, kPd, g.in, true); g.tick(rg, 200000);
    CHECK_EQ(rg.state(), ct::State::Done);
    CHECK_EQ(any_6a_request(g), false); CHECK_EQ(hunt_rows(rg).size(), 0);
}
static void test_hunt_after_the_chain_with_the_list_still_at_5a_is_a_neutral_skip() {   // test 2
    // Chain listed and done, the APDOs still advertise 5 A, the cable is not known to carry more than 3 A: nothing asked,
    // the row says why ("3 A cable"), no failure and no refusal evidence.
    Fake a; ct::Runner ra(a); locked_run(a, ra, true, 1, 0, false, 5000);
    CHECK_EQ(any_6a_request(a), false); CHECK_EQ(a.hunt_times.size(), 0);
    expect_not_unlocked(ra, ct::kDetailCableCap, "not unlocked (3 A cable)");
    CHECK_EQ(ra.hunt_evidence().size(), 0);
    // A 5 A cable the K2 read: nothing else explains it, the plain "not unlocked" (the card's detail space is full).
    Fake b; ct::Runner rb(b); locked_run(b, rb, true, 1, 5000, false, 5000);
    CHECK_EQ(any_6a_request(b), false);
    expect_not_unlocked(rb, 0, "not unlocked");
    // The chain listed but never finished: the wait is bounded, then the row ends without a Request.
    Fake c; ct::Runner rc(c); locked_run(c, rc, true, 0, 5000, false, 5000);
    CHECK_EQ(any_6a_request(c), false);
    expect_not_unlocked(rc, ct::kDetailChainUnfinished, "not unlocked (chain unfinished)");
    for (const auto& s : rb.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Skip, true);
}
static void test_hunt_after_the_chain_with_the_list_re_announced_at_6a_asks_for_6a() {   // test 3
    Fake base; ct::Runner rb(base); xiaomi_run(base, rb, false, 0, 6000);   // reference: when a row's turn comes
    const uint32_t t0 = base.hunt_times[0];
    // The chain completes 1 s after the first row's turn and the source then announces its PPS APDOs at 6 A.
    Fake f; f.auth_caps_on = true; f.auth_caps = cuktech_caps(6000);
    ct::Runner r(f); xiaomi_run(f, r, true, t0 + 1000, 5000);
    CHECK_EQ(f.hunt_times.size(), 2);
    for (unsigned n = 0; n < f.hunt_apdo_ma.size(); ++n) { CHECK_EQ(f.hunt_apdo_ma[n] >= 6000, true); CHECK_EQ(f.hunt_authed[n], true); }
    const auto h = hunt_rows(r);
    CHECK_EQ(h.size(), 2);
    for (const auto& s : h) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(s.reason, ct::Reason::None); }
    // The re-announced list may arrive a moment after the chain: the row gives it kXiaomiReannounceMs before it ends.
    Fake g; g.auth_caps_on = true; g.auth_caps = cuktech_caps(6000); g.auth_caps_delay_ms = 200;
    ct::Runner rg(g); xiaomi_run(g, rg, true, t0 + 1000, 5000);
    CHECK_EQ(g.hunt_times.size(), 2);
    for (const auto& s : hunt_rows(rg)) CHECK_EQ(s.verdict, ct::Verdict::Pass);
    // Later than that it is too late for the row: it ended without a Request (the list is the one in hand when its turn comes).
    Fake e; e.auth_caps_on = true; e.auth_caps = cuktech_caps(6000); e.auth_caps_delay_ms = ct::kXiaomiReannounceMs + 15000;
    ct::Runner re(e); xiaomi_run(e, re, true, t0 + 1000, 5000);
    CHECK_EQ(e.hunt_times.size(), 0);
    for (const auto& s : hunt_rows(re)) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::NotUnlocked); }
}
// ---- a hunt row answered by a source Hard Reset (Xiaomi 20K, hardware 2026-11-07) ----
// The bank takes the 6 A over-offer as a Hard Reset, re-announces its fixed PDOs alone (the chain has not run again) and brings the
// full list 640 ms later. Cable not read, chain finished before the row.
static Fake* hunt_reset_run(Fake& f, ct::Runner& r, bool svid = true, unsigned short_ms = 640, unsigned dark_ms = 1000,
                            uint32_t auth_at = 1, unsigned cable_seen = 0) {
    f.caps = cuktech_caps(6000); f.xiaomi_svid = svid; f.auth_at = svid ? auth_at : 0;   // 6 A advertised: the rows go out (they would not at 5 A)
    f.hunt_hr = true; f.hr_short_ms = short_ms; f.reset_dark_ms = dark_ms;
    f.in.cable_observed = cable_seen != 0; f.in.cable_ma = cable_seen ? cable_seen : 3000; f.in.cable_mv = 20000;
    r.start(f.caps, kPd, f.in, true);
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    return &f;
}
static void test_preauth_list_after_a_hunt_reset_is_not_offers_changed() {
    Fake f; ct::Runner r(f); hunt_reset_run(f, r);
    CHECK_EQ(f.in.pd.source_hard_resets >= 1, true);
    CHECK_EQ(f.in.pd.hard_resets, 0u);   // the K2 sent none: the source's reset is not its own
    // The truncated list is the source recovering, not another charger: no row is skipped for it.
    for (const auto& s : r.steps()) CHECK_EQ(s.reason == ct::Reason::CapsChanged, false);
    // Every ordinary row (the 20 V PPS APDO the cut-off tail belongs to included) still ran and passed.
    unsigned pps = 0;
    for (const auto& s : r.steps()) {
        if (s.hunt || over5_cable_skip(s)) continue;
        CHECK_EQ(s.verdict, ct::Verdict::Pass);
        pps += s.algorithm == A::Pps && s.pos == 7;
    }
    CHECK_EQ(pps >= 1, true);
}
// The hunt rows' verdicts after a run in which the source answered each 6 A over-offer with a Hard Reset.
static void expect_reset_hunts(const ct::Runner& r, const Fake& f, uint8_t detail, const char* words) {
    const auto h = hunt_rows(r);
    CHECK_EQ(h.size(), 2);
    for (const auto& s : h) {
        CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::NotUnlocked); CHECK_EQ(s.detail, detail);
        CHECK_STR(ct::explanation(s), words);
    }
    CHECK_EQ(f.hunt_times.size(), 2);   // each row's Request went out once: a reset is an answer, not a reason to ask again
    const auto ev = r.hunt_evidence();
    CHECK_EQ(ev.size(), 2);
    for (const auto& e : ev) CHECK_EQ((e.flags & ct::HfHardReset) != 0, true);
    for (const auto& s : r.steps()) {
        if (s.hunt || over5_cable_skip(s)) continue;
        CHECK_EQ(s.verdict, ct::Verdict::Pass);   // the ordinary rows are untouched by the reset
    }
}
static void test_hunt_answered_by_a_hard_reset_reads_not_unlocked() {
    // Xiaomi 20K: chain finished, cable never read (assumed 3 A), the over-offer is answered by a source Hard Reset.
    Fake f; ct::Runner r(f); hunt_reset_run(f, r);
    expect_reset_hunts(r, f, ct::kDetailSourceReset, "not unlocked (source reset)");
    // A cable the K2 read as 5 A with the chain finished: nothing else explains it, same wording.
    Fake g; ct::Runner rg(g); hunt_reset_run(g, rg, true, 640, 1000, 1, 5000);
    expect_reset_hunts(rg, g, ct::kDetailSourceReset, "not unlocked (source reset)");
}
static void test_hunt_hard_reset_keeps_the_chain_and_cable_reasons() {
    Fake a; ct::Runner ra(a); hunt_reset_run(a, ra, false, 0);   // no SVID 0x2717, the list comes back whole
    expect_reset_hunts(ra, a, ct::kDetailNoChain, "not unlocked (no OEM chain)");
    // SVID listed, chain not finished when the first row asked. The reset restarts the chain, which then finishes before the second
    // row: that one says what happened.
    Fake b; ct::Runner rb(b); hunt_reset_run(b, rb, true, 640, 1000, 0);
    const auto hb = hunt_rows(rb);
    CHECK_EQ(hb.size(), 2);
    if (hb.size() == 2) {
        CHECK_EQ(hb[0].verdict, ct::Verdict::Skip); CHECK_EQ(hb[0].reason, ct::Reason::NotUnlocked);
        CHECK_EQ(hb[0].detail, ct::kDetailChainUnfinished); CHECK_STR(ct::explanation(hb[0]), "not unlocked (chain unfinished)");
        CHECK_EQ(hb[1].verdict, ct::Verdict::Skip); CHECK_EQ(hb[1].detail, ct::kDetailSourceReset);
    }
    Fake c; ct::Runner rc(c); hunt_reset_run(c, rc, true, 640, 1000, 1, 3000);   // a 3 A cable the K2 read
    expect_reset_hunts(rc, c, ct::kDetailCableCap, "not unlocked (3 A cable)");
    Fake d; ct::Runner rd(d); d.in.spoof_on = true; d.in.spoof_ma = 5000; d.in.spoof_mv = 20000;   // a spoofed 5 A identity
    hunt_reset_run(d, rd);
    expect_reset_hunts(rd, d, ct::kDetailVendorRefused, "not unlocked (vendor not accepted)");
}
static bool any_caps_changed(const ct::Runner& r) {
    for (const auto& s : r.steps()) if (s.reason == ct::Reason::CapsChanged) return true;
    return false;
}
static unsigned count_reason_all(const ct::Runner& r, ct::Reason why) {
    unsigned n = 0;
    for (const auto& s : r.steps()) n += s.reason == why;
    return n;
}
static bool only_pass_or_skip(const ct::Runner& r) {
    for (const auto& s : r.steps()) if (s.verdict != ct::Verdict::Pass && s.verdict != ct::Verdict::Skip) return false;
    return true;
}
static void test_a_cut_short_list_that_never_grows_is_still_offers_changed() {
    // The tolerance is bounded: a list that stays cut short past kXiaomiAuthWaitMs is another charger (or a dead chain).
    Fake f; ct::Runner r(f); hunt_reset_run(f, r, true, ct::kXiaomiAuthWaitMs + 4000);
    CHECK_EQ(any_caps_changed(r), true);
    // The rows below the cut are untouched.
    for (const auto& s : r.steps()) if (s.pos <= 5) CHECK_EQ(s.verdict, ct::Verdict::Pass);
}
static void test_a_cut_short_list_without_a_xiaomi_chain_is_still_offers_changed() {
    // The pattern belongs to a source that listed SVID 0x2717: any other source that comes back with fewer PDOs changed its offers.
    Fake f; ct::Runner r(f); hunt_reset_run(f, r, false);
    CHECK_EQ(any_caps_changed(r), true);
}// ---- pattern reject-then-reduced-list: the list announced again after the hunt row's own Reject ----
// A CUKTECH-like source without SVID 0x2717 (a Rejected hunt row reads "not unlocked (no OEM chain)") announces `again` reannounce_ms
// after its first Reject. 100 ms is before the runner has looked at the Reject (row Requesting), 600 ms while the row is released.
static void rejected_hunt_run(Fake& f, ct::Runner& r, const pd::SourceCaps& again, uint32_t reannounce_ms) {
    // 6 A advertised: the hunt rows go out and are refused
    f.caps = cuktech_caps(6000); f.reject_hunts = true; f.reannounce_caps = again; f.reannounce_ms = reannounce_ms;
    r.start(f.caps, kPd, f.in, true);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
}
static pd::SourceCaps without_pps(pd::SourceCaps c) { c.num_pdos = 5; c.pdos[5] = c.pdos[6] = {}; return c; }
static void test_hunt_reject_then_the_same_list_again_keeps_the_rows() {
    for (const uint32_t ms : {100u, 600u}) {
        Fake f; ct::Runner r(f); rejected_hunt_run(f, r, cuktech_caps(6000), ms);
        CHECK_EQ(any_caps_changed(r), false);
        unsigned pps = 0;
        for (const auto& s : r.steps()) {
            if (s.hunt) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::NotUnlocked); continue; }
            if (over5_cable_skip(s)) continue;   // the 6 A APDO's full-current row: no cable vouched for above 5 A
            CHECK_EQ(s.verdict, ct::Verdict::Pass);   // the fixed rows and the clamped full-current PPS rows run against the re-announced list
            pps += s.algorithm == A::Pps;
        }
        CHECK_EQ(pps >= 2, true);
        CHECK_EQ(f.hunt_times.size(), 2);
    }
}
static void test_hunt_reject_then_a_list_without_the_pps_apdos_skips_only_those_rows() {
    for (const uint32_t ms : {100u, 600u}) {
        Fake f; ct::Runner r(f); rejected_hunt_run(f, r, without_pps(cuktech_caps(6000)), ms);
        CHECK_EQ(any_caps_changed(r), false);
        const auto h = hunt_rows(r);
        CHECK_EQ(h.size(), 2);
        if (h.size() == 2) {   // the row that was refused: not unlocked; the other one asks for an APDO that is gone
            CHECK_EQ(h[0].verdict, ct::Verdict::Skip); CHECK_EQ(h[0].reason, ct::Reason::NotUnlocked);
            CHECK_EQ(h[1].verdict, ct::Verdict::Skip); CHECK_EQ(h[1].reason, ct::Reason::NoOffer);
            CHECK_STR(ct::explanation(h[1]), "PPS not advertised");
        }
        CHECK_EQ(f.hunt_times.size(), 1);
        unsigned fixed = 0, gone = 0;
        for (const auto& s : r.steps()) {
            if (s.hunt || over5_cable_skip(s)) continue;
            if (s.algorithm == A::Pd3Fixed) { CHECK_EQ(s.verdict, ct::Verdict::Pass); ++fixed; }
            else if (s.verdict == ct::Verdict::Skip) { CHECK_EQ(s.reason, ct::Reason::NoOffer); ++gone; }   // PPS rows the list no longer offers
            else CHECK_EQ(s.verdict, ct::Verdict::Pass);   // a PPS row that ran before the Reject
        }
        CHECK_EQ(fixed, 5);
        CHECK_EQ(gone >= 1, true);
    }
}
static void test_an_unsolicited_list_change_during_a_normal_row_replans_per_row() {
    Fake f; f.caps = cuktech_caps(); ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    CHECK_EQ(run_until(f, r, [&] { return f.pd_requests == 1; }), true);
    f.caps = without_pps(f.caps);   // nobody was Rejected: the source simply re-announced without the APDOs
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    // The row in flight (its own slot still offers it) finished; the APDO rows it no longer offers are withdrawn by the
    // source, not "offers changed"; the unit went on and the run completed.
    CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Pass);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    CHECK_EQ(count_reason_all(r, ct::Reason::Withdrawn) > 0, true);
    CHECK_EQ(only_pass_or_skip(r), true);
    CHECK_EQ(f.replans.empty(), false);   // the re-plan was logged once, with its counts
    // A later unsolicited change is re-planned the same way - unless no pending PD row can be placed at all (here only APDO
    // rows are left and the APDOs are gone): that is still "offers changed".
    // (6 A advertised so a hunt row is sent and Rejected)
    Fake g; g.caps = cuktech_caps(6000); g.reject_hunts = true; ct::Runner rg(g); rg.start(g.caps, kPd, g.in, true);
    CHECK_EQ(run_until(g, rg, [&] { return g.hunt_times.size() == 1 && rg.state() == ct::State::Requesting && !rg.steps()[rg.index()].hunt; }), true);
    g.caps = without_pps(g.caps);
    g.tick(rg, 3000);
    CHECK_EQ(any_caps_changed(rg), true);
}
// ---- hunt evidence: why a refused 6 A row was refused ----
// One refused hunt scenario against a scripted CUKTECH-like sink: `svid`/`auth_at` the chain, `reannounce` the caps after the auth,
// `cable` the cable's current when read (0 = unread), `apdo_ma` the limit of the PPS APDOs in force, `fixed_only` a list with no APDO
// (the fixed 20 V hunt row), `mode` how the source meets the Request (Reject / never answers / the chain guard holds it back).
enum class Meet { Reject, Silent, Held };
struct HuntScen {
    bool svid = true; uint32_t auth_at = 1; bool reannounce = false, in_chain = false, fixed_only = false;
    unsigned cable = 5000, apdo_ma = 6000, last_step = 0x0A; Meet mode = Meet::Reject;   // 6000: only a 6 A list gets a hunt Request
};
static Fake* hunt_scenario(Fake& f, ct::Runner& r, const HuntScen& s) {
    f.caps = cuktech_caps();
    if (s.fixed_only) { f.caps.num_pdos = 5; f.caps.pdos[5] = f.caps.pdos[6] = {}; }
    else for (unsigned n : {5u, 6u}) f.caps.pdos[n] = pd::decode_pdo(0xc0000000 | ((n == 5 ? 110u : 200u) << 17) | (50u << 8) | (s.apdo_ma / 50));
    f.xm_reannounce = s.reannounce; f.xm_in_chain = s.in_chain; f.xm_last_step = uint8_t(s.last_step);
    f.reject_hunts = s.mode == Meet::Reject; f.silent_hunts = s.mode != Meet::Reject; f.hold_hunts = s.mode == Meet::Held;
    f.in.cable_observed = s.cable != 0; f.in.cable_ma = s.cable ? s.cable : 3000; f.in.cable_mv = s.cable ? 50000 : 20000;
    f.xiaomi_svid = s.svid; f.auth_at = s.auth_at;
    r.start(f.caps, kPd, f.in, true);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    return &f;
}
static void expect_cause(const ct::Runner& r, ct::HuntCause cause, size_t rows = 2) {
    const auto ev = r.hunt_evidence();
    CHECK_EQ(ev.size(), rows);
    for (const auto& e : ev) CHECK_EQ(int(e.cause), int(cause));
    for (const auto& e : ev) CHECK_EQ(int(ct::classify_hunt(e)), int(cause));   // the stored cause is the pure classifier's answer
}
static void test_hunt_evidence_names_each_refusal_cause() {
    // The source never listed SVID 0x2717.
    { Fake f; ct::Runner r(f); hunt_scenario(f, r, {.svid = false, .auth_at = 0}); expect_cause(r, ct::HuntCause::NoChain); }
    // Listed, the chain stopped after step 4: the last step seen says where.
    { Fake f; ct::Runner r(f); hunt_scenario(f, r, {.auth_at = 0, .last_step = 4}); expect_cause(r, ct::HuntCause::ChainStalled);
      for (const auto& e : r.hunt_evidence()) { CHECK_EQ(e.last_step, 4); CHECK_EQ(e.flags & ct::HfAuthed, 0); CHECK_EQ(e.authed_ms, 0); } }
    // Our Request left while the chain was running.
    { Fake f; ct::Runner r(f); hunt_scenario(f, r, {.in_chain = true}); expect_cause(r, ct::HuntCause::InsideChain); }
    // The chain guard still held the Request when the row ran out of time: nothing was asked.
    { Fake f; ct::Runner r(f); hunt_scenario(f, r, {.mode = Meet::Held}); expect_cause(r, ct::HuntCause::RequestHeld);
      for (const auto& e : r.hunt_evidence()) { CHECK_EQ(e.apdo, 0); CHECK_EQ(e.flags & ct::HfSent, 0); CHECK_EQ(e.flags & ct::HfHeld, ct::HfHeld); } }
    // Authed, no Source_Capabilities after it, the APDO in force advertises 6 A and the source still refuses: the only cause left is the
    // source. A real FAIL.
    { Fake f; ct::Runner r(f); hunt_scenario(f, r, {}); expect_cause(r, ct::HuntCause::RefusedDespiteOffer);
      for (const auto& e : r.hunt_evidence()) { CHECK_EQ(e.apdo_ma, 6000); CHECK_EQ(e.req_ma, 6000); CHECK_EQ(e.flags & ct::HfReannounced, 0); }
      for (const auto& s : hunt_rows(r)) { CHECK_EQ(s.verdict, ct::Verdict::Fail); CHECK_EQ(s.reason, ct::Reason::Request); } }
    // The same after a re-announced list.
    { Fake f; ct::Runner r(f); hunt_scenario(f, r, {.reannounce = true}); expect_cause(r, ct::HuntCause::RefusedDespiteOffer);
      for (const auto& e : r.hunt_evidence()) CHECK_EQ(e.flags & ct::HfReannounced, ct::HfReannounced);
      for (const auto& s : hunt_rows(r)) CHECK_EQ(s.verdict, ct::Verdict::Fail); }
    // ApdoStale, CableCapped5A, CableCap and FixedPdo describe a Request sent against a list that does not advertise 6 A. Since
    // 2026-11-07 the runner never sends one, so no scenario reaches them (classify_hunt keeps them for the pure-classifier test):
    // a 5 A list (stale, re-announced, unread cable) and a list with no APDO (the fixed 20 V row) send nothing and record nothing.
    for (const HuntScen& sc : {HuntScen{.apdo_ma = 5000}, HuntScen{.reannounce = true, .apdo_ma = 5000},
                               HuntScen{.reannounce = true, .cable = 0, .apdo_ma = 5000}, HuntScen{.reannounce = true, .fixed_only = true}}) {
        Fake f; ct::Runner r(f); hunt_scenario(f, r, sc);
        CHECK_EQ(r.hunt_evidence().size(), 0); CHECK_EQ(f.hunt_times.size(), 0); CHECK_EQ(any_6a_request(f), false);
        for (const auto& s : hunt_rows(r)) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::NotUnlocked); }
    }
}
static void test_hunt_evidence_records_what_the_sink_saw() {
    Fake f; ct::Runner r(f); hunt_scenario(f, r, {.reannounce = true});
    const auto ev = r.hunt_evidence();
    CHECK_EQ(ev.size(), 2);
    if (ev.size() != 2) return;
    // The 11 V row asks APDO 6, the 20 V row APDO 7; both ask 6 A of an APDO that advertises 6 A (the only
    // list a hunt row goes out against, 2026-11-07) and are answered with a Reject 30 ms later.
    CHECK_EQ(ev[0].pos, 6); CHECK_EQ(ev[0].req_mv, 11000); CHECK_EQ(ev[1].pos, 7); CHECK_EQ(ev[1].req_mv, 20000);
    for (const auto& e : ev) {
        CHECK_EQ(e.req_ma, 6000); CHECK_EQ(e.apdo_ma, 6000); CHECK_EQ(e.caps, 7); CHECK_EQ(e.answer, int(pd::ReqAnswer::Reject));
        CHECK_EQ(e.answer_ms, 30); CHECK_EQ(e.last_step, 0x0A); CHECK_EQ(e.type, int(pd::PdoType::Pps));
        CHECK_EQ(e.flags & ~unsigned(ct::HfCableRead), ct::HfSvid | ct::HfOpener | ct::HfAuthed | ct::HfReannounced | ct::HfSent);
        CHECK_EQ(e.cable_ma, 5000); CHECK_EQ(e.authed_ms > 0, true);
        CHECK_EQ(e.apdo, f.caps.pdos[e.pos - 1].raw);
    }
    // Each row ties to its step, and the hook saw the same snapshots once each, in order.
    for (const auto& e : ev) { CHECK_EQ(r.steps()[e.step].hunt, true); CHECK_EQ(r.hunt_evidence_for(e.step), &e); }
    CHECK_EQ(r.hunt_evidence_for(0), nullptr);
    CHECK_EQ(f.evidence_seen.size(), 2);
    if (f.evidence_seen.size() == 2) { CHECK_EQ(f.evidence_seen[0].step, ev[0].step); CHECK_EQ(f.evidence_seen[1].step, ev[1].step); }
}
static void test_hunt_evidence_marks_a_silent_source_as_a_timeout() {
    Fake f; ct::Runner r(f); hunt_scenario(f, r, {.reannounce = true, .mode = Meet::Silent});
    expect_cause(r, ct::HuntCause::RefusedDespiteOffer);   // 6 A advertised (the only list a row is sent against): the silence is the source's
    for (const auto& e : r.hunt_evidence()) {
        CHECK_EQ(e.answer, int(pd::ReqAnswer::None)); CHECK_EQ(e.answer_ms, 0);
        CHECK_EQ(e.flags & ct::HfTimeout, ct::HfTimeout); CHECK_EQ(e.flags & ct::HfSent, ct::HfSent);
    }
}
static void test_hunt_evidence_without_a_trace_is_unexplained() {
    // A chain that finished but no sink trace to say more: the refusal is recorded and says so, never guessed.
    Fake f; ct::Runner r(f); f.in.xm = nullptr; hunt_scenario(f, r, {.reannounce = true});
    expect_cause(r, ct::HuntCause::Unexplained);
    for (const auto& e : r.hunt_evidence()) { CHECK_EQ(e.apdo, 0); CHECK_EQ(e.flags & ct::HfSent, 0); }
    // The chain half still comes from the sink status: a source with no SVID is a NoChain refusal with or without a trace.
    Fake g; ct::Runner s(g); g.in.xm = nullptr; hunt_scenario(g, s, {.svid = false, .auth_at = 0});
    expect_cause(s, ct::HuntCause::NoChain);
}
static void test_hunt_evidence_only_for_refused_rows() {
    // A source that offers the 6 A: both rows pass, nothing is recorded and the hook never fires.
    Fake f; ct::Runner r(f); xiaomi_run(f, r, true, 1);
    CHECK_EQ(r.hunt_evidence().size(), 0); CHECK_EQ(f.evidence_seen.size(), 0);
    // A new run starts with an empty table.
    Fake g; ct::Runner s(g); hunt_scenario(g, s, {});
    CHECK_EQ(s.hunt_evidence().size(), 2);
    s.start(g.caps, kPd, g.in, true);
    CHECK_EQ(s.hunt_evidence().size(), 0);
}
static void test_classify_hunt_precedence() {
    // The order of the cause table: each cause beats every one below it.
    ct::HuntEvidence e; e.flags = ct::HfSvid | ct::HfAuthed | ct::HfReannounced | ct::HfSent; e.type = uint8_t(pd::PdoType::Pps);
    e.req_ma = 6000; e.apdo_ma = 5000; e.cable_ma = 5000; e.apdo = 1;
    CHECK_EQ(int(ct::classify_hunt(e)), int(ct::HuntCause::CableCapped5A));
    ct::HuntEvidence f = e; f.apdo_ma = 6000;
    CHECK_EQ(int(ct::classify_hunt(f)), int(ct::HuntCause::RefusedDespiteOffer));
    f = e; f.cable_ma = 3000;
    CHECK_EQ(int(ct::classify_hunt(f)), int(ct::HuntCause::CableCap));
    f = e; f.flags &= ~ct::HfReannounced; f.cable_ma = 3000;
    CHECK_EQ(int(ct::classify_hunt(f)), int(ct::HuntCause::ApdoStale));   // the stale list is the sharper cause than the unread cable
    f = e; f.type = uint8_t(pd::PdoType::Fixed); f.apdo_ma = 6000;
    CHECK_EQ(int(ct::classify_hunt(f)), int(ct::HuntCause::FixedPdo));    // a fixed PDO is refused whatever it advertises
    f = e; f.flags |= ct::HfInChain;
    CHECK_EQ(int(ct::classify_hunt(f)), int(ct::HuntCause::InsideChain));
    f.flags |= ct::HfHeld; f.flags &= ~ct::HfSent;
    CHECK_EQ(int(ct::classify_hunt(f)), int(ct::HuntCause::RequestHeld));
    f = e; f.flags &= ~ct::HfAuthed;
    CHECK_EQ(int(ct::classify_hunt(f)), int(ct::HuntCause::ChainStalled));
    f.flags &= ~ct::HfSvid;
    CHECK_EQ(int(ct::classify_hunt(f)), int(ct::HuntCause::NoChain));
    f = e; f.apdo = 0; f.flags &= ~ct::HfSent;
    CHECK_EQ(int(ct::classify_hunt(f)), int(ct::HuntCause::Unexplained));
    // Every cause has a name and wording; the wording fits the detail page.
    for (unsigned n = 0; n < unsigned(ct::HuntCause::Count); ++n) {
        const auto c = ct::HuntCause(n);
        CHECK_EQ(std::string(ct::name(c)).empty(), false); CHECK_EQ(std::string(ct::explanation(c)).size() <= 26, true);
        for (unsigned m = 0; m < n; ++m) CHECK_EQ(std::string(ct::name(c)) == ct::name(ct::HuntCause(m)), false);
    }
}
static void test_unread_cable_limit_follows_the_source_offers() {
    // A far-end sink never reads the cable. A source that offers more than 3 A has seen a 5 A e-marker (USB PD caps a 3 A cable at
    // 3 A), so the run's cable current limit is 5 A, not the 3 A default; the voltage default stays 20 V.
    Fake f; f.caps = cuktech_caps(); f.in.cable_observed = false; f.in.cable_mv = 20000; f.in.cable_ma = 3000;
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    CHECK_EQ(r.evidence().flags & ct::PhysicalCable, 0);
    CHECK_EQ(r.evidence().cable_mv, 20000); CHECK_EQ(r.evidence().cable_ma, 5000);
    // 3 A offers prove nothing: the default stays.
    Fake g; g.in.cable_observed = false; g.in.cable_mv = 20000; g.in.cable_ma = 3000;
    for (auto& p : g.caps.pdos) if (p.raw && p.type == pd::PdoType::Fixed) p = pd::decode_pdo(((p.max_mv / 50) << 10) | 300);
    g.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (50u << 8) | 60); g.caps.pdos[6] = pd::decode_pdo(0xc0000000 | (210u << 17) | (50u << 8) | 60);
    ct::Runner s(g); s.start(g.caps, kPd, g.in, true);
    CHECK_EQ(s.evidence().cable_ma, 3000);
    // A cable that was read keeps its own limit.
    Fake h; h.caps = cuktech_caps(); h.in.cable_observed = true; h.in.cable_mv = 20000; h.in.cable_ma = 3000;
    ct::Runner t(h); t.start(h.caps, kPd, h.in, true);
    CHECK_EQ(t.evidence().cable_ma, 3000);
}
static void test_refusals() {
    Fake f; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, {}, f.in, false), ct::Reason::NeedConfirm);
    f.in.inline_load = true; CHECK_EQ(r.start(f.caps, {}, f.in, true), ct::Reason::Inline);
    f.in.inline_load = false; f.in.fake = true; CHECK_EQ(r.start(f.caps, {}, f.in, true), ct::Reason::Fake);
    f.in.fake = false; f.in.vbus_mv = -1; CHECK_EQ(r.start(f.caps, {}, f.in, true), ct::Reason::NoMonitor);
    f.in.vbus_mv = 0; CHECK_EQ(r.start(f.caps, {}, f.in, true), ct::Reason::NoSource);
    f.in.vbus_mv = 5000; f.in.busy = true; CHECK_EQ(r.start(f.caps, {}, f.in, true), ct::Reason::Busy);
    CHECK_EQ(f.pd_requests + f.fc_requests + f.releases, 0);
}
// The page arms the journal (the whole plan is written in pieces) before the first row: hold(true) keeps the run in its Baseline,
// nothing is requested and no phase marker written, and the 500 ms of vSafe5V is counted from the release, not from the start.
static void test_hold_keeps_the_baseline_until_released() {
    Fake f; f.in.vbus_mv = 5100;
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    r.hold(true);
    f.tick(r, 20000);                                    // far longer than any Baseline
    CHECK_EQ(r.state(), ct::State::Baseline);
    CHECK_EQ(f.pd_requests + f.fc_requests + f.releases, 0);
    CHECK_EQ(f.phases.size(), 0u);
    r.hold(false);
    f.tick(r, 300);
    CHECK_EQ(r.state(), ct::State::Baseline);            // the clock starts at the release
    CHECK_EQ(run_until(f, r, [&] { return f.pd_requests == 1; }), true);
    // A cancel while held ends the run as it always does (the release runs, then the run is over).
    Fake g; g.in.vbus_mv = 5100;
    ct::Runner c(g);
    CHECK_EQ(c.start(g.caps, kPd, g.in, true), ct::Reason::None);
    c.hold(true); g.tick(c, 1000);
    c.cancel(g.in);
    g.tick(c, 20000);
    CHECK_EQ(c.busy(), false);
}
static void test_safe5_baseline() {
    for (int mv : {4750, 5269, 5500}) {
        Fake f; f.in.vbus_mv = mv;
        const auto preview = ct::Runner::preview(f.caps, kPd, f.in);
        CHECK_EQ(preview.refusal, ct::Reason::None);
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
        CHECK_EQ(r.state(), ct::State::Baseline);
        CHECK_EQ(run_until(f, r, [&] { return f.pd_requests == 1; }), true);   // every unit starts with its own detach and attach
    }
    for (int mv : {4749, 5501}) {
        Fake f; f.in.vbus_mv = mv;
        CHECK_EQ(ct::Runner::preview(f.caps, kPd, f.in).refusal, ct::Reason::Release);
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::Release);
        CHECK_EQ(r.state(), ct::State::Unsafe);
        CHECK_EQ(f.pd_requests + f.fc_requests, 0);
    }
    Fake fixed;
    fixed.in.vbus_mv = 5269;
    ct::Runner r(fixed);
    CHECK_EQ(r.start(fixed.caps, kPd, fixed.in, true), ct::Reason::None);
    CHECK_EQ(run_until(fixed, r, [&] { return fixed.pd_requests == 1; }), true);
    fixed.in.vbus_mv = 5269;
    fixed.tick(r, 1700);
    CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Pass);
    CHECK_EQ(r.steps()[0].measured_mv, 5269);
}
// ms from start until the first request goes out, ticking in 100 ms steps.
static uint32_t first_request_ms(Fake& f, ct::Runner& r) {
    const uint32_t t0 = f.in.now;
    for (unsigned n = 0; n < 100 && f.pd_requests == 0; ++n) f.tick(r, 100);
    return f.in.now - t0;
}
static void test_source_probe_costs_at_most_a_second() {
    // The probe runs after the last row, back at a settled 5 V contract: no row waits for it.
    Fake base; base.in.pd.state = pd::SinkState::Ready; ct::Runner rb(base);
    CHECK_EQ(rb.start(base.caps, kPd, base.in, true), ct::Reason::None);
    const uint32_t plain = first_request_ms(base, rb);
    base.tick(rb, 60000);
    CHECK_EQ(rb.state(), ct::State::Done); CHECK_EQ(base.probe_starts, 1); CHECK_EQ(base.probe_stops, 0);   // no probe available

    for (int mode : {1, 2}) {
        Fake f; f.in.pd.state = pd::SinkState::Ready; f.probe_mode = mode; ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
        CHECK_EQ(first_request_ms(f, r), plain);                 // the first row is not delayed
        f.tick(r, 60000);
        CHECK_EQ(r.state(), ct::State::Done);                    // finishes normally, probe answered or capped
        CHECK_EQ(f.probe_starts, 1); CHECK_EQ(f.probe_stops, 1);
        CHECK_EQ(f.in.pd.hard_resets + f.in.pd.source_hard_resets, 0u);
        for (const auto& st : r.steps()) { CHECK_EQ(st.reason == ct::Reason::HardReset, false); CHECK_EQ(st.verdict == ct::Verdict::Fail, false); }
    }
}
static void test_source_probe_only_in_a_pd_contract_and_once() {
    Fake f; f.probe_mode = 1; f.caps.active_cc = 0; f.in.pd_traffic = false;   // no PD contract: nothing to ask
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, {.selected = ct::bit(A::Qc2)}, f.in, true), ct::Reason::None);
    f.tick(r, 30000);
    CHECK_EQ(f.probe_starts, 0);

    Fake g; g.in.pd.state = pd::SinkState::Ready; g.probe_mode = 2; ct::Runner rg(g);
    CHECK_EQ(rg.start(g.caps, kPd, g.in, true), ct::Reason::None);
    for (unsigned n = 0; n < 4000 && g.probe_starts == 0; ++n) g.tick(rg, 100);
    CHECK_EQ(g.probe_starts, 1);                                // started only after the last row
    for (const auto& st : rg.steps()) CHECK_EQ(st.verdict, ct::Verdict::Pass);
    rg.cancel(g.in);                                            // cancel while probing stops the probe
    g.tick(rg, 3000);
    CHECK_EQ(g.probe_stops >= 1, true);
}
// A source that never gives a contract back at the end: the probe is skipped after the bound and the run is Done.
static void test_source_probe_skipped_without_a_contract() {
    Fake f; f.probe_mode = 1;   // pd.state stays Idle
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    f.tick(r, 60000);
    CHECK_EQ(r.state(), ct::State::Done);
}
static void test_cancel_and_failed_release() {
    Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    for (unsigned n = 0; n < 150 && f.pd_requests < 2; ++n) f.tick(r, 100);
    CHECK_EQ(f.pd_requests, 2);
    CHECK_EQ(f.in.vbus_mv, 9000);
    f.release_ok = false; r.cancel(f.in); f.tick(r, 9000);
    CHECK_EQ(r.state(), ct::State::Unsafe);
    read_only(r, f);
    CHECK_EQ(r.busy(), true);
    CHECK_EQ(r.start(f.caps, {}, f.in, true), ct::Reason::Busy);
    const int requests = f.pd_requests; f.tick(r, 30000); CHECK_EQ(f.pd_requests, requests);
    f.in.vbus_mv = 5269; f.in.pd.held = false; f.in.pd.state = pd::SinkState::Ready; f.in.pd.mv = 5000;
    f.tick(r, 700); CHECK_EQ(r.state(), ct::State::Stopped);
    read_only(r, f);
}
// Pattern budget-reannounce-mid-run (Anker 2-way splitter): the sink verifies the 5 V release against VBUS, retries, and as the
// last step sends a Hard Reset of its own. While it verifies the release window stays open (no Unsafe after 8 s); its own Hard Reset
// is a recovery (VBUS dips and returns), never "source lost".
static void test_release_waits_for_the_sink_verification_and_its_hard_reset() {
    auto at_9v = [](Fake& f, ct::Runner& r) {
        r.start(f.caps, kPd, f.in, true);
        for (unsigned n = 0; n < 150 && f.pd_requests < 2; ++n) f.tick(r, 100);
        CHECK_EQ(f.in.vbus_mv, 9000);
        f.release_ok = false;   // the fake source does not follow the release: VBUS stays up
        f.in.pd.verifying = true; f.in.pd.held = false; f.in.pd.state = pd::SinkState::Ready; f.in.pd.mv = 5000;
        r.cancel(f.in);
    };
    {   // verification running: the window stays open far past 8 s, then the sink's Hard Reset brings the port to 5 V
        Fake f; ct::Runner r(f); at_9v(f, r);
        f.tick(r, 20000);
        CHECK_EQ(r.state(), ct::State::Releasing);
        ++f.in.pd.hard_resets; f.in.pd.verifying = false; f.in.pd.state = pd::SinkState::Failed;
        f.in.vbus_mv = 0; f.tick(r, 500);
        CHECK_EQ(r.state() != ct::State::Unsafe, true);
        f.in.vbus_mv = 5000; f.tick(r, 3000);
        CHECK_EQ(r.state(), ct::State::Stopped);
        CHECK_EQ(r.reason() != ct::Reason::Lost, true);
    }
    {   // verification over without a result (no Hard Reset allowed): the old 8 s window then ends it as Unsafe
        Fake f; ct::Runner r(f); at_9v(f, r);
        f.tick(r, 20000); CHECK_EQ(r.state(), ct::State::Releasing);
        f.in.pd.verifying = false; f.in.pd.state = pd::SinkState::Failed;
        f.tick(r, 1000);
        CHECK_EQ(r.state(), ct::State::Unsafe);
    }
}
// Unsafe is acknowledged by the unplug: VBUS absent (under 1 V) for kUnsafeGoneMs ends it (Stopped, a new run may start). VBUS present
// and outside vSafe5V never does, and a dark blip shorter than the debounce does not either (GaN3 65W, 2026-11-08: unplug and replug
// left "unsafe" until a reboot).
static void test_unsafe_clears_when_vbus_is_gone_for_the_debounce() {
    auto unsafe_at_9v = [](Fake& f, ct::Runner& r) {
        r.start(f.caps, kPd, f.in, true);
        for (unsigned n = 0; n < 150 && f.pd_requests < 2; ++n) f.tick(r, 100);
        f.release_ok = false; r.cancel(f.in); f.tick(r, 9000);
        CHECK_EQ(r.state(), ct::State::Unsafe);
    };
    {   // unplugged: 0 V for the debounce
        Fake f; ct::Runner r(f); unsafe_at_9v(f, r);
        f.in.vbus_mv = 0;
        f.tick(r, ct::kUnsafeGoneMs - 300); CHECK_EQ(r.state(), ct::State::Unsafe);
        f.tick(r, 600);
        CHECK_EQ(r.state(), ct::State::Stopped);
        CHECK_EQ(r.busy(), false);
        f.in.vbus_mv = 5000; f.in.pd.held = false; f.in.pd.state = pd::SinkState::Ready; f.in.pd.mv = 5000;
        CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    }
    {   // a blip of darkness shorter than the debounce, then 7 V: still unsafe
        Fake f; ct::Runner r(f); unsafe_at_9v(f, r);
        f.in.vbus_mv = 0; f.tick(r, ct::kUnsafeGoneMs - 400);
        f.in.vbus_mv = 7000; f.tick(r, 5000);
        CHECK_EQ(r.state(), ct::State::Unsafe);
        f.in.vbus_mv = 0; f.tick(r, ct::kUnsafeGoneMs - 400);   // the debounce restarts: the earlier dark time does not add up
        CHECK_EQ(r.state(), ct::State::Unsafe);
    }
    {   // present and outside vSafe5V: stays
        Fake f; ct::Runner r(f); unsafe_at_9v(f, r);
        f.in.vbus_mv = 7000; f.tick(r, 20000);
        CHECK_EQ(r.state(), ct::State::Unsafe);
        CHECK_EQ(r.busy(), true);
    }
}
// Unsafe protects the user from a source left above vSafe5V. A failed 5 V release with VBUS under vSafe5V (REMAX 10K USB-A,
// 2026-11-08: the port drops to a 2.05 V standby after the reset) holds no elevated voltage: it ends as a lost source, not as
// "unplug; verify measured 5 V" forever (the unplug clear only looked for VBUS under 1 V).
static void test_failed_release_at_low_vbus_is_a_lost_source_not_unsafe() {
    for (int mv : {2050, 1200, 4749}) {
        Fake f; ct::Runner r(f);
        r.start(f.caps, kPd, f.in, true);
        for (unsigned n = 0; n < 150 && f.pd_requests < 2; ++n) f.tick(r, 100);
        f.release_ok = false; r.cancel(f.in);
        f.in.vbus_mv = mv; f.in.pd.state = pd::SinkState::Idle; f.in.pd.held = false;
        f.tick(r, 9000);
        CHECK_EQ(r.state() == ct::State::Unsafe, false);
        CHECK_EQ(r.state(), ct::State::Stopped);
        CHECK_EQ(r.busy(), false);
    }
    for (int mv : {5501, 7000, 9000}) {   // stuck above vSafe5V: still unsafe
        Fake f; ct::Runner r(f);
        r.start(f.caps, kPd, f.in, true);
        for (unsigned n = 0; n < 150 && f.pd_requests < 2; ++n) f.tick(r, 100);
        f.release_ok = false; r.cancel(f.in);
        f.in.vbus_mv = mv;
        f.tick(r, 9000);
        CHECK_EQ(r.state(), ct::State::Unsafe);
    }
}
// Unsafe entered with the source high, then the source falls to a standby level under vSafe5V and stays there: the unplug
// clear accepts any VBUS under vSafe5V, not only a dark port.
static void test_unsafe_clears_when_vbus_stays_under_vsafe5() {
    auto unsafe_at_9v = [](Fake& f, ct::Runner& r) {
        r.start(f.caps, kPd, f.in, true);
        for (unsigned n = 0; n < 150 && f.pd_requests < 2; ++n) f.tick(r, 100);
        f.release_ok = false; r.cancel(f.in); f.tick(r, 9000);
        CHECK_EQ(r.state(), ct::State::Unsafe);
    };
    {
        Fake f; ct::Runner r(f); unsafe_at_9v(f, r);
        f.in.vbus_mv = 2050;
        f.tick(r, ct::kUnsafeGoneMs - 300); CHECK_EQ(r.state(), ct::State::Unsafe);
        f.tick(r, 600);
        CHECK_EQ(r.state(), ct::State::Stopped);
    }
    {   // bouncing back above the low end restarts the debounce
        Fake f; ct::Runner r(f); unsafe_at_9v(f, r);
        f.in.vbus_mv = 2050; f.tick(r, ct::kUnsafeGoneMs - 400);
        f.in.vbus_mv = 9000; f.tick(r, 2000);
        f.in.vbus_mv = 2050; f.tick(r, ct::kUnsafeGoneMs - 400);
        CHECK_EQ(r.state(), ct::State::Unsafe);
    }
}
static void test_faults() {
    for (unsigned mode = 0; mode < 5; ++mode) {   // the 5 V legacy rows pass on one reading and have no hold
        Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
        run_until(f, r, [&] { return f.pd_requests >= 1; }); f.tick(r, 600);
        if (mode == 0) f.in.inline_load = true;
        if (mode == 1) ++f.in.session;
        if (mode == 2) f.in.vbus_mv = -1;
        if (mode == 3) ++f.in.pd.source_hard_resets;
        if (mode == 4) --f.caps.pdos[0].raw;   // current limit dropped on the row's own PDO
        if (mode == 5) f.in.vbus_mv = 5501;
        f.tick(r, 100);
        // A source loss or hard reset mid-row costs one retry of the row (see the hard-reset tests); everything else fails it.
        // mode 4 is the exception (user decision 2026-11-09): a fixed offer whose current drops keeps its row - the row in
        // flight runs on at the new, lower current word, not withdrawn (only a voltage no longer offered is), not failed.
        CHECK_EQ(r.steps()[0].verdict,
                 (mode == 1 || mode == 3) ? ct::Verdict::Pending : mode == 4 ? ct::Verdict::Running : ct::Verdict::Fail);
        if (mode == 4) {
            CHECK_EQ(int(r.steps()[0].reason), int(ct::Reason::None));
            CHECK_EQ(pd::decode_pdo(r.steps()[0].pdo).max_ma, 3240u);   // the word was refreshed to the lower current
        }
        CHECK_EQ(f.releases > 0, mode != 1 && mode != 3 && mode != 4);
    }
}
// The HID-cable loop 5 V lock (k2/core/loop_guard.hpp): a run never starts (ahead of the confirm, so the screen names the lock
// first), and a lock that appears mid-run fails the row in flight, hands the contract back and ends the run with the reason.
static void test_loop_lock_refuses_and_aborts() {
    {
        Fake f; ct::Runner r(f);
        f.in.loop_lock = true;
        CHECK_EQ(r.start(f.caps, kPd, f.in, false), ct::Reason::Loop);
        CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::Loop);
        CHECK_EQ(ct::Runner::preview(f.caps, kPd, f.in).refusal, ct::Reason::Loop);
        CHECK_EQ(f.pd_requests + f.fc_requests + f.releases, 0);
        CHECK_STR(ct::name(ct::Reason::Loop), "HID-cable loop: 5V lock");
        CHECK_STR(ct::refusal_words(ct::Reason::Loop), "HID-cable loop: 5V lock");
    }
    {   // mid-row
        Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
        run_until(f, r, [&] { return f.pd_requests >= 1; }); f.tick(r, 600);
        const int requests = f.pd_requests;
        f.in.loop_lock = true;
        f.tick(r, 100);
        CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Fail);
        CHECK_EQ(int(r.steps()[0].reason), int(ct::Reason::Loop));
        CHECK_EQ(f.releases > 0, true);
        f.tick(r, 20000);
        CHECK_EQ(r.busy(), false);
        CHECK_EQ(int(r.reason()), int(ct::Reason::Loop));
        CHECK_EQ(f.pd_requests, requests);   // no row after the lock
    }
    {   // in the Baseline (5 V, before the first row): the run ends without a row
        Fake f; f.in.vbus_mv = 5100; ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
        r.hold(true); f.tick(r, 200);
        f.in.loop_lock = true;
        r.hold(false); f.tick(r, 20000);
        CHECK_EQ(r.busy(), false);
        CHECK_EQ(int(r.reason()), int(ct::Reason::Loop));
        CHECK_EQ(f.pd_requests + f.fc_requests, 0);
    }
}
static void test_timeouts_and_skips() {
    Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    run_until(f, r, [&] { return f.pd_requests >= 1; });
    f.in.pd.state = pd::SinkState::Requesting; f.tick(r, 12500);
    CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Fail); CHECK_EQ(r.steps()[0].reason, ct::Reason::Timeout);
    Fake limited; limited.in.cap_mv = 15000; ct::Runner lr(limited);
    lr.start(limited.caps, kPd, limited.in, true);
    bool skipped20 = false, cappedPps = false;
    for (const auto& s : lr.steps()) {
        if (s.pos == 5) skipped20 = s.verdict == ct::Verdict::Skip && s.reason == ct::Reason::OverCap;
        if (s.pos == 7 && s.mv == 15000) cappedPps = true;
        if (s.verdict == ct::Verdict::Pending) CHECK_EQ(s.mv <= 15000, true);
    }
    CHECK_EQ(skipped20, true); CHECK_EQ(cappedPps, true);
}
static void test_one_second_holds() {
    for (unsigned mode = 0; mode < 6; ++mode) {
        Fake f;
        A algorithm = A::Pd3Fixed;
        if (mode == 0) f.caps.num_pdos = 1;
        if (mode == 1) algorithm = A::Pps;
        if (mode == 2 || mode == 3) {
            f.caps.epr = true; f.caps.num_pdos = 8;
            f.caps.pdos[7] = mode == 2 ? pd::decode_pdo((560u << 10) | 300) :
                pd::decode_pdo(0xD0000000 | (480u << 17) | (150u << 8) | 140);
            algorithm = mode == 2 ? A::Epr : A::EprAvs;
        }
        if (mode >= 4) {
            f.caps = {};
            algorithm = A::Afc;
        }
        ct::Runner r(f);
        const ct::Options options{.selected = ct::bit(algorithm)};
        const auto preview = ct::Runner::preview(f.caps, options, f.in);
        CHECK_EQ(preview.estimate_ms, 500 + preview.runnable * 2000 + 4000);   // plus one detach and attach for the unit
        CHECK_EQ(r.start(f.caps, options, f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return r.steps()[0].verdict == ct::Verdict::Running; }), true);
        f.tick(r, 1000);
        CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Running);
        f.in.now += 99; r.poll(f.in, f.caps);
        CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Running);
        ++f.in.now; r.poll(f.in, f.caps);
        CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Pass);
    }
}
static void test_qc2_settles_within_two_percent() {
    Fake f; f.caps = {};
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Qc2)}, f.in, true);
    for (unsigned n = 0; n < 150 && f.fc_requests < 2; ++n) f.tick(r, 100);   // the 5 V row asks the engine first, then the 9 V row
    CHECK_EQ(f.fc_requests, 2);
    CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Pass);
    CHECK_EQ(r.steps()[1].mv, 9000);
    f.in.fc_state = fc::State::Negotiating;
    f.tick(r, 1000);
    CHECK_EQ(r.state(), ct::State::Requesting);
    f.in.fc_state = fc::State::Active;
    f.in.vbus_mv = 8819; f.tick(r, 700);
    CHECK_EQ(r.state(), ct::State::Requesting);
    f.in.vbus_mv = 9271; f.tick(r, 700);
    CHECK_EQ(r.state(), ct::State::Requesting);
    f.in.vbus_mv = 8820; f.tick(r, 500);
    f.in.vbus_mv = 8819; f.tick(r, 100);
    f.in.vbus_mv = 9180; f.tick(r, 100);
    f.tick(r, 900);
    CHECK_EQ(r.steps()[1].verdict, ct::Verdict::Running);
    f.tick(r, 100);
    CHECK_EQ(r.steps()[1].verdict, ct::Verdict::Pass);

    Fake slow; slow.caps = {};
    ct::Runner sr(slow);
    sr.start(slow.caps, {.selected = ct::bit(A::Qc2)}, slow.in, true);
    run_until(slow, sr, [&] { return slow.fc_requests >= 2; });
    slow.in.fc_state = fc::State::Negotiating;
    slow.tick(sr, 12500);
    CHECK_EQ(sr.state(), ct::State::Requesting);
    slow.in.fc_state = fc::State::Active;
    slow.tick(sr, 1100);
    CHECK_EQ(sr.steps()[1].verdict, ct::Verdict::Pass);

    Fake stuck; stuck.caps = {};
    ct::Runner tr(stuck);
    tr.start(stuck.caps, {.selected = ct::bit(A::Qc2)}, stuck.in, true);
    run_until(stuck, tr, [&] { return stuck.fc_requests >= 2; });
    stuck.in.fc_state = fc::State::Negotiating;
    stuck.tick(tr, 14900);
    CHECK_EQ(tr.state(), ct::State::Requesting);
    stuck.tick(tr, 100);
    CHECK_EQ(tr.steps()[1].verdict, ct::Verdict::Fail);
    CHECK_EQ(tr.steps()[1].reason, ct::Reason::Timeout);
}
// A source at 5.12 V (CJ21 on a real cable, 2.4 % over) is a valid 5 V QC2 baseline; the 2 % band is for 9 V and up.
static void test_qc2_5v_accepts_vsafe5_reading() {
    Fake f; f.caps = {}; f.fc_5v_mv = 5120;
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Qc2)}, f.in, true);
    CHECK_EQ(run_until(f, r, [&] { return r.steps()[0].verdict == ct::Verdict::Pass; }), true);
    CHECK_EQ(r.steps()[0].mv, 5000);
    CHECK_EQ(r.steps()[0].measured_mv, 5120);
}

// A 5 V step of any legacy group passes on a vSafe5V reading (Baseus idles at 5.19 V, 3.8 % off 5 V).
static void test_legacy_5v_accepts_vsafe5_reading() {
    for (auto a : {A::Qc3, A::Qc3Plus}) {
        Fake f; f.caps = {}; f.fc_5v_mv = 5190;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        CHECK_EQ(run_until(f, r, [&] { return r.steps()[0].verdict == ct::Verdict::Pass; }), true);
        CHECK_EQ(r.steps()[0].mv, 5000);
        CHECK_EQ(r.steps()[0].measured_mv, 5190);
    }
}
// The QC3 rows pass in an asymmetric band: 300 mV below the level, 150 mV above (the loop aims below, an overshoot is the risk).
static void test_qc3_rows_use_an_asymmetric_band() {
    struct Case { int offset; bool pass; };
    for (auto a : {A::Qc3, A::Qc3Plus}) {
        for (const Case c : {Case{140, true}, Case{-280, true}, Case{-300, true}, Case{160, false}, Case{-320, false}, Case{250, false}}) {
            Fake f; f.caps = {}; f.fc_offset_mv = c.offset;
            ct::Runner r(f);
            r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
            size_t row = 0;
            for (size_t n = 0; n < r.steps().size(); ++n) if (r.steps()[n].mv == 9000) { row = n; break; }
            CHECK_EQ(run_until(f, r, [&] { return r.steps()[row].verdict == ct::Verdict::Pass || r.steps()[row].verdict == ct::Verdict::Fail; }, 60000), true);
            CHECK_EQ(r.steps()[row].verdict == ct::Verdict::Pass, c.pass);
        }
    }
}
// The AFC / FCP / UFCS / SCP rows above 5 V pass within 5 % of the level (150 mV at least), not 85..110 %: a 9 V row that settles
// at 8.0 V or 9.8 V is a miss (docs/CHARGER_QUIRKS.md, pattern legacy-row-wide-band).
static void test_legacy_rows_use_a_five_percent_band() {
    struct Case { int offset; bool pass; };
    for (auto a : {A::Afc, A::Fcp, A::Ufcs}) {
        for (const Case c : {Case{430, true}, Case{-430, true}, Case{470, false}, Case{-470, false}, Case{-1000, false}, Case{800, false}}) {
            Fake f; f.caps = {}; f.fc_offset_mv = c.offset;
            ct::Runner r(f);
            r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
            size_t row = 0;
            for (size_t n = 0; n < r.steps().size(); ++n) if (r.steps()[n].mv == 9000) { row = n; break; }
            CHECK_EQ(r.steps()[row].mv, 9000);
            CHECK_EQ(run_until(f, r, [&] { return r.steps()[row].verdict == ct::Verdict::Pass || r.steps()[row].verdict == ct::Verdict::Fail; }, 60000), true);
            CHECK_EQ(r.steps()[row].verdict == ct::Verdict::Pass, c.pass);
            if (!c.pass) CHECK_EQ(r.steps()[row].reason, ct::Reason::Timeout);   // never inside the band: the row times out
        }
    }
}
// A 5 V row of those groups keeps the vSafe5V window (a 5 V charger idles at 5.1-5.2 V) but no longer passes at 4.5 V.
static void test_legacy_5v_row_uses_the_vsafe5_window() {
    struct Case { int mv; bool pass; };
    for (auto a : {A::Fcp, A::Scp, A::Ufcs}) {
        for (const Case c : {Case{5195, true}, Case{4800, true}, Case{4600, false}, Case{5600, false}}) {
            Fake f; f.caps = {}; f.fc_5v_mv = c.mv;
            ct::Runner r(f);
            r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
            CHECK_EQ(r.steps()[0].mv, 5000);
            CHECK_EQ(run_until(f, r, [&] { return r.steps()[0].verdict == ct::Verdict::Pass || r.steps()[0].verdict == ct::Verdict::Fail; }, 60000), true);
            CHECK_EQ(r.steps()[0].verdict == ct::Verdict::Pass, c.pass);
        }
    }
}
// The SCP 4.5 V row sits below the 5 V the session starts from: an unloaded output cannot sink down, so the band is 5 % below and
// the unloaded ceiling above (target + max(10 %, 600 mV), the programmable rule). Its proof is the adapter's own readback, which
// must read the request within 150 mV whenever the protocol provides it.
static void test_scp_4v5_row_judges_the_adapter_readback() {
    struct Case { int vbus; int adapter; bool pass; };
    for (const Case c : {Case{4500, 4500, true}, Case{5090, 4500, true}, Case{4290, 4480, true}, Case{5090, 0, true},
                         Case{5150, 4500, false}, Case{4250, 4500, false}, Case{4500, 4700, false}, Case{5090, 5000, false}, Case{4500, 4349, false}}) {
        Fake f; f.caps = {}; f.fc_low_mv = c.vbus; f.scp_adapter_mv = c.adapter;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Scp)}, f.in, true);
        size_t row = SIZE_MAX;
        for (size_t n = 0; n < r.steps().size(); ++n) if (r.steps()[n].mv == 4500) { row = n; break; }
        CHECK_EQ(row != SIZE_MAX, true);
        if (row == SIZE_MAX) continue;
        CHECK_EQ(run_until(f, r, [&] { return r.steps()[row].verdict == ct::Verdict::Pass || r.steps()[row].verdict == ct::Verdict::Fail; }, 120000), true);
        CHECK_EQ(r.steps()[row].verdict == ct::Verdict::Pass, c.pass);
    }
}
// QC3+ whose output never leaves 5 V: "no QC3+ response", a skip (not a fail) for the 9 V row and the rows chained after it.
// The same engine failure on a QC3 row stays a failed request.
static void test_qc3plus_without_response_is_a_distinct_skip() {
    Fake f; f.caps = {}; f.no_response_proto = fc::Proto::Qc3Plus;
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Qc3Plus)}, f.in, true);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    unsigned skipped = 0, failed = 0;
    for (const auto& s : r.steps()) {
        if (s.group != ct::Group::Qc3Plus) continue;
        failed += s.verdict == ct::Verdict::Fail;
        if (s.mv == 5000 && s.verdict == ct::Verdict::Pass) continue;   // the baseline row
        CHECK_EQ(s.verdict, ct::Verdict::Skip);
        CHECK_EQ(s.reason, ct::Reason::NoQc3Plus);
        ++skipped;
    }
    CHECK_EQ(failed, 0u);
    CHECK_EQ(skipped > 0, true);
    CHECK_EQ(std::string(ct::name(ct::Reason::NoQc3Plus)) == "no QC3+ response", true);

    Fake g; g.caps = {}; g.no_response_proto = fc::Proto::Qc3;
    ct::Runner q(g);
    q.start(g.caps, {.selected = ct::bit(A::Qc3)}, g.in, true);
    g.tick(q, 200000);
    bool failed_request = false;
    for (const auto& s : q.steps()) failed_request |= s.verdict == ct::Verdict::Fail && s.reason == ct::Reason::Request;
    CHECK_EQ(failed_request, true);
}
// A QC-only bank (Xiaomi 20K, 2026-11-07) never answers FCP / SCP / AFC: the first row of the protocol ends Unsupported and the
// rest of that protocol's rows are skipped the same way, no engine session for them. The other protocols are untouched.
static void test_unsupported_protocol_skips_its_other_rows() {
    for (auto a : {A::Afc, A::Fcp, A::Scp}) {
        const auto p = a == A::Afc ? fc::Proto::Afc : a == A::Fcp ? fc::Proto::Fcp : fc::Proto::Scp;
        Fake f; f.caps = {}; f.unsupported_proto = p;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        f.tick(r, 120000);
        CHECK_EQ(r.state(), ct::State::Done);
        size_t rows = 0;
        for (const auto& s : r.steps()) {
            if (s.algorithm != a) continue;
            ++rows;
            CHECK_EQ(s.verdict == ct::Verdict::Pass, false);
            CHECK_EQ(s.reason, ct::Reason::Request);
            CHECK_EQ(s.detail, static_cast<uint8_t>(fc::Fail::Unsupported));
            CHECK_EQ(s.verdict == ct::Verdict::Fail || s.verdict == ct::Verdict::Skip, true);
        }
        CHECK_EQ(rows > 1, true);
        CHECK_EQ(f.fc_requests, 1);   // only the first row asked the engine
        size_t failed = 0;
        for (const auto& s : r.steps()) failed += s.algorithm == a && s.verdict == ct::Verdict::Fail;
        CHECK_EQ(failed, 1u);
        for (const auto& s : r.steps()) CHECK_EQ(s.algorithm == a && s.verdict == ct::Verdict::Pending, false);
    }
    // A charger that speaks the protocol still passes every row.
    for (auto a : {A::Afc, A::Fcp, A::Scp}) {
        Fake f; f.caps = {};
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        f.tick(r, 120000);
        for (const auto& s : r.steps()) if (s.algorithm == a) CHECK_EQ(s.verdict, ct::Verdict::Pass);
    }
}
// A plain 5 V source (no handshake answers at all, a bank or a PD-only port) must not pass ANY legacy row: a row passes only
// with the charger's own answer. The 5 V row of QC2 / QC3 / QC3+ is no exception (CUKTECH 120 W C1, 2026-11-06: those three
// passed 5 V on a bare vSafe5V reading while their 9 V and 12 V rows said "no handshake"). With the same plain source
// every row asks the engine and the engine says no.
static void test_silent_source_fails_every_handshake_row() {
    for (auto a : {A::Qc2, A::Qc3, A::Qc3Plus, A::Vooc, A::SuperVooc, A::SuperVooc65, A::Ufcs, A::Vivo, A::Fcp, A::Scp}) {
        Fake f; f.caps = {}; f.request_ok = false;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        f.tick(r, 60000);
        CHECK_EQ(r.steps()[0].mv, 5000);
        CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Fail);
        CHECK_EQ(f.fc_requests > 0, true);   // the 5 V row asked the engine; it was not decided from VBUS alone
        for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pass, false);
    }
}
// The same units against a charger that answers the handshake: the 5 V row passes, and only because the engine ran.
static void test_answered_handshake_passes_the_5v_row() {
    for (auto a : {A::Qc2, A::Qc3, A::Qc3Plus, A::Vooc, A::SuperVooc, A::SuperVooc65, A::Ufcs, A::Vivo, A::Fcp, A::Scp}) {
        Fake f; f.caps = {};
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        run_until(f, r, [&] { return r.steps()[0].verdict == ct::Verdict::Pass || r.steps()[0].verdict == ct::Verdict::Fail; }, 60000);
        CHECK_EQ(r.steps()[0].mv, 5000);
        CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Pass);
        CHECK_EQ(f.fc_requests >= 1, true);
    }
}
// An SVOOC adapter that accepted the cable check and ramped passes at the raised voltage; one that handshook but
// never ramped ("handshake ok") passes too, with that code; one that never clocked ("needs load") fails.
static void test_svooc_row_follows_the_adapter_ramp() {
    {
        Fake f; f.caps = {};
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::SuperVooc)}, f.in, true);
        run_until(f, r, [&] { return f.fc_requests >= 1; });
        f.in.fc_state = fc::State::Active; f.in.vbus_mv = 9000;
        f.tick(r, 1100);
        CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Pass);
    }
    for (auto why : {fc::Fail::NoVoltage, fc::Fail::NeedsLoad}) {
        Fake f; f.caps = {};
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::SuperVooc65)}, f.in, true);
        run_until(f, r, [&] { return f.fc_requests >= 1; });
        f.in.fc_state = fc::State::Failed; f.in.fc_fail = why; f.in.fc_tries = 3; f.in.vbus_mv = 5000;
        f.tick(r, 1100);
        CHECK_EQ(r.steps()[0].verdict, why == fc::Fail::NoVoltage ? ct::Verdict::Pass : ct::Verdict::Fail);
        CHECK_EQ(int(r.steps()[0].detail), int(why));
        CHECK_EQ(int(r.steps()[0].tries), 3);   // the row keeps how many wake trains it took
    }
}
// An SVOOC adapter that clocked and answered SVOOC frames but stops before the cable check passes as "detected".
static void test_svooc_detected_is_a_pass() {
    Fake f; f.caps = {};
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::SuperVooc)}, f.in, true);
    run_until(f, r, [&] { return f.fc_requests >= 1; });
    f.in.fc_state = fc::State::Failed; f.in.fc_fail = fc::Fail::Detected; f.in.fc_tries = 1; f.in.vbus_mv = 5000;
    f.tick(r, 1100);
    CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Pass);
    CHECK_EQ(int(r.steps()[0].detail), int(fc::Fail::Detected));
}
// pattern vooc-family-same-wake: the VOOC, SVOOC and SVOOC65 units open with the same wake and end on the same ident (the K2
// answers nothing, so nothing after the ident can tell the variants apart). One session decides all three rows: a detected
// family passes all three as "detected", a session that heard nothing (needs load) or no ident (no response) skips the other
// two with the first row's reason. The rows keep their names; only the first unit of the family runs an engine session.
static void test_vooc_family_shares_one_session() {
    for (const auto why : {fc::Fail::NeedsLoad, fc::Fail::NoResponse, fc::Fail::Detected}) {
        Fake f; f.caps = {};
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Vooc) | ct::bit(A::SuperVooc) | ct::bit(A::SuperVooc65)}, f.in, true);
        run_until(f, r, [&] { return f.fc_requests >= 1; });
        CHECK_EQ(int(f.protocols[0]), int(fc::Proto::Vooc));
        f.in.fc_state = fc::State::Failed; f.in.fc_fail = why; f.in.fc_tries = 1; f.in.vbus_mv = 5000;
        CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }, 120000), true);
        CHECK_EQ(f.fc_requests, 1);
        const bool pass = why == fc::Fail::Detected;
        unsigned vooc_rows = 0, svooc_rows = 0;
        for (const auto& s : r.steps()) {
            if (s.group == ct::Group::Vooc) {
                ++vooc_rows;
                CHECK_EQ(s.verdict, pass ? ct::Verdict::Pass : ct::Verdict::Fail); CHECK_EQ(int(s.detail), int(why));
            }
            if (s.group != ct::Group::SuperVooc && s.group != ct::Group::SuperVooc65) continue;
            ++svooc_rows;
            CHECK_EQ(s.verdict, pass ? ct::Verdict::Pass : ct::Verdict::Skip);
            CHECK_EQ(s.reason, pass ? ct::Reason::None : ct::Reason::Request);
            CHECK_EQ(int(s.detail), int(why));
            CHECK_EQ(int(s.tries), 1);
        }
        CHECK_EQ(vooc_rows == 1 && svooc_rows == 2, true);
    }
    // Without the VOOC unit the first family unit that runs decides for the other.
    Fake f; f.caps = {};
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::SuperVooc) | ct::bit(A::SuperVooc65)}, f.in, true);
    run_until(f, r, [&] { return f.fc_requests >= 1; });
    f.in.fc_state = fc::State::Failed; f.in.fc_fail = fc::Fail::Detected; f.in.fc_tries = 2; f.in.vbus_mv = 5000;
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }, 120000), true);
    CHECK_EQ(f.fc_requests, 1);
    for (const auto& s : r.steps()) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(int(s.detail), int(fc::Fail::Detected)); }
}
// A VIVO charger that never answered its first row (no frame in the entry window) speaks nothing the other levels could
// prove: they are skipped with that reason and get no session, as AFC / FCP / SCP / UFCS do.
static void test_vivo_no_response_skips_the_other_vivo_rows() {
    Fake f; f.caps = {};
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Vivo)}, f.in, true);
    run_until(f, r, [&] { return f.fc_requests >= 1; });
    f.in.fc_state = fc::State::Failed; f.in.fc_fail = fc::Fail::NoResponse; f.in.vbus_mv = 5000;
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }, 120000), true);
    CHECK_EQ(f.fc_requests, 1);
    CHECK_EQ(r.steps().size(), 4);
    CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Fail);
    for (size_t i = 1; i < r.steps().size(); ++i) {
        CHECK_EQ(r.steps()[i].verdict, ct::Verdict::Skip); CHECK_EQ(r.steps()[i].reason, ct::Reason::Request);
        CHECK_EQ(int(r.steps()[i].detail), int(fc::Fail::NoResponse));
    }
}
// Baseus 35 W: after a PPS 3.3 V request the unloaded output stops at 3.73 V (13 % over). The step is the source's
// best, so it passes; a source stuck at 4.5 V does not.
static void test_pps_min_step_accepts_unloaded_floor() {
    for (int stuck_mv : {3730, 4500}) {
        Fake f; f.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 60);
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Pps)}, f.in, true);
        for (int n = 0; n < 700; ++n) {
            f.tick(r, 100);
            if (f.in.pd.mv == 3300 && f.in.pd.held) f.in.vbus_mv = stuck_mv;
        }
        bool seen = false;
        for (const auto& st : r.steps())
            if (st.mv == 3300 && !seen) {   // the first one; a failed run skips the later ones
                seen = true;
                CHECK_EQ(st.verdict, stuck_mv == 3730 ? ct::Verdict::Pass : ct::Verdict::Fail);
            }
        CHECK_EQ(seen, true);
    }
}
// The wide unloaded ceiling (+10 %, min 600 mV) belongs to a DOWN-step: an unloaded source cannot sink its output down. An UP-step has no
// such excuse, the source drives the output up itself, so it is judged on the tight +-5 % / 100 mV band: a source that lands 8 % above
// the level it was asked to rise to is out of regulation and must not pass.
static void test_pps_up_step_uses_the_tight_band() {
    for (int over_pct : {3, 8}) {
        Fake f; f.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 60);
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Pps)}, f.in, true);
        bool saw_up = false;
        for (int n = 0; n < 900; ++n) {
            f.tick(r, 100);
            if (f.in.pd.held && f.in.pd.mv > 5000) { f.in.vbus_mv = int(f.in.pd.mv) + int(f.in.pd.mv) * over_pct / 100; saw_up = true; }
        }
        CHECK_EQ(saw_up, true);
        for (const auto& st : r.steps())
            if (st.mv > 5000) {   // the first up-step row
                CHECK_EQ(st.verdict, over_pct == 3 ? ct::Verdict::Pass : ct::Verdict::Fail);
                if (over_pct == 8) CHECK_EQ(st.reason, ct::Reason::Timeout);
                break;
            }
    }
}
// pattern output-capped-below-advertised (Baseus FH22 100 W, 2026-11-08): the PPS 5-21 V APDO is accepted at every level but the
// converter stops at 19.4 V. The top steps are Capped (a ceiling, not a silent timeout), the chain goes on to the lower levels, and
// the lower levels pass. Without the cap the same list passes every row.
static void run_pps_only(Fake& f, ct::Runner& r) {
    r.start(f.caps, {.selected = ct::bit(A::Pps)}, f.in, true);
    for (unsigned n = 0; n < 12000 && r.state() != ct::State::Done; ++n) f.tick(r, 100);
}
static void test_capped_pps_top_is_recorded_and_the_chain_goes_on() {
    Fake f; f.pd_max_mv = 19400; ct::Runner r(f);
    run_pps_only(f, r);
    CHECK_EQ(r.state(), ct::State::Done);
    unsigned capped = 0, ran_after = 0, skipped = 0; bool seen_capped = false;
    for (const auto& s : r.steps()) {
        if (s.algorithm != A::Pps) continue;
        if (s.verdict == ct::Verdict::Skip) ++skipped;
        if (s.reason == ct::Reason::Capped) {
            ++capped; seen_capped = true;
            CHECK_EQ(s.verdict, ct::Verdict::Fail);
            CHECK_EQ(s.mv > 19400, true);
            CHECK_EQ(s.measured_mv >= 19300 && s.measured_mv <= 19500, true);   // the ceiling it measured, not a bare timeout
        } else if (seen_capped && s.verdict == ct::Verdict::Pass) ++ran_after;
        if (s.mv <= 19000 && !s.hunt) CHECK_EQ(s.verdict, ct::Verdict::Pass);
    }
    CHECK_EQ(capped >= 1, true);
    CHECK_EQ(ran_after >= 1, true);   // levels after the capped one still ran (and passed)
    CHECK_EQ(skipped, 0);
}
// Review item E: a capped row used to wait out the whole 12 s row timeout although the plateau it is named for was proven after
// kCappedPlateauMs. The verdict now comes when the output has stood on the plateau that long, not at the timeout.
static void test_a_capped_row_does_not_wait_the_full_timeout() {
    Fake f; f.pd_max_mv = 19400; ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Pps)}, f.in, true);
    uint32_t asked_at = 0, capped_at = 0; size_t seen = 0;
    for (unsigned n = 0; n < 12000 && r.state() != ct::State::Done && !capped_at; ++n) {
        f.tick(r, 100);
        for (; seen < f.choices.size(); ++seen) if (!asked_at && f.choices[seen].mv > 19400) asked_at = f.in.now;
        for (const auto& s : r.steps()) if (s.reason == ct::Reason::Capped && !capped_at) capped_at = f.in.now;
    }
    CHECK_EQ(asked_at != 0, true);
    CHECK_EQ(capped_at != 0, true);
    CHECK_EQ(capped_at - asked_at < 8000u, true);   // not the 12 s timeout
    CHECK_EQ(capped_at - asked_at >= ct::kCappedPlateauMs, true);   // but only after the plateau was proven
}
// Hardware 2026-11-11 (Baseus FH22, PPS 21 V row on a source that stops at 19.42 V): the sink's PPS keep-alive re-requests the same
// level every 5 s and `ready` drops for 20-140 ms between its Request and PS_RDY. That reset the plateau just before it reached
// kCappedPlateauMs, every time, so the row waited out the 12 s timeout. The keep-alive of the SAME level keeps the plateau.
struct KaRun { uint32_t asked_at = 0, capped_at = 0; ct::Verdict verdict = ct::Verdict::Pending; ct::Reason reason = ct::Reason::None; };
static KaRun run_21v_row(Fake& f, std::vector<std::pair<uint32_t, int>> script, std::vector<uint32_t> windows, uint32_t until_ms) {
    f.out_script_mv = 21000; f.out_script = std::move(script); f.ka_period_ms = 5000; f.ka_windows = std::move(windows); f.step_ms = 20;
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Pps)}, f.in, true);
    KaRun k; size_t seen = 0;
    for (unsigned n = 0; n < 2000000 && r.state() != ct::State::Done; n += f.step_ms) {
        f.tick(r, f.step_ms);
        for (; seen < f.choices.size(); ++seen) if (!k.asked_at && f.choices[seen].mv == 21000) k.asked_at = f.in.now;
        if (!k.asked_at) continue;
        for (const auto& s : r.steps())
            if (s.mv == 21000 && !s.hunt && s.verdict >= ct::Verdict::Pass && !k.capped_at) { k.capped_at = f.in.now; k.verdict = s.verdict; k.reason = s.reason; }
        if (k.capped_at || f.in.now - k.asked_at > until_ms) break;
    }
    return k;
}
static void test_keep_alive_windows_do_not_restart_the_capped_plateau() {
    Fake f;
    const auto k = run_21v_row(f, {{0, 5000}, {2500, 19420}}, {20, 140}, 20000);
    CHECK_EQ(int(k.reason), int(ct::Reason::Capped));
    CHECK_EQ(k.capped_at != 0, true);
    CHECK_EQ(k.capped_at - k.asked_at >= ct::kCappedPlateauMs, true);   // only after the plateau was proven
    CHECK_EQ(k.capped_at - k.asked_at < 9500u, true);                   // ~2.5 s ramp + 5 s plateau, not the 12 s timeout
}
// A slow ramp that pauses on a ledge (3 s, under kCappedPlateauMs) and then rises still passes, keep-alive windows and all.
static void test_a_ledge_shorter_than_the_capped_plateau_still_passes_through_keep_alive() {
    Fake f;
    const auto k = run_21v_row(f, {{0, 5000}, {2000, 15000}, {5000, 15000}, {6000, 21000}}, {20, 140}, 20000);
    CHECK_EQ(int(k.verdict), int(ct::Verdict::Pass));
    CHECK_EQ(int(k.reason), int(ct::Reason::None));
}
// A keep-alive Request that stays unanswered for longer than the bound is not a renewal: the plateau restarts at its PS_RDY, so the
// verdict comes from the row timeout as it did.
static void test_a_long_unanswered_keep_alive_restarts_the_plateau() {
    Fake f;
    const auto k = run_21v_row(f, {{0, 5000}, {2500, 19420}}, {2500}, 20000);
    CHECK_EQ(int(k.reason), int(ct::Reason::Capped));
    CHECK_EQ(k.capped_at - k.asked_at >= 11500u, true);
}
static void test_an_uncapped_pps_run_has_no_capped_step() {
    Fake f; ct::Runner r(f);
    run_pps_only(f, r);
    for (const auto& s : r.steps())
        if (s.algorithm == A::Pps && !s.hunt) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(s.reason != ct::Reason::Capped, true); }
}
// A source that never moves off 5 V did not answer the request: that stays a plain timeout (nothing is capped at vSafe5V), and the
// rest of the chain is skipped as before.
static void test_a_pps_stuck_at_5v_is_a_timeout_not_capped() {
    Fake f; f.pd_max_mv = 5000; ct::Runner r(f);
    run_pps_only(f, r);
    bool timeout = false;
    for (const auto& s : r.steps()) {
        CHECK_EQ(s.reason != ct::Reason::Capped, true);
        timeout |= s.algorithm == A::Pps && s.reason == ct::Reason::Timeout;
    }
    CHECK_EQ(timeout, true);
}
// pattern burst-mode-ripple-at-20v: an unloaded charger in burst mode swings VBUS around its level (TOPADRE 65 W: 18.39-20.70 V,
// mean 19.80 V, 22 % of the 20 Hz samples outside +-5 %). The fixed PD band is judged on a short mean, not on every sample.
// Runs the Pd3Fixed 20 V row with vbus_at(n) applied once per 100 ms tick until the row has a verdict; returns its index.
template <class VbusAt>
static size_t drive_fixed_20v(Fake& f, ct::Runner& r, VbusAt vbus_at, unsigned max_ticks = 1500) {
    r.start(f.caps, {.selected = ct::bit(A::Pd3Fixed)}, f.in, true);
    size_t row = SIZE_MAX;
    for (size_t n = 0; n < r.steps().size(); ++n) if (r.steps()[n].algorithm == A::Pd3Fixed && r.steps()[n].mv == 20000 && !r.steps()[n].hunt && row == SIZE_MAX) row = n;
    CHECK_EQ(row != SIZE_MAX, true);
    unsigned k = 0;   // ticks since the 20 V contract: vbus_at(k)
    for (unsigned n = 0; n < max_ticks; ++n) {
        if (f.in.pd.held && f.in.pd.mv == 20000) f.in.vbus_mv = vbus_at(k++);
        f.tick(r, 100);
        const auto v = r.steps()[row].verdict;
        if (v == ct::Verdict::Pass || v == ct::Verdict::Fail || v == ct::Verdict::Skip) break;
    }
    return row;
}
static void test_fixed_band_judges_a_burst_mode_ripple_on_its_mean() {
    // +-1.2 V around 19.8 V, alternating per tick: 18.6 V is 7 % low, 21.0 V is in band; the mean is 19.5-20.1 V.
    Fake f; ct::Runner r(f);
    const size_t row = drive_fixed_20v(f, r, [](unsigned n) { return n % 2 ? 21000 : 18600; });
    CHECK_EQ(r.steps()[row].verdict, ct::Verdict::Pass);
    CHECK_EQ(r.steps()[row].measured_mv >= 19400 && r.steps()[row].measured_mv <= 20200, true);   // the row records the window mean

    // The same ripple across a quiet hold: the 1 s hold is judged on the mean too.
    Fake g; ct::Runner gr(g);
    const size_t grow = drive_fixed_20v(g, gr, [](unsigned n) { return n < 8 ? 19900 : n % 2 ? 21000 : 18600; });
    CHECK_EQ(gr.steps()[grow].verdict, ct::Verdict::Pass);
}
static void test_fixed_band_still_fails_a_steady_low_or_high_level() {
    for (int mv : {18500, 21500}) {   // 7.5 % low / 7.5 % high: the mean is out of band
        Fake f; ct::Runner r(f);
        const size_t row = drive_fixed_20v(f, r, [mv](unsigned) { return mv; });
        CHECK_EQ(r.steps()[row].verdict, ct::Verdict::Fail);
        CHECK_EQ(r.steps()[row].reason, ct::Reason::Timeout);
        CHECK_EQ(std::abs(int(r.steps()[row].measured_mv) - mv) < 100, true);
    }
    // In Holding a level that drops out of band fails as Voltage once the window mean follows it (a few samples: the cost of the mean).
    Fake g; ct::Runner gr(g);
    const size_t grow = drive_fixed_20v(g, gr, [](unsigned n) { return n < 4 ? 20000 : 18500; });
    CHECK_EQ(gr.steps()[grow].verdict, ct::Verdict::Fail); CHECK_EQ(gr.steps()[grow].reason, ct::Reason::Voltage);
}
static void test_fixed_band_keeps_the_per_sample_overvoltage_guards() {
    const auto ripple_then_spike = [](unsigned n) { return n < 9 ? 19900 : n == 9 ? 25000 : 19900; };
    {   // no cap or cable in the way: one 25 V sample is a gross excursion and fails the hold at once (it did before)
        Fake f; ct::Runner r(f);
        const size_t row = drive_fixed_20v(f, r, ripple_then_spike);
        CHECK_EQ(r.steps()[row].verdict, ct::Verdict::Fail); CHECK_EQ(r.steps()[row].reason, ct::Reason::Voltage);
        CHECK_EQ(r.steps()[row].measured_mv, 25000);
    }
    {   // the K2's own monitor ceiling: OverCap, per sample, ends the run
        Fake f; f.in.cap_mv = 22000; ct::Runner r(f);   // abort line 23.1 V: the 25 V sample is over it
        const size_t row = drive_fixed_20v(f, r, ripple_then_spike);
        CHECK_EQ(r.steps()[row].verdict, ct::Verdict::Fail); CHECK_EQ(r.steps()[row].reason, ct::Reason::OverCap);
    }
    {   // a 20 V cable (21 V measured limit): CableLimit, per sample
        Fake f; f.in.cable_mv = 20000; ct::Runner r(f);
        const size_t row = drive_fixed_20v(f, r, ripple_then_spike);
        CHECK_EQ(r.steps()[row].verdict, ct::Verdict::Skip); CHECK_EQ(r.steps()[row].reason, ct::Reason::CableLimit);
    }
}
static void test_missing_samples_and_wrap() {
    Fake f; f.in.now = 0xfffffc00; ct::Runner r(f);
    r.start(f.caps, kPd, f.in, true);
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Holding; }), true);   // the 32-bit clock wraps while the unit attaches
    read_only(r, f);
    f.tick(r, 200);
    f.in.now += 1000; r.poll(f.in, f.caps);
    f.tick(r, 900); CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Running);
    f.tick(r, 100); CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Pass);
}
static void test_pps_renewal() {
    Fake f; ct::Runner r(f);
    r.start(f.caps, kPd, f.in, true);
    for (unsigned n = 0; n < 300 && !(r.index() == 5 && r.state() == ct::State::Holding); ++n)
        f.tick(r, 100);
    CHECK_EQ(r.steps()[5].verdict, ct::Verdict::Running);
    CHECK_EQ(r.state(), ct::State::Holding);
    f.in.pd.state = pd::SinkState::Requesting;
    f.tick(r, 200);
    CHECK_EQ(r.steps()[5].verdict, ct::Verdict::Running);
    f.in.pd.state = pd::SinkState::Transition;
    f.tick(r, 200);
    CHECK_EQ(r.steps()[5].verdict, ct::Verdict::Running);
    f.in.pd.state = pd::SinkState::Ready;
    f.tick(r, 100);
    CHECK_EQ(r.steps()[5].verdict, ct::Verdict::Pass);
}
static void test_read_revision() {
    Fake f; ct::Runner r(f);
    CHECK_EQ(r.run_id(), 0); CHECK_EQ(r.revision(), 0);
    r.start(f.caps, kPd, f.in, false);
    CHECK_EQ(r.run_id(), 0); CHECK_EQ(r.revision(), 1);
    r.start(f.caps, kPd, f.in, true);
    CHECK_EQ(r.run_id(), 1); CHECK_EQ(r.revision(), 2);
    const auto* storage = r.steps().data();
    const auto r0 = r.revision();
    f.tick(r, 100);
    CHECK_EQ(r.state(), ct::State::Baseline); CHECK_EQ(r.revision(), r0);   // the START settle is a timer only
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Detach; }), true);
    const auto detaching = r.revision();
    CHECK_EQ(detaching, r0 + 1);
    f.tick(r, 500);
    CHECK_EQ(r.state(), ct::State::Detach); CHECK_EQ(r.revision(), detaching);   // waiting for the source to drop is not a stored result
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Holding; }), true);
    const auto holding = r.revision();
    f.tick(r, 400);
    CHECK_EQ(r.revision(), holding);   // holding timers are not stored results
    r.cancel(f.in);
    read_only(r, f);
    CHECK_EQ(r.revision(), holding + 1);
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Stopped; }), true);
    const auto stopped = r.revision();
    f.tick(r, 1000);
    CHECK_EQ(r.revision(), stopped);
    r.start(f.caps, kPd, f.in, true);
    CHECK_EQ(r.run_id(), 2); CHECK_EQ(r.revision(), stopped + 1);
    CHECK_EQ(r.steps().data() == storage, true);
}
static void test_selection() {
    ct::Options options;
    CHECK_EQ(options.selected, ct::kDefaultAlgorithms);
    CHECK_EQ(options.contains(A::Pd30Revision) || options.contains(A::Pd31Revision) || options.contains(A::Pd32Revision), false);
    options.selected = ct::kAllAlgorithms;
    for (unsigned i = 0; i < unsigned(A::Count); ++i) {
        const auto a = static_cast<A>(i);
        CHECK_EQ(options.contains(a), true);
        options.selected ^= ct::bit(a); CHECK_EQ(options.contains(a), false);
        Fake f; ct::Runner r(f); r.start(f.caps, options, f.in, true);
        for (const auto& s : r.steps()) CHECK_EQ(s.algorithm == a, false);
        options.selected ^= ct::bit(a); CHECK_EQ(options.contains(a), true);
    }
    CHECK_EQ(ct::bit(A::Count), 0); CHECK_EQ(options.contains(A::Count), false);
    Fake f; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, {.selected = 0}, f.in, true), ct::Reason::EmptySelection);
    CHECK_EQ(r.start(f.caps, {.selected = 1u << 31}, f.in, true), ct::Reason::EmptySelection);
    CHECK_EQ(r.steps().empty(), true); CHECK_EQ(r.run_id(), 0);
    CHECK_EQ(ct::Runner::preview(f.caps, {.selected = 0}, f.in).refusal, ct::Reason::EmptySelection);
    CHECK_EQ(f.pd_requests + f.fc_requests + f.releases, 0);
    CHECK_EQ(r.start(f.caps, {.selected = ct::bit(A::Vivo) | (1u << 31)}, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }), true);
    const auto revision = r.revision(), run = r.run_id();
    CHECK_EQ(r.evidence().selected, ct::bit(A::Vivo));
    CHECK_EQ(r.start(f.caps, {.selected = 0}, f.in, true), ct::Reason::EmptySelection);
    CHECK_EQ(r.revision(), revision); CHECK_EQ(r.run_id(), run); CHECK_EQ(r.reason(), ct::Reason::None);
    CHECK_EQ(r.steps()[0].algorithm, A::Vivo); CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Pass);
}
static void test_selected_skip_without_operations() {
    Fake f; f.in.reported = {.valid = true, .rev_major = 3, .rev_minor = 2, .ver_major = 1, .ver_minor = 7};
    const uint32_t mask = ct::bit(A::Pd30Revision) | ct::bit(A::Pd31Revision) | ct::bit(A::Pd32Revision);
    ct::Runner r(f);
    const auto preview = ct::Runner::preview(f.caps, {.selected = mask}, f.in);
    CHECK_EQ(preview.total, 3); CHECK_EQ(preview.skipped, 3); CHECK_EQ(preview.runnable, 0);
    CHECK_EQ(f.records.size(), 0); CHECK_EQ(f.pd_requests + f.fc_requests + f.releases, 0);
    r.start(f.caps, {.selected = mask}, f.in, true); f.tick(r, 5000);
    CHECK_EQ(r.state(), ct::State::Done); CHECK_EQ(r.steps().size(), 3);
    for (const auto& s : r.steps()) {
        CHECK_EQ(s.verdict, ct::Verdict::Skip);
        CHECK_EQ(s.reason, ct::Reason::RevisionUntestable);
        CHECK_EQ(s.pos + s.mv + s.ma + s.measured_mv + s.pdo, 0);
        CHECK_EQ(std::string_view(ct::explanation(s)).empty(), false);
    }
    CHECK_STR(ct::explanation(r.steps()[0]), "No independent PD3.0 test");
    CHECK_STR(ct::explanation(r.steps()[2]), "No independent PD3.2 test");
    CHECK_EQ(f.records.size(), 3); CHECK_EQ(f.pd_requests + f.fc_requests + f.releases, 0);
    CHECK_EQ(r.evidence().wire_rev, 2); CHECK_EQ(r.evidence().reported.rev_minor, 2);
    read_only(r, f);
}
static void test_wire_classes_and_feature_evidence() {
    for (unsigned wire : {1u, 2u, 3u}) {
        Fake f; f.caps.spec_rev = wire; ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Pd2Fixed) | ct::bit(A::Pd3Fixed)}, f.in, true);
        f.tick(r, 50000);
        unsigned passed = 0;
        for (const auto& s : r.steps()) {
            passed += s.verdict == ct::Verdict::Pass;
            if (s.verdict == ct::Verdict::Pass) CHECK_EQ(s.algorithm, wire == 1 ? A::Pd2Fixed : A::Pd3Fixed);
            else { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::RevisionUntestable); }
        }
        CHECK_EQ(passed, wire == 3 ? 0 : 5);   // the 5 fixed rows; the 20 V 6 A hunt row (a fixed PDO never advertises 6 A) is dropped, 6 before 2026-11-07
        CHECK_EQ(f.pd_requests, passed); CHECK_EQ(f.fc_requests, 0);
    }
    Fake f; f.caps.pdos[0].raw |= 1u << 23;
    f.in.reported = {.valid = true, .rev_major = 3, .rev_minor = 1, .ver_major = 1, .ver_minor = 0};
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    CHECK_EQ(r.evidence().flags & ct::PpsOffer, ct::PpsOffer);
    CHECK_EQ(r.evidence().flags & ct::EprCapable, ct::EprCapable);
    CHECK_EQ(r.evidence().flags & ct::EprTable, 0);
    CHECK_EQ(r.evidence().flags & ct::SprAvsOffer, 0);
    CHECK_EQ(r.evidence().wire_rev, 2); CHECK_EQ(r.evidence().reported.rev_minor, 1);
    const auto e = r.evidence();
    CHECK_EQ(run_until(f, r, [&] { return f.pd_requests == 1; }), true);
    f.in.reported.rev_minor = 2; f.caps.spec_rev = 1;
    f.tick(r, 700);
    CHECK_EQ(f.pd_requests, 1); CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Fail); CHECK_EQ(r.steps()[0].reason, ct::Reason::CapsChanged);
    CHECK_EQ(r.evidence().reported.rev_minor, e.reported.rev_minor);
    CHECK_EQ(r.evidence().raw_header, e.raw_header); CHECK_EQ(r.evidence().session, e.session);
}
// Class B (20 V) is part of the default selection; its rows plan after the QC2 / QC3 rows of the same unit.
static void test_qc_class_b_is_in_the_default_selection() {
    CHECK_EQ(ct::Options{}.contains(A::Qc2ClassB), true);
    CHECK_EQ(ct::Options{}.contains(A::Qc3ClassB), true);
    CHECK_EQ((ct::kDefaultAlgorithms & ct::bit(A::Qc2ClassB)) != 0, true);
    Fake f; f.caps = {}; ct::Runner r(f);
    r.start(f.caps, {}, f.in, true);
    size_t last_qc2 = 0, first_qc2b = SIZE_MAX, last_qc3 = 0, first_qc3b = SIZE_MAX;
    for (size_t n = 0; n < r.steps().size(); ++n) {
        const auto a = r.steps()[n].algorithm;
        if (a == A::Qc2) last_qc2 = n;
        if (a == A::Qc2ClassB && first_qc2b == SIZE_MAX) first_qc2b = n;
        if (a == A::Qc3) last_qc3 = n;
        if (a == A::Qc3ClassB && first_qc3b == SIZE_MAX) first_qc3b = n;
    }
    CHECK_EQ(first_qc2b != SIZE_MAX && first_qc2b > last_qc2, true);
    CHECK_EQ(first_qc3b != SIZE_MAX && first_qc3b > last_qc3, true);
}
// A Class A charger (12 V top) never reaches 20 V: the Class B rows are neutral skips ("Class A only"), not failures. The rows
// below 20 V in the QC3 Class B ramp are real (they pass); the 12 V / 5 V rows that follow the missing 20 V are skipped the same way.
static void test_class_b_on_a_class_a_charger_is_a_neutral_skip() {
    for (const auto a : {A::Qc2ClassB, A::Qc3ClassB}) {
        Fake f; f.caps = {}; f.fc_max_mv = 12000;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        f.tick(r, 200000);
        CHECK_EQ(r.state(), ct::State::Done);
        unsigned skipped = 0, failed = 0, passed = 0;
        for (const auto& s : r.steps()) {
            failed += s.verdict == ct::Verdict::Fail;
            passed += s.verdict == ct::Verdict::Pass;
            if (s.verdict == ct::Verdict::Skip) { ++skipped; CHECK_EQ(s.reason, ct::Reason::ClassAOnly); }
        }
        CHECK_EQ(failed, 0u);
        if (a == A::Qc2ClassB) { CHECK_EQ(skipped, 1u); CHECK_EQ(passed, 0u); }
        else { CHECK_EQ(skipped, 3u); CHECK_EQ(passed, 2u); }   // 5 V and 12 V pass; 20 V, 12 V, 5 V skip
    }
    CHECK_EQ(std::string(ct::name(ct::Reason::ClassAOnly)) == "Class A only (12 V max)", true);
    // The default run: QC2 / QC3 pass, Class B reads as skips, the run has no failed row.
    Fake f; f.caps = {}; f.fc_max_mv = 12000;
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::kDefaultAlgorithms & (ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Qc2ClassB) | ct::bit(A::Qc3ClassB))}, f.in, true);
    f.tick(r, 300000);
    for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Fail, false);
}
// pattern qc3-20v-without-qc2-20v (Baseus CJ11): QC3 Class B reaches 20 V, QC2's discrete 20 V level does not. The charger is Class B,
// so the QC2 Class B 20 V row is a real fail ("20 V via QC3 only"), whichever unit ran first.
static void test_qc2_class_b_miss_on_a_qc3_class_b_charger_is_a_fail() {
    for (const bool qc3_first : {false, true}) {
        Fake f; f.caps = {}; f.qc2_no_20v = true;
        ct::Runner r(f);
        ct::Options o{.selected = ct::bit(A::Qc2ClassB) | ct::bit(A::Qc3ClassB)};
        if (qc3_first) { o.units.u[0] = ct::Unit::Qc3; o.units.u[1] = ct::Unit::Qc2; o.units.n = 2; }
        r.start(f.caps, o, f.in, true);
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        bool qc2b = false, qc3b20 = false;
        for (const auto& s : r.steps()) {
            if (s.algorithm == A::Qc2ClassB && s.mv == 20000) {
                qc2b = true; CHECK_EQ(s.verdict, ct::Verdict::Fail); CHECK_EQ(s.reason, ct::Reason::Qc3Only20V);
            }
            if (s.algorithm == A::Qc3ClassB && s.mv == 20000) { qc3b20 = true; CHECK_EQ(s.verdict, ct::Verdict::Pass); }
        }
        CHECK_EQ(qc2b && qc3b20, true);
    }
    CHECK_EQ(std::string(ct::name(ct::Reason::Qc3Only20V)) == "20 V via QC3 only", true);
}
// JT-P18 USB-C (2026-11-07): the port speaks no QC, QC2 5/9/12 V all fail "no handshake". "Class A only" claims the charger does QC2
// and tops out at 12 V: with no QC2 handshake in the run the Class B 20 V row carries the handshake failure, not the neutral skip.
// Class B selected alone (no Class A rows to learn from) keeps the neutral skip.
static void test_qc2_class_b_after_a_failed_qc2_handshake_is_not_class_a_only() {
    {
        Fake f; f.caps = {}; f.no_handshake_proto = fc::Proto::Qc2;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Qc2) | ct::bit(A::Qc2ClassB)}, f.in, true);
        f.tick(r, 200000);
        CHECK_EQ(r.state(), ct::State::Done);
        unsigned a_rows = 0, b_rows = 0;
        for (const auto& s : r.steps()) {
            // the 5 V row opens the session and fails; 9 V / 12 V are retargets in it and are skipped with its failure
            if (s.algorithm == A::Qc2) { ++a_rows; CHECK_EQ(s.verdict, a_rows == 1 ? ct::Verdict::Fail : ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::Request); }
            if (s.algorithm == A::Qc2ClassB) {
                ++b_rows; CHECK_EQ(s.verdict, ct::Verdict::Fail); CHECK_EQ(s.reason, ct::Reason::Request);
                CHECK_EQ(s.detail, static_cast<uint8_t>(fc::Fail::NoHandshake));
            }
        }
        CHECK_EQ(a_rows, 3u); CHECK_EQ(b_rows, 1u);
    }
    {
        Fake f; f.caps = {}; f.no_handshake_proto = fc::Proto::Qc2;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Qc2ClassB)}, f.in, true);
        f.tick(r, 200000);
        CHECK_EQ(r.state(), ct::State::Done);
        for (const auto& s : r.steps()) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::ClassAOnly); }
    }
}
// A charger that does reach 20 V passes the Class B rows as before.
static void test_class_b_pass_on_a_class_b_charger_stays_a_pass() {
    for (const auto a : {A::Qc2ClassB, A::Qc3ClassB}) {
        Fake f; f.caps = {};
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        f.tick(r, 200000);
        CHECK_EQ(r.state(), ct::State::Done);
        for (const auto& s : r.steps()) CHECK_EQ(s.verdict, ct::Verdict::Pass);
        bool asked20 = false;
        for (const int mv : f.fc_mvs) asked20 |= mv == 20000;
        CHECK_EQ(asked20, true);
    }
}
// A Class A charger answers the 20 V request and stays at 12 V: the row ends as the same neutral skip after ~3 s (not the 12 s QC3 /
// 15 s QC2 timeout). A VBUS that leaves the 12 V band at any point (a slow Class B charger) keeps the full timeout.
static size_t row_at_20v(const ct::Runner& r) {
    for (size_t n = 0; n < r.steps().size(); ++n) if (r.steps()[n].mv == 20000) return n;
    return SIZE_MAX;
}
static void test_class_b_row_on_a_charger_stuck_at_12v_ends_early() {
    for (const auto a : {A::Qc2ClassB, A::Qc3ClassB}) {
        Fake f; f.caps = {}; f.fc_max_mv = 12000;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        const size_t k = row_at_20v(r);
        CHECK_EQ(run_until(f, r, [&] { return r.steps()[k].verdict == ct::Verdict::Running; }, 60000), true);
        const uint32_t t0 = f.in.now;
        CHECK_EQ(run_until(f, r, [&] { return r.steps()[k].verdict != ct::Verdict::Running; }, 60000), true);
        CHECK_EQ(f.in.now - t0 <= 4000, true);
        CHECK_EQ(r.steps()[k].verdict, ct::Verdict::Skip); CHECK_EQ(r.steps()[k].reason, ct::Reason::ClassAOnly);
    }
    for (const auto a : {A::Qc2ClassB, A::Qc3ClassB}) {
        Fake f; f.caps = {}; f.fc_max_mv = 12000;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        const size_t k = row_at_20v(r);
        CHECK_EQ(run_until(f, r, [&] { return r.steps()[k].verdict == ct::Verdict::Running; }, 60000), true);
        const uint32_t t0 = f.in.now;
        f.tick(r, 1500); f.in.vbus_mv = 14000;   // it moved: this is no Class A top
        CHECK_EQ(run_until(f, r, [&] { return r.steps()[k].verdict != ct::Verdict::Running; }, 60000), true);
        CHECK_EQ(f.in.now - t0 >= 11000, true);
        CHECK_EQ(r.steps()[k].verdict, ct::Verdict::Skip); CHECK_EQ(r.steps()[k].reason, ct::Reason::ClassAOnly);
    }
}
// The K2's own ceiling stays in front of the 20 V request: a run whose cap is below 20 V skips the row (OverCap) and never asks.
static void test_class_b_keeps_the_voltage_ceiling_in_front() {
    for (const auto a : {A::Qc2ClassB, A::Qc3ClassB}) {
        Fake f; f.caps = {}; f.in.cap_mv = 15000;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        f.tick(r, 200000);
        CHECK_EQ(r.state(), ct::State::Done);
        for (const int mv : f.fc_mvs) CHECK_EQ(mv < 20000, true);
        for (const auto& s : r.steps()) if (s.mv == 20000) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::OverCap); }
    }
}
static void test_qc_class_b_rows_are_planned_per_algorithm() {
    for (const auto a : {A::Qc2ClassB, A::Qc3ClassB}) {
        Fake f; f.caps = {}; ct::Runner r(f);
        ct::Options o{.selected = ct::bit(a)};
        r.start(f.caps, o, f.in, true);
        unsigned n20 = 0, n = 0;
        for (const auto& s : r.steps()) {
            CHECK_EQ(s.algorithm, a);
            CHECK_EQ(s.group, a == A::Qc2ClassB ? ct::Group::Qc2 : ct::Group::Qc3);
            n20 += s.mv == 20000; ++n;
        }
        CHECK_EQ(n20, 1u); CHECK_EQ(n, a == A::Qc2ClassB ? 1u : 5u);
    }
}
// pattern qc-redetect-after-reset (power strip USB-A, 2026-11-07): the QC2 rows are one session (5 V opens it, 9 V / 12 V / Class B 20 V
// are retargets in it), and the QC3 Class B ramp goes on in the QC3 unit's session. A fresh handshake right after our own release is
// refused by this charger; the plan never makes one inside a unit.
static void test_qc_rows_after_the_first_are_retargets_in_one_session() {
    {
        Fake f; f.caps = {}; f.redetect_gap_ms = 1000;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Qc2) | ct::bit(A::Qc2ClassB)}, f.in, true);
        f.tick(r, 200000);
        CHECK_EQ(r.state(), ct::State::Done);
        for (const auto& s : r.steps()) CHECK_EQ(s.verdict, ct::Verdict::Pass);
        CHECK_EQ(r.steps().size(), 4u);
        CHECK_EQ(f.fc_requests, 4); CHECK_EQ(f.retargets, 3); CHECK_EQ(f.redetect_refused, 0);
        CHECK_EQ(f.fc_releases, 1);
        const std::vector<int> mvs{5000, 9000, 12000, 20000};
        CHECK_EQ(f.fc_mvs == mvs, true);
    }
    {   // QC3 unit: the Class B ramp continues the QC3 session (5 rows + 5 rows, one fresh handshake)
        Fake f; f.caps = {}; f.redetect_gap_ms = 1000;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Qc3) | ct::bit(A::Qc3ClassB)}, f.in, true);
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        for (const auto& s : r.steps()) CHECK_EQ(s.verdict, ct::Verdict::Pass);
        CHECK_EQ(r.steps().size(), 10u);
        CHECK_EQ(f.fc_requests, 10); CHECK_EQ(f.retargets, 9); CHECK_EQ(f.redetect_refused, 0);
        CHECK_EQ(f.fc_releases, 1);
    }
    for (const auto a : {A::Qc2ClassB, A::Qc3ClassB}) {   // selected alone: nothing before it to continue, its own session
        Fake f; f.caps = {};
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
        CHECK_EQ(r.steps()[0].continuous, false);
        f.tick(r, 200000);
        CHECK_EQ(f.retargets, a == A::Qc2ClassB ? 0 : 4);
    }
}
// A level the charger does not follow ends that row and the session; the Class B row after it is still tried (fresh session) and keeps
// its labels: "Class A only" where a QC2 row passed, the handshake failure where none did.
static void test_a_failed_qc2_retarget_leaves_the_class_b_row_to_run() {
    {
        Fake f; f.caps = {}; f.fc_max_mv = 9000;   // follows 9 V, stays there for 12 V and 20 V
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Qc2) | ct::bit(A::Qc2ClassB)}, f.in, true);
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        const ct::Verdict v[] = {ct::Verdict::Pass, ct::Verdict::Pass, ct::Verdict::Fail, ct::Verdict::Skip};
        CHECK_EQ(r.steps().size(), 4u);
        for (size_t n = 0; n < 4; ++n) CHECK_EQ(r.steps()[n].verdict, v[n]);
        CHECK_EQ(r.steps()[3].reason, ct::Reason::ClassAOnly);
        CHECK_EQ(f.fc_requests, 4);
    }
    {   // the first level fails: the 9 V / 12 V retargets are skipped, the Class B row still asks
        Fake f; f.caps = {}; f.no_handshake_proto = fc::Proto::Qc2;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Qc2) | ct::bit(A::Qc2ClassB)}, f.in, true);
        f.tick(r, 200000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Fail);
        for (size_t n = 1; n < 3; ++n) { CHECK_EQ(r.steps()[n].verdict, ct::Verdict::Skip); CHECK_EQ(r.steps()[n].reason, ct::Reason::Request); }
        CHECK_EQ(r.steps()[3].verdict, ct::Verdict::Fail); CHECK_EQ(r.steps()[3].reason, ct::Reason::Request);
        CHECK_EQ(f.fc_requests, 2);
    }
}
static void test_full_legacy_ranges_and_guards() {
    Fake f; f.caps = {}; ct::Runner r(f);
    r.start(f.caps, {}, f.in, true);
    bool qc20 = false, qc320 = false;
    for (const auto& s : r.steps()) {
        qc20 |= s.algorithm == A::Qc2 && s.mv == 20000;
        qc320 |= s.algorithm == A::Qc3 && s.mv > 12000;
        if (s.algorithm == A::Qc3Plus) CHECK_EQ(s.mv <= 12000, true);
    }
    CHECK_EQ(qc20 || qc320, false);   // QC2/QC3 never plan a 20 V row
    f.tick(r, 500000);
    CHECK_EQ(r.state(), ct::State::Done); CHECK_EQ(f.pd_requests, 0); CHECK_EQ(f.rescans, 0);
    CHECK_EQ(f.fc_requests, 41); CHECK_EQ(f.retargets, 24);   // the QC 5 V rows are requested too; 40 = 34 + the six Class B rows; retargets as in the full plan: QC2 3, QC3 4 + QC3B 5, QC3+ 4, UFCS 5, AFC 1, FCP 2
    for (const auto p : f.protocols) CHECK_EQ(p < fc::Proto::Count, true);

    for (unsigned mode = 0; mode < 3; ++mode) {
        Fake limited; limited.caps = {};
        if (mode == 0) limited.in.cap_mv = 15000;
        if (mode == 1) { limited.in.cable_mv = 12000; limited.in.spoof_on = true; }
        if (mode == 2) limited.in.cable_ma = 3000;
        ct::Runner lr(limited); lr.start(limited.caps, {}, limited.in, true); limited.tick(lr, 500000);
        for (const auto& s : lr.steps()) {
            if (s.verdict != ct::Verdict::Skip) {
                CHECK_EQ(s.mv <= limited.in.cap_mv && s.mv <= limited.in.cable_mv, true);
            }
            if ((mode == 0 || mode == 1) && s.mv == 20000) {
                CHECK_EQ(s.verdict, ct::Verdict::Skip);
                CHECK_EQ(s.reason, mode == 0 ? ct::Reason::OverCap : ct::Reason::CableLimit);
            }
        }
    }
}
// SCP: the suite draws no current, so the cable rating gates no row: all four rows (handshake, 10 V 2.25 A, 4.5 V 5 A,
// 5 V 4.5 A) are pending on an assumed 3 A cable, an observed 3 A cable and an observed 5 A cable alike.
static void test_scp_rows_ignore_the_cable_current() {
    for (int c = 0; c < 3; ++c) {
        Fake f; f.caps = {};
        f.in.cable_observed = c != 0; f.in.cable_ma = c == 2 ? 5000 : 3000;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Scp)}, f.in, true);
        f.tick(r, 600);
        CHECK_EQ(r.steps().size(), 4);
        CHECK_EQ(r.steps()[0].ma, 0); CHECK_EQ(r.steps()[1].ma, 2250);
        CHECK_EQ(r.steps()[2].ma, 5000); CHECK_EQ(r.steps()[3].ma, 4500);
        for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Skip, false);
    }
}
// The guard that replaces the cable-current rule: current above kLoadAbortMa during a raised row stops the row.
static void test_scp_row_aborts_when_a_load_draws_current() {
    Fake f; f.caps = {};
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Scp)}, f.in, true);
    f.tick(r, 600);
    f.in.fc_state = fc::State::Failed; f.in.fc_fail = fc::Fail::Detected; f.in.vbus_mv = 5000;
    f.tick(r, 1100);
    f.in.fc_state = fc::State::Active; f.in.fc_fail = fc::Fail::None; f.in.vbus_mv = 10000; f.in.ibus_ma = 800;
    f.tick(r, 3000);
    CHECK_EQ(r.steps()[1].verdict == ct::Verdict::Pass, false);
}
// SCP / UFCS rows follow what the charger reported (fc::Caps, read by the engine in the unit's first session). START plans the old
// fixed rows (nothing is known yet; a list left in the engine by an earlier session is not trusted); the rows still to run are
// planned again once the first row has read the capabilities. No row asks for more than the adapter reported.
static fc::Caps scp_caps_of(int min_mv, int max_mv, int min_ma, int max_ma) {
    fc::Caps c; c.scp = {min_mv, max_mv, min_ma, max_ma, true}; return c;
}
static ufcs::Cap ufcs_mode(int pos, int min_mv, int max_mv, int min_ma, int max_ma) {
    ufcs::Cap c; c.min_mv = min_mv; c.max_mv = max_mv; c.min_ma = min_ma; c.max_ma = max_ma;
    c.pos = static_cast<uint8_t>(pos); c.step_mv = 10; c.step_ma = 10; return c;
}
static fc::Caps ufcs_caps_of(std::initializer_list<ufcs::Cap> modes) {
    fc::Caps c; for (const auto& m : modes) c.ufcs[c.ufcs_n++] = m; return c;
}
struct PlanRow { int mv; int ma; ct::Verdict v; ct::Reason r; };
static std::vector<PlanRow> rows_of(const ct::Runner& r, A a) {
    std::vector<PlanRow> out;
    for (const auto& s : r.steps()) if (s.algorithm == a) out.push_back({s.mv, s.ma, s.verdict, s.reason});
    return out;
}
static void run_unit(Fake& f, ct::Runner& r, A a, int cap_mv = 0) {
    if (cap_mv) f.in.cap_mv = cap_mv;
    r.start(f.caps, {.selected = ct::bit(a)}, f.in, true);
    f.tick(r, 120000);
    CHECK_EQ(r.state(), ct::State::Done);
}
static void test_scp_rows_follow_the_reported_capabilities() {
    {   // 20 V 6 A adapter: the top level joins the plan, no row asks above what it reported
        Fake f; f.caps = {}; const auto caps = scp_caps_of(3300, 20000, 1000, 6000); f.serve_caps = &caps;
        ct::Runner r(f); run_unit(f, r, A::Scp);
        const auto rows = rows_of(r, A::Scp);
        CHECK_EQ(rows.size(), 5u);
        CHECK_EQ(rows[0].mv, 5000); CHECK_EQ(rows[0].ma, 0);
        CHECK_EQ(rows[1].mv, 10000); CHECK_EQ(rows[1].ma, 2250);
        CHECK_EQ(rows[2].mv, 20000); CHECK_EQ(rows[2].ma, 2250);
        CHECK_EQ(rows[3].mv, 4500); CHECK_EQ(rows[3].ma, 5000);
        CHECK_EQ(rows[4].mv, 5000); CHECK_EQ(rows[4].ma, 4500);
        for (const auto& s : rows) CHECK_EQ(s.v, ct::Verdict::Pass);
        for (int ma : f.fc_mas) CHECK_EQ(ma <= 6000, true);
        for (int mv : f.fc_mvs) CHECK_EQ(mv <= 20000, true);
    }
    {   // 12 V 2.5 A port (a USB-A port, hardware dump): the 5 A direct-charge rows are not offered, so they are never asked
        Fake f; f.caps = {}; const auto caps = scp_caps_of(3300, 12000, 1000, 2500); f.serve_caps = &caps;
        ct::Runner r(f); run_unit(f, r, A::Scp);
        const auto rows = rows_of(r, A::Scp);
        CHECK_EQ(rows.size(), 5u);
        CHECK_EQ(rows[1].mv, 10000); CHECK_EQ(rows[1].v, ct::Verdict::Pass);
        CHECK_EQ(rows[2].mv, 12000); CHECK_EQ(rows[2].ma, 2250); CHECK_EQ(rows[2].v, ct::Verdict::Pass);
        CHECK_EQ(rows[3].mv, 4500); CHECK_EQ(rows[3].v, ct::Verdict::Skip); CHECK_EQ(rows[3].r, ct::Reason::NoOffer);
        CHECK_EQ(rows[4].mv, 5000); CHECK_EQ(rows[4].v, ct::Verdict::Skip); CHECK_EQ(rows[4].r, ct::Reason::NoOffer);
        for (int ma : f.fc_mas) CHECK_EQ(ma <= 2500, true);
    }
    {   // 5.5 V 5 A port: the 10 V row is not offered; the top level is 5.5 V
        Fake f; f.caps = {}; const auto caps = scp_caps_of(3400, 5500, 1000, 5000); f.serve_caps = &caps;
        ct::Runner r(f); run_unit(f, r, A::Scp);
        const auto rows = rows_of(r, A::Scp);
        CHECK_EQ(rows.size(), 5u);
        CHECK_EQ(rows[1].mv, 10000); CHECK_EQ(rows[1].v, ct::Verdict::Skip); CHECK_EQ(rows[1].r, ct::Reason::NoOffer);
        CHECK_EQ(rows[2].mv, 5500); CHECK_EQ(rows[2].v, ct::Verdict::Pass);
        CHECK_EQ(rows[3].v, ct::Verdict::Pass); CHECK_EQ(rows[4].v, ct::Verdict::Pass);
        for (int mv : f.fc_mvs) CHECK_EQ(mv <= 5500, true);
    }
    {   // the monitor ceiling clips the top level
        Fake f; f.caps = {}; const auto caps = scp_caps_of(3300, 20000, 1000, 6000); f.serve_caps = &caps;
        ct::Runner r(f); run_unit(f, r, A::Scp, 15000);
        const auto rows = rows_of(r, A::Scp);
        CHECK_EQ(rows.size(), 5u);
        CHECK_EQ(rows[2].mv, 15000); CHECK_EQ(rows[2].v, ct::Verdict::Pass);
    }
    {   // nothing read (the adapter answered no limits): the fixed rows stay
        Fake f; f.caps = {};
        ct::Runner r(f); run_unit(f, r, A::Scp);
        CHECK_EQ(rows_of(r, A::Scp).size(), 4u);
        CHECK_EQ(f.plan_changes, 0);
    }
    {   // a list left in the engine before the run is not this charger's: START plans the fixed rows
        Fake f; f.caps = {}; const auto stale = scp_caps_of(3300, 5500, 1000, 1500); f.in.fc_caps = &stale;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Scp)}, f.in, true);
        CHECK_EQ(rows_of(r, A::Scp).size(), 4u);
        CHECK_EQ(rows_of(r, A::Scp)[1].v, ct::Verdict::Pending);
    }
}
static void test_scp_replan_is_announced_once() {
    Fake f; f.caps = {}; const auto caps = scp_caps_of(3300, 12000, 1000, 2500); f.serve_caps = &caps;
    ct::Runner r(f); run_unit(f, r, A::Scp);
    CHECK_EQ(f.plan_changes, 1);   // the 4 rows became 5: one announcement, at the first row's end
}
static void test_ufcs_levels_follow_the_reported_modes() {
    {   // two modes: bottom / middle / top of each, a level shared by two modes once, 1 A, then the step back to 5 V
        Fake f; f.caps = {};
        const auto caps = ufcs_caps_of({ufcs_mode(1, 3400, 11000, 1000, 4000), ufcs_mode(2, 11000, 21000, 1000, 3000)}); f.serve_caps = &caps;
        ct::Runner r(f); run_unit(f, r, A::Ufcs);
        const auto rows = rows_of(r, A::Ufcs);
        const int want[] = {5000, 3400, 7200, 11000, 16000, 21000, 5000};
        CHECK_EQ(rows.size(), std::size(want));
        for (size_t n = 0; n < rows.size() && n < std::size(want); ++n) {
            CHECK_EQ(rows[n].mv, want[n]);
            CHECK_EQ(rows[n].v, ct::Verdict::Pass);
            CHECK_EQ(rows[n].ma, n == 0 ? 0 : 1000);
        }
        for (const auto& s : r.steps()) if (s.algorithm == A::Ufcs && &s != &r.steps()[0]) CHECK_EQ(s.continuous, true);
        for (int ma : f.fc_mas) CHECK_EQ(ma <= 1000, true);
        CHECK_EQ(f.plan_changes, 1);
    }
    {   // the 30 V band when the monitor reaches it; the top is clipped to the ceiling
        Fake f; f.caps = {};
        const auto caps = ufcs_caps_of({ufcs_mode(1, 3400, 12000, 1000, 3000), ufcs_mode(2, 21000, 36000, 1000, 3000)}); f.serve_caps = &caps;
        ct::Runner r(f); run_unit(f, r, A::Ufcs, 28000);
        const auto rows = rows_of(r, A::Ufcs);
        const int want[] = {5000, 3400, 7700, 12000, 21000, 24500, 28000, 5000};
        CHECK_EQ(rows.size(), std::size(want));
        for (size_t n = 0; n < rows.size() && n < std::size(want); ++n) { CHECK_EQ(rows[n].mv, want[n]); CHECK_EQ(rows[n].v, ct::Verdict::Pass); }
        for (int mv : f.fc_mvs) CHECK_EQ(mv <= 28000, true);
    }
    {   // a monitor that stops below the band: one named, skipped row for the band, never requested
        Fake f; f.caps = {};
        const auto caps = ufcs_caps_of({ufcs_mode(1, 3400, 12000, 1000, 3000), ufcs_mode(2, 21000, 36000, 1000, 3000)}); f.serve_caps = &caps;
        ct::Runner r(f); run_unit(f, r, A::Ufcs, 20000);
        const auto rows = rows_of(r, A::Ufcs);
        size_t over = 0;
        for (const auto& s : rows) {
            if (s.mv >= 21000) { ++over; CHECK_EQ(s.v, ct::Verdict::Skip); CHECK_EQ(s.r, ct::Reason::OverCap); }
        }
        CHECK_EQ(over, 1u);
        for (int mv : f.fc_mvs) CHECK_EQ(mv <= 12000, true);
    }
    {   // a 20 V cable: the 21 V+ band is one CableLimit row
        Fake f; f.caps = {};
        const auto caps = ufcs_caps_of({ufcs_mode(1, 3400, 12000, 1000, 3000), ufcs_mode(2, 21000, 36000, 1000, 3000)}); f.serve_caps = &caps;
        f.in.cable_mv = 20000; f.in.cable_ma = 3000;
        ct::Runner r(f); run_unit(f, r, A::Ufcs);
        size_t limited = 0;
        for (const auto& s : rows_of(r, A::Ufcs)) if (s.mv >= 21000) { ++limited; CHECK_EQ(s.r, ct::Reason::CableLimit); }
        CHECK_EQ(limited, 1u);
        for (int mv : f.fc_mvs) CHECK_EQ(mv <= 12000, true);
    }
    {   // a mode whose smallest current is above 1 A asks that current, never less than it offers
        Fake f; f.caps = {};
        const auto caps = ufcs_caps_of({ufcs_mode(1, 5000, 9000, 1500, 3000)}); f.serve_caps = &caps;
        ct::Runner r(f); run_unit(f, r, A::Ufcs);
        for (const auto& s : rows_of(r, A::Ufcs)) CHECK_EQ(s.ma == 0 || s.ma >= 1500, true);
    }
    {   // no list read: the fixed rows stay; a stale list at START is not used
        Fake f; f.caps = {};
        ct::Runner r(f); run_unit(f, r, A::Ufcs);
        CHECK_EQ(rows_of(r, A::Ufcs).size(), 6u);
        CHECK_EQ(f.plan_changes, 0);
        Fake g; g.caps = {}; const auto stale = ufcs_caps_of({ufcs_mode(1, 3400, 5500, 1000, 2000)}); g.in.fc_caps = &stale;
        ct::Runner s(g);
        s.start(g.caps, {.selected = ct::bit(A::Ufcs)}, g.in, true);
        CHECK_EQ(rows_of(s, A::Ufcs).size(), 6u);
    }
}
// Every UFCS row planned from the charger's modes names the mode it samples (Step::pdo, packed by pack_ufcs_mode), the 5 V baseline
// and the step back to 5 V included (the mode that reaches 5 V): the Results chip, the Running cell and the Detail page read it.
static void test_ufcs_steps_carry_their_mode() {
    {
        Fake f; f.caps = {};
        const auto caps = ufcs_caps_of({ufcs_mode(1, 3400, 11000, 1000, 4000), ufcs_mode(2, 11000, 21000, 1000, 3000)}); f.serve_caps = &caps;
        ct::Runner r(f); run_unit(f, r, A::Ufcs);
        // 5 V (home mode 1), 3.4 / 7.2 / 11 V (mode 1; 11 V is shared and belongs to the first), 16 / 21 V (mode 2), the step back
        const unsigned want_max_ma[] = {4000, 4000, 4000, 4000, 3000, 3000, 4000};
        const unsigned want_max_mv[] = {11000, 11000, 11000, 11000, 21000, 21000, 11000};
        size_t n = 0;
        for (const auto& s : r.steps()) {
            if (s.algorithm != A::Ufcs) continue;
            ct::UfcsMode m;
            CHECK_EQ(ct::ufcs_mode_of(s, m), true);
            if (n < std::size(want_max_ma)) { CHECK_EQ(m.max_ma, want_max_ma[n]); CHECK_EQ(m.max_mv, want_max_mv[n]); CHECK_EQ(m.min_ma, 1000u); }
            ++n;
        }
        CHECK_EQ(n, std::size(want_max_ma));
        CHECK_EQ(f.plan_changes, 1);
    }
    {   // a mode that does not reach 5 V: the baseline and the step back read the lowest mode
        Fake f; f.caps = {};
        const auto caps = ufcs_caps_of({ufcs_mode(1, 5500, 11000, 1000, 4000)}); f.serve_caps = &caps;
        ct::Runner r(f); run_unit(f, r, A::Ufcs);
        size_t n = 0;
        for (const auto& s : r.steps()) {
            if (s.algorithm != A::Ufcs) continue;
            ct::UfcsMode m;
            CHECK_EQ(ct::ufcs_mode_of(s, m), true); CHECK_EQ(m.max_ma, 4000u); CHECK_EQ(m.min_mv, 5500u);
            ++n;
        }
        CHECK_EQ(n > 3, true);
    }
    {   // nothing read: the fixed rows carry no mode
        Fake f; f.caps = {};
        ct::Runner r(f); run_unit(f, r, A::Ufcs);
        for (const auto& s : r.steps()) { ct::UfcsMode m; if (s.algorithm == A::Ufcs) CHECK_EQ(ct::ufcs_mode_of(s, m), false); }
    }
}
static void test_epr_physical_guard_and_spoof_separation() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        Fake f; f.caps.num_pdos = 8; f.caps.epr = true;
        f.caps.pdos[7] = pd::decode_pdo((360u << 10) | 350); // EPR mode even below 20V
        if (mode == 0) f.in.cable_observed = false;
        if (mode == 1) f.in.cable_ma = 3000;
        if (mode == 2) f.in.cable_mv = 20000;
        f.in.spoof_on = true;
        ct::Runner r(f); r.start(f.caps, {.selected = ct::bit(A::Epr)}, f.in, true);
        if (mode < 3) f.tick(r, 1200); else CHECK_EQ(run_until(f, r, [&] { return f.pd_requests >= 1; }), true);
        if (mode < 3) {
            CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Skip); CHECK_EQ(r.steps()[0].reason, ct::Reason::CableLimit);
            CHECK_EQ(f.pd_requests + f.fc_requests + f.releases, 0);
            // The unread cable stays 20 V and never unlocks EPR; its current is read off the offers (the list has 3.25 A PDOs).
            if (!mode) { CHECK_EQ(r.evidence().cable_mv, 20000); CHECK_EQ(r.evidence().cable_ma, 5000); }
        } else {
            CHECK_EQ(f.pd_requests, 1); CHECK_EQ(f.in.pd.epr_mode, true);
            f.in.cable_ma = 3000; f.tick(r, 100);   // the cable now reads 3 A: not an EPR cable
            f.tick(r, 100000);   // a cable-limit row is skipped, never a reason to stop the run
            CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Skip); CHECK_EQ(r.steps()[0].reason, ct::Reason::CableLimit);
            CHECK_EQ(r.state(), ct::State::Done); CHECK_EQ(f.releases >= 1, true);
            CHECK_EQ(0, 0); CHECK_EQ(f.pd_requests, 1);
        }
    }
}
static void test_epr_rows_respect_the_36v_ceiling() {
    Fake f; f.in.cap_mv = 36000; f.in.cable_mv = 50000; f.in.cable_ma = 5000; f.in.cable_observed = true;
    f.caps.epr = true; f.caps.num_pdos = 10;
    for (unsigned i = 0; i < 7; ++i) f.caps.pdos[i] = pd::decode_pdo(i ? 0 : (100u << 10) | 300);
    f.caps.pdos[7] = pd::decode_pdo((560u << 10) | 300);   // EPR 28 V 5 A
    f.caps.pdos[8] = pd::decode_pdo((960u << 10) | 300);   // EPR 48 V: above the INA226 ceiling
    f.caps.pdos[9] = pd::decode_pdo(0xD0000000 | (1440u << 17) | (150u << 8) | 140);   // EPR AVS 15..72 V
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Epr) | ct::bit(A::EprAvs)}, f.in, true);
    f.tick(r, 60000);
    unsigned ran = 0;
    for (const auto& s : r.steps()) {
        if (s.verdict != ct::Verdict::Skip) { ++ran; CHECK_EQ(s.mv <= 36000, true); }
        else if (s.algorithm == A::Epr && s.mv == 48000) CHECK_EQ(s.reason, ct::Reason::OverCap);
    }
    CHECK_EQ(ran >= 2, true);
    for (const auto& c : f.choices) CHECK_EQ(c.mv <= 36000, true);
    CHECK_EQ(f.in.pd.epr_mode, false);   // the run ends with EPR exited
}
// One row per EPR fixed level the source offers (28, 36, 48 V); the monitor ceiling is per device (INA226 36 V, INA228/237 48 V)
// and a level above it is skipped, never requested. A level at the ceiling runs (36 V on a 36 V monitor, 48 V on a 48 V one).
static void test_epr_levels_follow_the_monitor_ceiling() {
    struct Case { unsigned cap; bool run28, run36, run48; };
    for (const Case c : {Case{36000, true, true, false}, Case{48000, true, true, true}}) {
        Fake f; f.in.cap_mv = c.cap; f.in.cable_mv = 50000; f.in.cable_ma = 5000; f.in.cable_observed = true;
        f.caps.epr = true; f.caps.num_pdos = 10;
        f.caps.pdos[7] = pd::decode_pdo((560u << 10) | 500);   // EPR 28 V 5 A
        f.caps.pdos[8] = pd::decode_pdo((720u << 10) | 500);   // EPR 36 V 5 A
        f.caps.pdos[9] = pd::decode_pdo((960u << 10) | 500);   // EPR 48 V 5 A
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Epr)}, f.in, true);
        f.tick(r, 90000);
        unsigned seen = 0;
        for (const auto& s : r.steps()) {
            if (s.algorithm != A::Epr) continue;
            const bool want = s.mv == 28000 ? c.run28 : s.mv == 36000 ? c.run36 : c.run48;
            ++seen;
            CHECK_EQ(s.verdict == ct::Verdict::Pass, want);
            if (!want) {
                CHECK_EQ(s.verdict == ct::Verdict::Skip, true);
                CHECK_EQ(s.reason, ct::Reason::OverCap);
                CHECK_EQ(s.detail, c.cap == 36000 ? ct::kDetailMonitor36 : ct::kDetailMonitor48);
                CHECK_STR(ct::explanation(s), c.cap == 36000 ? "EPR: above K2's 36V monitor" : "EPR: above K2's 48V monitor");
            }
        }
        CHECK_EQ(seen, 3);
        for (const auto& ch : f.choices) CHECK_EQ(ch.mv <= c.cap, true);
    }
}
// The OverCap abort is the monitor ceiling + 5 % (pd::overcap_abort_mv: INA226 37.8 V, INA228/237 50.4 V); what the run may REQUEST
// stays the nominal ceiling. A source that sits a few hundred mV above its top level is in tolerance, not a fault.
static void test_overcap_abort_is_the_monitor_ceiling_plus_5_percent() {
    struct Case { unsigned cap, level, over_mv; bool abort; };
    for (const Case c : {Case{36000, 36000, 36300, false}, Case{36000, 36000, 37700, false}, Case{36000, 36000, 37900, true},
                         Case{48000, 48000, 50300, false}, Case{48000, 48000, 50500, true}}) {
        CHECK_EQ(pd::overcap_abort_mv(c.cap), c.cap == 36000 ? 37800u : 50400u);
        Fake f; f.in.cap_mv = c.cap; f.in.cable_mv = 50000; f.in.cable_ma = 5000; f.in.cable_observed = true;
        f.caps.epr = true; f.caps.num_pdos = 10;
        f.caps.pdos[7] = pd::decode_pdo((560u << 10) | 500);                   // EPR 28 V 5 A
        f.caps.pdos[8] = pd::decode_pdo((720u << 10) | 500);                   // EPR 36 V 5 A
        f.caps.pdos[9] = pd::decode_pdo((960u << 10) | 500);                   // EPR 48 V 5 A
        f.pd_fold_from_mv = c.level; f.pd_fold_to_mv = c.over_mv;              // the top level delivers over_mv
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Epr)}, f.in, true);
        f.tick(r, 90000);
        bool seen = false;
        for (const auto& s : r.steps()) {
            if (s.algorithm != A::Epr || s.mv != c.level) continue;
            seen = true;
            if (c.abort) { CHECK_EQ(s.verdict, ct::Verdict::Fail); CHECK_EQ(s.reason, ct::Reason::OverCap); }
            else CHECK_EQ(s.verdict, ct::Verdict::Pass);
        }
        CHECK_EQ(seen, true);
        for (const auto& ch : f.choices) CHECK_EQ(ch.mv <= c.cap, true);       // requests stay at the nominal ceiling
        if (!c.abort) CHECK_EQ(r.state(), ct::State::Done);
    }
}
static void test_epr_entry_before_rows() {
    for (unsigned mode = 0; mode < 8; ++mode) {
        Fake f;
        f.caps.pdos[0] = pd::decode_pdo(f.caps.pdos[0].raw | (1u << 23));   // EPR Mode Capable, SPR list only
        if (mode == 2) f.in.cable_ma = 3000;                                 // cable not EPR
        // Mode 6/7: the cable was never read (START skipped the scan, Rp on the line): the source checks the cable at entry
        if (mode >= 6) { f.in.cable_observed = false; f.in.cable_mv = 20000; f.in.cable_ma = 3000; }
        f.epr_refuse_code = mode == 1 ? 3 : mode == 7 ? 1 : 0;
        if (mode == 3) f.caps.pdos[0] = pd::decode_pdo(f.caps.pdos[0].raw & ~(1u << 23));   // not capable
        f.in.pd.state = pd::SinkState::Ready; f.in.pd.mv = 5000;
        f.no_contract = mode == 5; f.contract_delay_ms = mode == 4 ? 1500 : 0;   // mode 4: the contract is late, mode 5: never
        f.epr_refuse = mode == 1 || mode == 7;
        f.epr_caps = f.caps; f.epr_caps.epr = true; f.epr_caps.num_pdos = 10;
        f.epr_caps.pdos[7] = pd::decode_pdo((560u << 10) | 300);
        f.epr_caps.pdos[8] = pd::decode_pdo((720u << 10) | 300);   // 36 V
        f.epr_caps.pdos[9] = pd::decode_pdo(0xD0000000 | (480u << 17) | (150u << 8) | 140);
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Epr) | ct::bit(A::EprAvs)}, f.in, true);
        f.tick(r, 90000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(f.in.pd.epr_mode, false);
        unsigned pass = 0; ct::Reason why = ct::Reason::None;
        for (const auto& st : r.steps()) { pass += st.verdict == ct::Verdict::Pass; if (st.verdict == ct::Verdict::Skip) why = st.reason; }
        if (mode == 0 || mode == 4 || mode == 6) {
            CHECK_EQ(f.choices[0].pos, 0);
            CHECK_EQ(pass >= 2, true);
            for (const auto& c : f.choices) CHECK_EQ(c.mv <= 36000, true);
            CHECK_EQ(r.evidence().flags & ct::EprTable, ct::EprTable);
            // 28 V and 36 V (at the 36 V monitor ceiling) pass
            for (const auto& st : r.steps()) {
                if (st.algorithm != A::Epr) continue;
                if (st.mv == 28000 || st.mv == 36000) CHECK_EQ(st.verdict == ct::Verdict::Pass, true);
            }
        } else {
            CHECK_EQ(pass, 0);
            CHECK_EQ(why, (mode == 1 || mode == 7) ? ct::Reason::EprRefused : mode == 2 ? ct::Reason::CableLimit : mode == 5 ? ct::Reason::NoContract : ct::Reason::NoOffer);
            CHECK_EQ(f.pd_requests, (mode == 1 || mode == 7) ? 1 : 0);
            for (const auto& st : r.steps()) {
                if (st.algorithm != A::Epr && st.algorithm != A::EprAvs) continue;
                CHECK_EQ(st.verdict == ct::Verdict::Skip, true);
                if (mode == 1) { CHECK_EQ(st.detail, ct::kDetailEprFail + 3); CHECK_STR(ct::explanation(st), "EPR: refused (RDO)"); }
                if (mode == 7) { CHECK_EQ(st.detail, ct::kDetailEprFail + 1); CHECK_STR(ct::explanation(st), "EPR: source rejects cable"); }
                if (mode == 2) CHECK_STR(ct::explanation(st), "EPR: cable not 50V 5A");
                if (mode == 3) CHECK_STR(ct::explanation(st), "EPR: not offered");
                if (mode == 5) CHECK_STR(ct::explanation(st), "EPR: no PD contract");
            }
        }
    }
}
// An EPR-capable source (EPR bit in PDO 1) whose cable cannot carry EPR: the sink asks EPR_Get_Source_Cap in SPR mode (no entry,
// PD R3.2 6.5.17.1) and the EPR group reports what the charger offers, "EPR offered: needs EPR cable", per level.
static Fake epr_capable_fake() {
    Fake f;
    f.caps.pdos[0] = pd::decode_pdo(f.caps.pdos[0].raw | (1u << 23));
    f.in.pd.state = pd::SinkState::Ready; f.in.pd.mv = 5000;
    f.info_list = f.caps; f.info_list.epr = true; f.info_list.num_pdos = 11;
    f.info_list.pdos[7] = pd::decode_pdo((560u << 10) | 500);   // 28 V 5 A
    f.info_list.pdos[8] = pd::decode_pdo((720u << 10) | 500);   // 36 V 5 A
    f.info_list.pdos[9] = pd::decode_pdo((960u << 10) | 500);   // 48 V 5 A
    f.info_list.pdos[10] = pd::decode_pdo(0xD0000000 | (1440u << 17) | (150u << 8) | 140);   // EPR AVS 15..72 V
    return f;
}
static void test_epr_info_names_the_levels_without_an_epr_cable() {
    Fake f = epr_capable_fake();
    f.in.cable_ma = 3000;   // a read cable that is not 50 V / 5 A: no entry is attempted
    f.info_mode = 1;
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Epr) | ct::bit(A::EprAvs)}, f.in, true);
    f.tick(r, 90000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.info_asks, 1);
    CHECK_EQ(f.pd_requests, 0);   // the list is informational: never an EPR entry or an EPR_Request
    unsigned mv28 = 0, mv36 = 0, mv48 = 0, avs = 0, rows = 0;
    for (const auto& s : r.steps()) {
        if (s.algorithm != A::Epr && s.algorithm != A::EprAvs) continue;
        ++rows;
        CHECK_EQ(s.verdict == ct::Verdict::Skip, true);
        CHECK_EQ(s.reason, ct::Reason::CableLimit);
        CHECK_EQ(s.detail, ct::kDetailEprOffered);
        CHECK_STR(ct::explanation(s), "EPR offered: needs EPR cable");
        mv28 += s.algorithm == A::Epr && s.mv == 28000 && s.pos == 8;
        mv36 += s.algorithm == A::Epr && s.mv == 36000 && s.pos == 9;
        mv48 += s.algorithm == A::Epr && s.mv == 48000 && s.pos == 10;
        avs += s.algorithm == A::EprAvs && s.avs && s.pos == 11;
    }
    CHECK_EQ(rows, 4); CHECK_EQ(mv28, 1); CHECK_EQ(mv36, 1); CHECK_EQ(mv48, 1); CHECK_EQ(avs, 1);
    CHECK_EQ(f.in.pd.epr_mode, false);
}
static void test_epr_info_refused_or_silent_changes_nothing() {
    for (int mode : {0, 2, 3}) {   // cannot ask / Not_Supported / no answer: the old wording, one row per EPR algorithm
        Fake f = epr_capable_fake();
        f.in.cable_ma = 3000; f.info_mode = mode;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Epr) | ct::bit(A::EprAvs)}, f.in, true);
        f.tick(r, 90000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(f.info_asks, 1); CHECK_EQ(f.pd_requests, 0);
        unsigned rows = 0;
        for (const auto& s : r.steps()) {
            if (s.algorithm != A::Epr && s.algorithm != A::EprAvs) continue;
            ++rows;
            CHECK_EQ(s.reason, ct::Reason::CableLimit); CHECK_EQ(s.detail, 0);
            CHECK_STR(ct::explanation(s), "EPR: cable not 50V 5A");
        }
        CHECK_EQ(rows, 2);
    }
}
// The cable was never read: the source's own cable check answers the entry. Enter_Failed data 1 (cable) with the info list in
// hand keeps every level the charger offers (detail 147 "EPR offered: needs EPR cable") and adds the source's own Enter_Failed
// answer to the same row (detail 149 + data, "EPR offered; entry: ..."): the refused entry must not overwrite what the charger
// said it can do (Baseus 140W, 2026-11-05: 28 V 5 A listed, entry refused with Enter_Failed data 2). One verdict per level is all a
// card holds, so the two facts share one detail code.
static void test_epr_info_levels_survive_a_refused_entry() {
    Fake f = epr_capable_fake();
    f.in.cable_observed = false; f.in.cable_mv = 20000; f.in.cable_ma = 3000;
    f.epr_refuse = true; f.epr_refuse_code = 1; f.info_mode = 1; f.info_busy = 2;
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Epr) | ct::bit(A::EprAvs)}, f.in, true);
    f.tick(r, 90000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.info_asks, 3);   // two Busy answers, then the ask went out
    CHECK_EQ(f.pd_requests, 1);   // the entry attempt
    unsigned levels = 0, rows = 0;
    for (const auto& s : r.steps()) {
        if (s.algorithm != A::Epr && s.algorithm != A::EprAvs) continue;
        ++rows; levels += s.mv == 28000 || s.mv == 36000 || s.mv == 48000;
        CHECK_EQ(s.verdict == ct::Verdict::Skip, true);
        CHECK_EQ(s.reason, ct::Reason::CableLimit); CHECK_EQ(s.detail, ct::kDetailEprOfferedRefused + 1);
        CHECK_STR(ct::explanation(s), "EPR offered; entry: cable rejected");
    }
    CHECK_EQ(rows, 4); CHECK_EQ(levels, 3);   // every listed level and the AVS range survive, none was rewritten
    unsigned recorded = 0;   // the card is fed from the recorded rows
    for (const auto& s : f.records)
        recorded += (s.algorithm == A::Epr || s.algorithm == A::EprAvs) && s.detail == ct::kDetailEprOfferedRefused + 1;
    CHECK_EQ(recorded >= 4, true);
}
// Baseus 140W on the TOP plug, 2026-11-05: the answer to the SPR-mode ask is 32 B, six SPR PDOs, an all-zero PDO 7 (padding) and
// 28 V 5 A at position 8. The zero word is no offer: it makes no row, the 28 V level is named.
static void test_epr_info_padding_word_is_no_offer() {
    Fake f = epr_capable_fake();
    f.info_list.num_pdos = 8;
    for (unsigned i = 6; i < f.info_list.pdos.size(); ++i) f.info_list.pdos[i] = pd::decode_pdo(0);
    f.info_list.pdos[7] = pd::decode_pdo(0x0008C1F4);   // 28 V 5 A
    f.in.cable_ma = 3000; f.info_mode = 1;
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Epr)}, f.in, true);
    f.tick(r, 90000);
    unsigned rows = 0;
    for (const auto& s : r.steps()) {
        if (s.algorithm != A::Epr) continue;
        ++rows;
        CHECK_EQ(s.pos, 8); CHECK_EQ(s.mv, 28000); CHECK_EQ(s.detail, ct::kDetailEprOffered);
    }
    CHECK_EQ(rows, 1);
}
// A synthetic answer with no EPR PDO at all from a source whose PDO 1 has the EPR bit: the EPR rows say the list is withheld, then
// the entry's own Enter_Failed wording stays as it was.
static void test_epr_info_with_no_epr_pdos_is_named_withheld() {
    Fake f = epr_capable_fake();
    f.info_list.num_pdos = 8;   // 6 SPR words + two padding words
    for (unsigned i = 6; i < f.info_list.pdos.size(); ++i) f.info_list.pdos[i] = pd::decode_pdo(0);
    f.in.cable_observed = false; f.in.cable_mv = 20000; f.in.cable_ma = 3000;
    f.epr_refuse = true; f.epr_refuse_code = 2; f.info_mode = 1;
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Epr) | ct::bit(A::EprAvs)}, f.in, true);
    f.tick(r, 90000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.info_asks, 1);
    unsigned withheld = 0;
    for (const auto& s : f.records) {
        if (s.algorithm != A::Epr && s.algorithm != A::EprAvs) continue;
        if (s.detail != ct::kDetailEprWithheld) continue;
        ++withheld;
        CHECK_EQ(s.reason, ct::Reason::CableLimit);
        CHECK_STR(ct::explanation(s), "EPR list withheld: cable-gated");
    }
    CHECK_EQ(withheld >= 2, true);
    for (const auto& s : r.steps()) {   // the final rows keep the source's own Enter_Failed word
        if (s.algorithm != A::Epr && s.algorithm != A::EprAvs) continue;
        CHECK_EQ(s.reason, ct::Reason::EprRefused); CHECK_EQ(s.detail, ct::kDetailEprFail + 2);
        CHECK_STR(ct::explanation(s), "EPR: source has no VCONN");
    }
}
// With a fit cable the info list is not asked at all (the entry itself yields the EPR list): the old flow, unchanged.
static void test_epr_info_is_not_asked_when_entry_works() {
    Fake f = epr_capable_fake();
    f.epr_caps = f.info_list; f.info_mode = 1;
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Epr)}, f.in, true);
    f.tick(r, 90000);
    CHECK_EQ(f.info_asks, 0); CHECK_EQ(f.choices.empty() ? 99 : f.choices[0].pos, 0);
    unsigned pass = 0;
    for (const auto& s : r.steps()) pass += s.algorithm == A::Epr && s.verdict == ct::Verdict::Pass;
    CHECK_EQ(pass >= 2, true);
}
// Baseus 140W, 2026-11-04: the 5 V row ran in EPR mode, its release ended with EPR_Mode Exit, the SPR list came back and every
// remaining row (SPR and EPR) was skipped "offers changed". The rows after an exit run; EPR rows re-enter EPR.
static void test_rows_survive_the_exit_to_the_spr_list() {
    Fake f;
    f.caps.pdos[0] = pd::decode_pdo(f.caps.pdos[0].raw | (1u << 23));
    f.in.pd.state = pd::SinkState::Ready; f.in.pd.mv = 5000;
    f.epr_caps = f.caps; f.epr_caps.epr = true; f.epr_caps.num_pdos = 9;
    f.epr_caps.pdos[7] = pd::decode_pdo((560u << 10) | 300);   // 28 V
    f.epr_caps.pdos[8] = pd::decode_pdo((720u << 10) | 300);   // 36 V
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Pd3Fixed) | ct::bit(A::Epr)}, f.in, true);
    f.tick(r, 120000);
    CHECK_EQ(r.state(), ct::State::Done);
    unsigned pass = 0, changed = 0, epr_pass = 0;
    for (const auto& st : r.steps()) {
        pass += st.verdict == ct::Verdict::Pass;
        epr_pass += st.algorithm == A::Epr && st.verdict == ct::Verdict::Pass;
        changed += st.verdict == ct::Verdict::Skip && st.reason == ct::Reason::CapsChanged;
    }
    CHECK_EQ(changed, 0);
    CHECK_EQ(epr_pass, 2);
    CHECK_EQ(pass >= 5, true);
    CHECK_EQ(f.in.pd.epr_mode, false);
}
static void test_spr_pps_21v_on_standard_cable() {
    for (bool observed : {false, true}) {
        Fake f;
        f.in.cable_mv = 20000; f.in.cable_ma = observed ? 5000 : 3000;
        f.in.cable_observed = observed; f.in.spoof_on = true;
        const ct::Options options{.selected = ct::bit(A::Pps)};
        const auto preview = ct::Runner::preview(f.caps, options, f.in);
        CHECK_EQ(preview.max_mv, 21000);
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, options, f.in, true), ct::Reason::None);
        bool pending21 = false;
        for (const auto& s : r.steps()) {
            if (s.pos == 7 && s.mv == 21000) pending21 = s.verdict == ct::Verdict::Pending;
            if (s.verdict == ct::Verdict::Pending && !s.hunt) CHECK_EQ(s.mv <= 21000 && s.ma <= f.in.cable_ma, true);
        }
        CHECK_EQ(pending21, true);
        CHECK_EQ(r.evidence().cable_mv, 20000);
        f.tick(r, 200000);
        bool passed21 = false;
        for (const auto& s : r.steps())
            if (s.pos == 7 && s.mv == 21000) passed21 = s.verdict == ct::Verdict::Pass;
        CHECK_EQ(passed21, true);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(f.in.vbus_mv, 5000);
    }

    Fake limited;
    limited.in.cable_mv = 20000;
    limited.in.cable_observed = false;
    limited.in.cap_mv = 20000;
    ct::Runner monitor_limited(limited);
    monitor_limited.start(limited.caps, {.selected = ct::bit(A::Pps)}, limited.in, true);
    bool monitor_skip = false;
    for (const auto& s : monitor_limited.steps()) {
        if (s.pos == 7 && s.mv == 21000)
            monitor_skip = s.verdict == ct::Verdict::Skip && s.reason == ct::Reason::OverCap;
        if (s.verdict == ct::Verdict::Pending) CHECK_EQ(s.mv <= 20000, true);
    }
    CHECK_EQ(monitor_skip, true);

    Fake offers;
    offers.in.cable_mv = 20000;
    offers.in.cable_observed = false;
    offers.caps.pdos[4] = pd::decode_pdo(((21000u / 50) << 10) | 300);
    offers.caps.pdos[6] = pd::decode_pdo(0xc0000000 | (220u << 17) | (50u << 8) | 60);
    ct::Runner bounded(offers);
    bounded.start(offers.caps, kPd, offers.in, true);
    bool fixed_skip = false, above_pps_skip = false, pps21_pending = false;
    for (const auto& s : bounded.steps()) {
        if (s.algorithm == A::Pd3Fixed && s.mv == 21000)
            fixed_skip = s.verdict == ct::Verdict::Skip && s.reason == ct::Reason::CableLimit;
        if (s.algorithm == A::Pps && s.pos == 7 && s.mv == 22000)
            above_pps_skip = s.verdict == ct::Verdict::Skip && s.reason == ct::Reason::CableLimit;
        if (s.algorithm == A::Pps && s.pos == 7 && s.mv == 21000)
            pps21_pending = s.verdict == ct::Verdict::Pending;
    }
    CHECK_EQ(fixed_skip, true);
    CHECK_EQ(above_pps_skip, true);
    CHECK_EQ(pps21_pending, true);

    Fake lower;
    lower.in.cable_mv = 12000;
    ct::Runner lower_limited(lower);
    lower_limited.start(lower.caps, {.selected = ct::bit(A::Pps)}, lower.in, true);
    for (const auto& s : lower_limited.steps())
        if (s.verdict == ct::Verdict::Pending) CHECK_EQ(s.mv <= 12000, true);

    Fake fault;
    fault.in.cable_mv = 20000; fault.in.cable_observed = false;
    ct::Runner guarded(fault);
    guarded.start(fault.caps, {.selected = ct::bit(A::Pps)}, fault.in, true);
    for (unsigned n = 0; n < 1800 &&
            !(guarded.state() == ct::State::Holding && guarded.steps()[guarded.index()].mv == 21000); ++n)
        fault.tick(guarded, 100);
    CHECK_EQ(guarded.state(), ct::State::Holding);
    CHECK_EQ(guarded.steps()[guarded.index()].mv, 21000);
    fault.in.vbus_mv = 21020;   // a real source overshoots a 21 V PPS request by 20 mV: still in tolerance
    const auto row = guarded.index();
    fault.tick(guarded, 20);
    CHECK_EQ(guarded.steps()[row].verdict == ct::Verdict::Skip, false);
    fault.in.vbus_mv = 22100;
    fault.tick(guarded, 100);
    CHECK_EQ(guarded.steps()[row].verdict, ct::Verdict::Skip);
    CHECK_EQ(guarded.steps()[row].reason, ct::Reason::CableLimit);
    CHECK_EQ(fault.releases > 0, true);
    fault.tick(guarded, 300000);
    CHECK_EQ(guarded.state(), ct::State::Done);
}
static void test_standard_cable_measured_tolerance() {
    for (int mv : {20001, 21000, 21001}) {
        Fake f;
        f.in.cable_mv = 20000; f.in.cable_ma = 3000; f.in.cable_observed = false;
        f.caps.num_pdos = 5;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Pd3Fixed)}, f.in, true);
        for (unsigned n = 0; n < 200 && f.pd_requests < 5; ++n) f.tick(r, 100);
        CHECK_EQ(f.pd_requests, 5);
        CHECK_EQ(r.steps()[4].mv, 20000);
        f.in.vbus_mv = mv;
        f.tick(r, mv <= 21000 ? 1100 : 100);
        CHECK_EQ(r.steps()[4].verdict, mv <= 21000 ? ct::Verdict::Pass : ct::Verdict::Skip);
        if (mv > 21000) CHECK_EQ(r.steps()[4].reason, ct::Reason::CableLimit);
    }
    Fake limited;
    limited.in.cable_mv = 20000; limited.in.cable_observed = false; limited.in.cap_mv = 20000;
    limited.caps.num_pdos = 5;
    ct::Runner r(limited);
    r.start(limited.caps, {.selected = ct::bit(A::Pd3Fixed)}, limited.in, true);
    for (unsigned n = 0; n < 200 && limited.pd_requests < 5; ++n) limited.tick(r, 100);
    CHECK_EQ(limited.pd_requests, 5);
    limited.in.vbus_mv = 21001;   // the monitor ceiling + 5 % (20 V cap: 21 V)
    limited.tick(r, 100);
    CHECK_EQ(r.steps()[4].verdict, ct::Verdict::Fail);
    CHECK_EQ(r.steps()[4].reason, ct::Reason::OverCap);
}
static void test_observed_30v_cable() {
    for (int mv : {21013, 30001}) {
        Fake f;
        f.in.cable_mv = 30000; f.in.cable_ma = 5000; f.in.cable_observed = true;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Pps)}, f.in, true);
        for (unsigned n = 0; n < 300 && !(r.state() == ct::State::Requesting &&
                r.steps()[r.index()].pos == 7 && r.steps()[r.index()].mv == 21000); ++n)
            f.tick(r, 100);
        CHECK_EQ(r.state(), ct::State::Requesting);
        CHECK_EQ(r.steps()[r.index()].mv, 21000);
        CHECK_EQ(r.evidence().cable_mv, 30000);
        const auto index = r.index();
        f.in.vbus_mv = mv;
        f.tick(r, mv == 21013 ? 1100 : 100);
        CHECK_EQ(r.steps()[index].verdict, mv == 21013 ? ct::Verdict::Pass : ct::Verdict::Skip);
        if (mv == 30001) CHECK_EQ(r.steps()[index].reason, ct::Reason::CableLimit);
    }

    Fake epr;
    epr.in.cable_mv = 30000; epr.in.cable_ma = 5000; epr.in.cable_observed = true;
    epr.caps.epr = true; epr.caps.num_pdos = 8;
    epr.caps.pdos[7] = pd::decode_pdo((560u << 10) | 300);
    ct::Runner r(epr);
    r.start(epr.caps, {.selected = ct::bit(A::Epr)}, epr.in, true);
    epr.tick(r, 700);
    CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Skip);
    CHECK_EQ(r.steps()[0].reason, ct::Reason::CableLimit);
    CHECK_EQ(epr.pd_requests, 0);
}
static void test_avs_current_and_bad_offers() {
    Fake f; f.caps.num_pdos = 2;
    f.caps.pdos[1] = pd::decode_pdo(0xE0000000 | (50u << 10) | 200);
    ct::Runner r(f); r.start(f.caps, {.selected = ct::bit(A::SprAvs)}, f.in, true);
    CHECK_EQ(r.steps().size(), 5); CHECK_EQ(r.evidence().flags & ct::SprAvsOffer, ct::SprAvsOffer);
    for (const auto& s : r.steps()) CHECK_EQ(s.ma, s.mv <= 15000 ? 500 : 1000);
    f.tick(r, 170000);
    CHECK_EQ(f.pd_requests, 5);
    for (const auto& c : f.choices) CHECK_EQ(c.ma, c.mv <= 15000 ? 500 : 1000);

    Fake invalid; invalid.caps.num_pdos = 2;
    invalid.caps.pdos[1] = pd::decode_pdo(0xD0000000 | (200u << 17) | (150u << 8) | 100);
    ct::Runner ir(invalid); ir.start(invalid.caps, {.selected = ct::bit(A::SprAvs)}, invalid.in, true);
    invalid.tick(ir, 1000);
    CHECK_EQ(ir.steps()[0].reason, ct::Reason::UnsupportedOffer);
    CHECK_STR(ct::explanation(ir.steps()[0]), "EPR AVS in an SPR slot");
    CHECK_EQ(invalid.pd_requests + invalid.fc_requests + invalid.releases, 0);
}
// UGREEN X865 (2026-11-04): the attach's first list has 6 PDOs; the SPR AVS APDO comes with the re-sent list 350 ms later,
// in the same attach, AFTER the 5 V contract is Ready (100 ms) and inside the 500 ms baseline. The PD rows follow the list in
// hand until the first PD row starts.
static void test_pd_plan_follows_a_list_that_grows_during_the_attach() {
    Fake f; f.caps.num_pdos = 6;
    f.grown = f.caps; f.grown.num_pdos = 7; f.grown.pdos[6] = pd::decode_pdo(0xE004B145);
    f.grow_after_ms = 400; f.contract_delay_ms = 100;
    ct::Runner r(f);
    ct::Options o{.selected = ct::bit(A::Pd3Fixed) | ct::bit(A::SprAvs)}; o.units.add(ct::Unit::Pd);
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 170000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(r.evidence().flags & ct::SprAvsOffer, ct::SprAvsOffer);
    CHECK_EQ(r.evidence().num_pdos, 7);
    unsigned avs_pass = 0;
    for (const auto& s : r.steps()) avs_pass += s.algorithm == A::SprAvs && s.verdict == ct::Verdict::Pass;
    CHECK_EQ(avs_pass, 5u);
    for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pass, true);
}
static void test_reannounced_identical_caps_are_not_a_change() {
    for (unsigned gone_ms : {2000u, 12000u}) {
        Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
        for (unsigned n = 0; n < 80 && r.state() != ct::State::Releasing; ++n) f.tick(r, 100);
        CHECK_EQ(r.state(), ct::State::Releasing);
        f.caps.valid = false;                       // hard reset: the list is gone until the source announces again
        f.tick(r, gone_ms);
        if (gone_ms < 8000) { CHECK_EQ(r.reason() == ct::Reason::CapsChanged, false); f.caps.valid = true; f.tick(r, 300); CHECK_EQ(r.reason() == ct::Reason::CapsChanged, false); }
        else {   // never came back: the unit's remaining rows end as CapsChanged (a unit failure, not the run's reason)
            unsigned changed = 0;
            for (const auto& s : r.steps()) changed += s.reason == ct::Reason::CapsChanged;
            CHECK_EQ(changed > 0, true);
        }
    }
}
static void test_measured_dwell_and_plan_capacity() {
    Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    for (unsigned n = 0; n < 80 && r.state() != ct::State::Releasing; ++n) f.tick(r, 100);
    f.in.pd.held = true; f.tick(r, 700); CHECK_EQ(f.pd_requests, 1);
    f.in.pd.held = false; f.in.vbus_mv = 9000; f.tick(r, 300); CHECK_EQ(f.pd_requests, 1);
    f.in.vbus_mv = 5000; f.tick(r, 400); CHECK_EQ(f.pd_requests, 1);
    f.in.now += 1000; r.poll(f.in, f.caps); f.tick(r, 400); CHECK_EQ(f.pd_requests, 1);
    f.tick(r, 200); CHECK_EQ(f.pd_requests, 2);

    for (int mv : {5269, 5500, 5501}) {
        Fake released; ct::Runner rr(released); rr.start(released.caps, kPd, released.in, true);
        for (unsigned n = 0; n < 80 && rr.state() != ct::State::Releasing; ++n) released.tick(rr, 100);
        CHECK_EQ(rr.state(), ct::State::Releasing);
        released.in.vbus_mv = mv;
        released.tick(rr, 600);
        CHECK_EQ(released.pd_requests, mv <= 5500 ? 2 : 1);
    }

    Fake many; many.in.cap_mv = 15000; many.caps.num_pdos = 11; many.caps.epr = true;
    for (unsigned i = 0; i < 11; ++i) many.caps.pdos[i] = pd::decode_pdo(i < 7 ?
        0xE0000000 | (100u << 10) | 100 :
        0xD0000000 | (480u << 17) | (150u << 8) | 140);
    ct::Runner mr(many); const auto preview = ct::Runner::preview(many.caps, {}, many.in);
    mr.start(many.caps, {}, many.in, true);
    CHECK_EQ(preview.total, 110); CHECK_EQ(mr.steps().size(), preview.total);
    CHECK_EQ(mr.steps().size() <= ct::kMaxSteps, true);
    // Rows are laid out in the unit order: the vendor modes first (FCP first), the PD rows in the middle (Vivo before them), the
    // standard protocols behind them: the plan's tail is the last UFCS row, the last PD row sits before the QC rows.
    CHECK_EQ(mr.steps().back().group, ct::Group::Ufcs);
    CHECK_EQ(mr.steps().front().group, ct::Group::Fcp);
    size_t last_pd = 0, first_qc = mr.steps().size();
    for (size_t n = 0; n < mr.steps().size(); ++n) {
        if (mr.steps()[n].group == ct::Group::Pd) last_pd = n;
        if (mr.steps()[n].group == ct::Group::Qc2 && first_qc == mr.steps().size()) first_qc = n;
    }
    CHECK_EQ(mr.steps()[last_pd].algorithm, A::EprAvs); CHECK_EQ(mr.steps()[last_pd].verdict, ct::Verdict::Skip);
    CHECK_EQ(last_pd < first_qc, true);
}
static void test_vivo_plan_runs_the_engine() {
    Fake f; f.caps = {};
    ct::Runner r(f);
    const uint32_t only = ct::bit(A::Vivo);
    CHECK_EQ(r.start(f.caps, {.selected = only}, f.in, true), ct::Reason::None);
    const unsigned want[4] = {5000, 9000, 11000, 5000};
    CHECK_EQ(r.steps().size(), 4);
    for (unsigned i = 0; i < r.steps().size() && i < 4; ++i) {
        CHECK_EQ(r.steps()[i].algorithm, A::Vivo); CHECK_EQ(r.steps()[i].mv, want[i]);
        CHECK_EQ(r.steps()[i].verdict, ct::Verdict::Pending);
    }
    f.tick(r, 100000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.fc_requests, 4); CHECK_EQ(f.pd_requests, 0);   // VIVO 5 V rows are handshake proofs
    for (const auto p : f.protocols) CHECK_EQ(p, fc::Proto::Vivo);
    for (const auto& s : r.steps()) CHECK_EQ(s.reason == ct::Reason::NotImplemented, false);
}
static void test_pd_bundles() {
    Fake f; f.caps.epr = true; f.caps.num_pdos = 9;
    f.caps.pdos[7] = pd::decode_pdo((560u << 10) | 300);
    f.caps.pdos[8] = pd::decode_pdo(0xD0000000 | (480u << 17) | (150u << 8) | 140);
    for (const uint32_t mask : {ct::kPdFixedAlgorithms, ct::kPpsAlgorithms}) {
        ct::Runner r(f); r.start(f.caps, {.selected = mask}, f.in, true);
        unsigned epr = 0;
        for (const auto& s : r.steps()) {
            CHECK_EQ(bool(mask & ct::bit(s.algorithm)), true);
            CHECK_EQ(s.algorithm >= A::Pd30Revision && s.algorithm <= A::Pd32Revision, false);
            if (s.verdict != ct::Verdict::Skip)
                CHECK_EQ(pd::decode_pdo(s.pdo).type == pd::PdoType::Fixed, mask == ct::kPdFixedAlgorithms);
            epr += s.algorithm == A::Epr || s.algorithm == A::EprAvs;
        }
        CHECK_EQ(epr, mask == ct::kPdFixedAlgorithms ? 1 : 6);
    }
}
static void test_skip_while_held_releases_before_done() {
    // Dropping the monitor cap at every point of a PPS run must never end in Done while a contract is held.
    for (unsigned drop = 1000; drop < 12000; drop += 700) {
        Fake f; ct::Runner r(f);
        static constexpr ct::Options pps{.selected = ct::bit(A::Pps)};
        r.start(f.caps, pps, f.in, true);
        f.tick(r, drop);
        f.in.cap_mv = 9000;
        for (unsigned n = 0; n < 4000 && r.busy(); ++n) {
            f.tick(r, 100);
            // Done leaves the sink attached in PD mode: the source is back at 5 V once its attach delay is over.
            if (r.state() == ct::State::Done) { f.tick(r, 1000); CHECK_EQ(f.in.pd.held, false); CHECK_EQ(f.in.vbus_mv, 5000); }
        }
        CHECK_EQ(r.busy(), false);
        CHECK_EQ(f.in.pd.held, false);
    }
}
static void test_release_retried_until_sink_lets_go() {
    Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    for (unsigned n = 0; n < 150 && f.pd_requests < 2; ++n) f.tick(r, 100);
    f.release_ok = false; r.cancel(f.in);
    const int base = f.releases;
    f.tick(r, 2000);
    CHECK_EQ(f.releases - base >= 6, true);  // about every 250 ms while Releasing
    f.release_ok = true; f.tick(r, 1000);
    CHECK_EQ(r.state(), ct::State::Stopped);
    // Unsafe keeps retrying and recovers once the sink finally releases.
    Fake g; ct::Runner q(g); q.start(g.caps, kPd, g.in, true);
    for (unsigned n = 0; n < 150 && g.pd_requests < 2; ++n) g.tick(q, 100);
    g.release_ok = false; q.cancel(g.in); g.tick(q, 9000);
    CHECK_EQ(q.state(), ct::State::Unsafe);
    const int b2 = g.releases;
    g.tick(q, 1000);
    CHECK_EQ(g.releases - b2 >= 3, true);
    g.release_ok = true; g.tick(q, 1500);
    CHECK_EQ(q.state(), ct::State::Stopped);
}
static constexpr ct::Options kQc2{.selected = ct::bit(A::Qc2)};
static unsigned count_verdict_any(const ct::Runner& r, ct::Verdict v) {
    unsigned n = 0;
    for (const auto& s : r.steps()) n += s.verdict == v;
    return n;
}
static void test_fc_release_is_issued_once_per_row() {
    // Every fc release restarts the D+/D- reset hold; repeating it every 250 ms while Releasing made the next
    // handshake start inside a reset hold that then rewrote D+/D- under it ("no handshake" on a bank).
    Fake f; f.caps = {}; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kQc2, f.in, true), ct::Reason::None);
    f.tick(r, 60000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.fc_releases, 1);   // one session for 5/9/12 V (9 V and 12 V are retargets), released once at the unit's end
}
static void test_fcp_rows_after_the_first_are_retargets_in_one_session() {
    // The FCP 9 V and 12 V rows ride the session the 5 V row opened (as AFC / QC2 do): a retarget does not repeat the
    // caps read or the attach, and the unit releases once at its end.
    Fake f; f.caps = {}; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, {.selected = ct::bit(A::Fcp)}, f.in, true), ct::Reason::None);
    f.tick(r, 60000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.fc_requests, 3);
    CHECK_EQ(f.retarget_log.size(), 3u);
    CHECK_EQ(f.retarget_log[0], false); CHECK_EQ(f.retarget_log[1], true); CHECK_EQ(f.retarget_log[2], true);
    CHECK_EQ(f.fc_mvs[0], 5000); CHECK_EQ(f.fc_mvs[1], 9000); CHECK_EQ(f.fc_mvs[2], 12000);
    CHECK_EQ(f.fc_releases, 1);
}
static void test_5v_row_needs_the_handshake() {
    // 5 V is what every source gives: a source that refuses the handshake passes no row, the 5 V one included. The 5 V row opens
    // the session, so its failure is the unit's: 9 V and 12 V are retargets in it and are skipped (no request of their own).
    Fake f; f.caps = {}; f.request_ok = false; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kQc2, f.in, true), ct::Reason::None);
    f.tick(r, 12000);   // the unit's own detach and attach come first
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(r.steps()[0].verdict, ct::Verdict::Fail);
    CHECK_EQ(f.fc_requests, 1);
    CHECK_EQ(count_verdict_any(r, ct::Verdict::Fail), 1);
    CHECK_EQ(count_verdict_any(r, ct::Verdict::Skip), 2);
    CHECK_EQ(count_verdict_any(r, ct::Verdict::Pass), 0);
}
static unsigned count_verdict(const ct::Runner& r, ct::Group g, ct::Verdict v) {
    unsigned n = 0;
    for (const auto& s : r.steps()) n += s.group == g && s.verdict == v;
    return n;
}
static void test_source_drop_recovers_on_a_non_pd_source() {
    // A bank that resets on an unsupported level: the row fails, the run waits for vSafe5V and carries on.
    Fake f; f.caps = {}; f.drop_at = 2; ct::Runner r(f);   // request 1 = the 5 V row, request 2 = the 9 V row
    CHECK_EQ(r.start(f.caps, kQc2, f.in, true), ct::Reason::None);
    f.tick(r, 20000);   // the 4 s outage ends the wait the moment VBUS is back
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_verdict(r, ct::Group::Qc2, ct::Verdict::Pass), 2);
    CHECK_EQ(count_verdict(r, ct::Group::Qc2, ct::Verdict::Fail), 1);
    CHECK_EQ(r.steps()[1].reason, ct::Reason::Lost);
}
// Source loss ends the legacy session. Nothing of the handshake survives it: the row after a lost one opens its own session
// (retarget=false, so the engine runs the whole handshake again) instead of retargeting inside the dead one. ("confirmed" in the
// [FC] start line is the user's consent for levels above 5 V, not a memory of a handshake.)
static void test_a_lost_row_ends_the_session_so_the_next_row_handshakes_again() {
    for (const auto a : {A::Qc2, A::Qc3, A::Qc3Plus}) {
        Fake f; f.caps = {}; f.drop_at = 2; ct::Runner r(f);   // request 2 = the second row of the unit
        CHECK_EQ(r.start(f.caps, {.selected = ct::bit(a)}, f.in, true), ct::Reason::None);
        f.tick(r, 90000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(r.steps()[1].reason, ct::Reason::Lost);
        // The request right after the lost one is a fresh session; a later request in that session may retarget again.
        CHECK_EQ(f.fc_requests >= 3, true);
        CHECK_EQ(f.retarget_log[1], true);                        // the lost row itself was a retarget in the first session
        CHECK_EQ(f.retarget_log[2], false);                       // the row after the loss opens a new session
    }
}
static void test_session_blip_with_vbus_up_is_not_source_lost() {
    // A legacy row whose session counter ticks (CC glitch) while VBUS stays at the level is not "source lost".
    Fake f; f.caps = {}; f.glitch_at = 2; ct::Runner r(f);   // request 2 = the 9 V row
    r.start(f.caps, kQc2, f.in, true);
    f.tick(r, 20000);
    CHECK_EQ(r.state(), ct::State::Done);
    for (const auto& st : r.steps()) CHECK_EQ(st.reason == ct::Reason::Lost, false);
}
static void test_source_gone_stops_the_run() {
    Fake f; f.caps = {}; f.drop_at = 2; f.gone = true; ct::Runner r(f);   // request 2 = the 9 V row
    r.start(f.caps, kQc2, f.in, true);
    f.tick(r, 40000);
    CHECK_EQ(r.state(), ct::State::Stopped);
    CHECK_EQ(r.reason(), ct::Reason::Lost);
}
static void test_pd_row_survives_one_source_hard_reset() {
    // The source resets mid-row and is back within 3 s: the row is asked again once, the run goes on.
    Fake f; f.pd_drop_at = 2; ct::Runner r(f);   // request 1 = the 5 V row, request 2 = the 9 V row
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    f.tick(r, 90000);
    CHECK_EQ(r.state(), ct::State::Done);
    unsigned pass = 0, fail = 0;
    for (const auto& s : r.steps()) { pass += s.group == ct::Group::Pd && s.verdict == ct::Verdict::Pass; fail += s.verdict == ct::Verdict::Fail; }
    CHECK_EQ(fail, 0u); CHECK_EQ(pass >= 5, true);
}
static void test_pd_row_gives_up_after_a_second_source_hard_reset() {
    // The same row loses the source twice (it comes back each time): that row fails, the others still run.
    Fake f; f.pd_drop_at = 2; f.pd_drop_times = 2; ct::Runner r(f);
    r.start(f.caps, kPd, f.in, true);
    f.tick(r, 120000);
    CHECK_EQ(r.state(), ct::State::Done);
    unsigned lost = 0, pass = 0;
    for (const auto& s : r.steps()) { lost += s.verdict == ct::Verdict::Fail && s.reason == ct::Reason::Lost; pass += s.group == ct::Group::Pd && s.verdict == ct::Verdict::Pass; }
    CHECK_EQ(lost, 1u); CHECK_EQ(pass >= 5, true);
}
// pattern pps-off-at-no-load (splitter run, 2026-11-10): a programmable level the source accepts and then drops while the port
// draws ~0 mA is "needs load" (a Skip), not "source lost". Only that exact signature: Accept, no PS_RDY, VBUS collapsed, ~0 mA.
struct CollapseRows { unsigned n = 0, needs_load = 0, lost = 0, other_fail = 0, passed = 0; };
static CollapseRows collapse_rows(const ct::Runner& r, A algorithm) {
    CollapseRows c;
    for (const auto& s : r.steps()) {
        if (s.group != ct::Group::Pd || s.algorithm != algorithm || s.mv <= 5000 || s.hunt) continue;
        ++c.n;
        c.needs_load += s.verdict == ct::Verdict::Skip && s.reason == ct::Reason::NeedsLoad;
        c.lost += s.verdict == ct::Verdict::Fail && s.reason == ct::Reason::Lost;
        c.other_fail += s.verdict == ct::Verdict::Fail && s.reason != ct::Reason::Lost;
        c.passed += s.verdict == ct::Verdict::Pass;
    }
    return c;
}
static void test_pps_accepted_then_dropped_at_no_load_is_needs_load() {
    for (const int ma : {0, 10}) {
        Fake f; f.collapse_on = pd::PdoType::Pps; f.collapse_ma = ma;
        ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
        f.tick(r, 400000);
        CHECK_EQ(r.state(), ct::State::Done);
        const auto c = collapse_rows(r, A::Pps);
        CHECK_EQ(c.n >= 2u, true);
        CHECK_EQ(c.needs_load, c.n); CHECK_EQ(c.lost, 0u); CHECK_EQ(c.other_fail, 0u);   // every raised PPS row of the run, none chain-skipped
        CHECK_STR(ct::name(ct::Reason::NeedsLoad), "needs load");
        for (const auto& s : r.steps()) {
            if (s.reason == ct::Reason::NeedsLoad) CHECK_EQ(s.verdict, ct::Verdict::Skip);
            if (s.algorithm == A::Pd3Fixed) CHECK_EQ(s.verdict, ct::Verdict::Pass);   // the fixed rows of the same source are untouched
            CHECK_EQ(s.verdict == ct::Verdict::Running || s.verdict == ct::Verdict::Pending, false);
        }
    }
}
static void test_pps_collapse_with_current_drawn_stays_lost() {
    // 80 mA is over the no-load bound and under the load abort (kLoadAbortMa): the source lost a row that carried a load
    Fake f; f.collapse_on = pd::PdoType::Pps; f.collapse_ma = 80;
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    const auto c = collapse_rows(r, A::Pps);
    CHECK_EQ(c.n >= 2u, true); CHECK_EQ(c.lost, c.n); CHECK_EQ(c.needs_load, 0u);
    // 400 mA is a load abort (Inline) today: it must not read "needs load" either
    Fake g; g.collapse_on = pd::PdoType::Pps; g.collapse_ma = 400;
    ct::Runner q(g); q.start(g.caps, kPd, g.in, true);
    g.tick(q, 400000);
    CHECK_EQ(count_reason_all(q, ct::Reason::NeedsLoad), 0u);
}
static void test_fixed_row_collapse_stays_lost() {
    Fake f; f.collapse_on = pd::PdoType::Fixed; f.collapse_ma = 0;
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    const auto c = collapse_rows(r, A::Pd3Fixed);
    CHECK_EQ(c.n >= 2u, true); CHECK_EQ(c.lost, c.n); CHECK_EQ(c.needs_load, 0u);   // a fixed source output must hold at no load
    CHECK_EQ(count_reason_all(r, ct::Reason::NeedsLoad), 0u);
}
static void test_pps_collapse_after_ps_rdy_stays_lost() {
    Fake f; f.collapse_on = pd::PdoType::Pps; f.collapse_ma = 0; f.collapse_after_ready = true;
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    const auto c = collapse_rows(r, A::Pps);
    CHECK_EQ(c.n >= 2u, true); CHECK_EQ(c.lost, c.n); CHECK_EQ(c.needs_load, 0u);   // the source did deliver the level: PS_RDY was the proof
    CHECK_EQ(count_reason_all(r, ct::Reason::NeedsLoad), 0u);
}
// ROADMAP "PPS coverage" item 1: a source that advertises 3.3 V but regulates at 5 V is a fail "floor at 5.0 V" with
// measured_mv = the reached level (the PdLow low-edge row reports its floor, not only pass/fail).
static void test_low_edge_that_regulates_at_5v_is_a_floored_fail() {
    Fake f; f.caps.pdos[5] = pd::decode_pdo(0xc0000000u | (110u << 17) | (33u << 8) | 100);   // PPS 3.3-11 V 5 A
    f.low_mv_for_3v3 = 5000;   // the 3.3 V request is granted (PS_RDY) but the output stays at 5 V
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    bool seen = false;
    for (const auto& s : r.steps()) {
        if (s.algorithm != A::Pps || s.mv != 3300) continue;
        seen = true;
        CHECK_EQ(int(s.verdict), int(ct::Verdict::Fail));
        CHECK_EQ(int(s.reason), int(ct::Reason::Floored));
        CHECK_EQ(s.measured_mv, 5000);   // the floor the source held
    }
    CHECK_EQ(seen, true);
    CHECK_STR(ct::name(ct::Reason::Floored), "floored above target");
    // the levels above the floor still ran (no chain skip)
    CHECK_EQ(count_reason_all(r, ct::Reason::Floored), 1u);
}
// ROADMAP "PPS coverage" item 2: the step-resolution walk - one row per PPS APDO, from the mid level, five +20 mV
// Requests. A source that follows them passes; one that rounds to 100 mV or ignores the steps fails
// "step resolution N mV" (detail = the measured move).
static bool walk_requests_at(const Fake& f, size_t i, unsigned pos, unsigned base_mv) {
    if (i + 5 >= f.choices.size()) return false;
    for (unsigned k = 0; k < 6; ++k)
        if (f.choices[i + k].pos != pos || int(f.choices[i + k].mv) != int(base_mv + 20 * k)) return false;
    return true;
}
static void test_pps_step_walk_passes_on_20mv_steps() {
    Fake f;   // default APDOs 5-11 V (pos 6, mid 8000) and 5-21 V (pos 7, mid 13000)
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    bool walk6 = false, walk7 = false;
    for (size_t i = 0; i + 5 < f.choices.size(); ++i) {
        if (walk_requests_at(f, i, 6, 8000)) walk6 = true;
        if (walk_requests_at(f, i, 7, 13000)) walk7 = true;
    }
    CHECK_EQ(walk6, true); CHECK_EQ(walk7, true);   // six Requests each: the mid level, then +20 mV x 5
    CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution), 0u);
    unsigned pass8000 = 0;
    for (const auto& s : r.steps())
        if (s.algorithm == A::Pps && s.pos == 6 && s.mv == 8000 && s.verdict == ct::Verdict::Pass) ++pass8000;
    CHECK_EQ(pass8000, 3u);   // the two mid level rows (the 7.14 <-> 11 V jump) and the step-walk row all passed
}
// Item G: the PS_RDY timing quirks are noted (Evidence::timing, one [CT] line per flagged row), never judged: the verdicts of a run
// whose source is late / inferred / out of band equal the verdicts of the same run without the quirk.
static std::vector<std::pair<ct::Verdict, ct::Reason>> verdicts_of(const ct::Runner& r) {
    std::vector<std::pair<ct::Verdict, ct::Reason>> v;
    for (const auto& s : r.steps()) v.emplace_back(s.verdict, s.reason);
    return v;
}
static void test_ps_rdy_late_is_noted_and_never_judged() {
    static_assert(!ct::ps_rdy_late(550, false) && ct::ps_rdy_late(551, false) && !ct::ps_rdy_late(1020, true) && ct::ps_rdy_late(1021, true));
    Fake plain; ct::Runner rp(plain); run_pps_only(plain, rp);
    CHECK_EQ(rp.evidence().timing, 0);
    CHECK_EQ(rp.evidence().ps_rdy_max, 0);
    Fake f; f.ps_rdy_delay_ms = 700; ct::Runner r(f); run_pps_only(f, r);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ((r.evidence().timing & ct::PsRdyLate) != 0, true);
    CHECK_EQ((r.evidence().timing & (ct::PsRdyInferred | ct::PsRdyOutOfBand)) == 0, true);
    CHECK_EQ(r.evidence().ps_rdy_max, 35);   // 700 ms in 20 ms units
    CHECK_EQ(verdicts_of(r) == verdicts_of(rp), true);
    Fake ok; ok.ps_rdy_delay_ms = 500; ct::Runner ro(ok); run_pps_only(ok, ro);   // inside the spec maximum: nothing to note
    CHECK_EQ(ro.evidence().timing, 0);
    CHECK_EQ(ro.evidence().ps_rdy_max, 25);
}
static void test_ps_rdy_inferred_is_noted_and_never_judged() {
    Fake plain; ct::Runner rp(plain); run_pps_only(plain, rp);
    Fake f; f.ps_rdy_inferred_fake = true; ct::Runner r(f); run_pps_only(f, r);
    CHECK_EQ((r.evidence().timing & ct::PsRdyInferred) != 0, true);
    CHECK_EQ(verdicts_of(r) == verdicts_of(rp), true);
}
// FH22 (2026-11-08): PS_RDY at a 21 V contract with 19.42 V delivered. The capped rows keep their Capped verdict; the quirk is noted.
static void test_ps_rdy_out_of_band_is_noted_and_never_judged() {
    Fake f; f.pd_max_mv = 19400; ct::Runner r(f); run_pps_only(f, r);
    CHECK_EQ((r.evidence().timing & ct::PsRdyOutOfBand) != 0, true);
    CHECK_EQ(count_reason_all(r, ct::Reason::Capped) >= 1u, true);
    Fake plain; ct::Runner rp(plain); run_pps_only(plain, rp);
    CHECK_EQ((rp.evidence().timing & ct::PsRdyOutOfBand) == 0, true);
}
// The latches restart at every step-walk level: a level that is late is seen although the base level had its PS_RDY long before.
static void test_ps_rdy_timing_is_noted_per_walk_level() {
    Fake f; f.ps_rdy_delay_ms = 800; f.ps_rdy_delay_mv = 8060;   // only the walk's fourth level (8000 + 3 x 20 mV) is late
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ((r.evidence().timing & ct::PsRdyLate) != 0, true);
    CHECK_EQ(r.evidence().ps_rdy_max, 40);
    CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution), 0u);
}
// ROADMAP "PPS deep sweep": opt-in (Options::pps_sweep), one row per APDO marked kDetailSweep, every 100 mV a Request
// through the gate at <= 1 A; a steady out-of-band point is RECORDED, not a stop - the FH22 shape (tracks -150 mV to
// 19.75 V at the 19.9 V request, then folds back to 19.42 V from 20.0 V) is recorded to the top and the summary names
// the peak and the fold-back.
static void test_pps_deep_sweep_records_every_point_and_the_fold_back() {
    Fake f;   // APDOs 5-11 V (pos 6) and 5-21 V (pos 7)
    f.pd_offset_mv = -150;                                // tracks -150 mV
    f.pd_fold_from_mv = 20000; f.pd_fold_to_mv = 19420;   // >= 20.0 V folds back, steady (FH22)
    ct::Runner r(f);
    ct::Options o{.selected = ct::bit(A::Pps)}; o.pps_sweep = true;
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    unsigned sweep_rows = 0;
    for (const auto& s : r.steps())
        if (ct::is_sweep(s)) {
            ++sweep_rows;
            CHECK_EQ(s.detail, ct::kDetailSweep);
            CHECK_EQ(s.ma <= 1000, true);   // 1 A, never above the APDO current (5 A here) or the cable (5 A)
        }
    CHECK_EQ(sweep_rows, 2u);   // one row per APDO - the points are not rows
    f.tick(r, 900000);
    CHECK_EQ(r.state(), ct::State::Done);
    // both sweep rows passed and kept their marker after finish()
    unsigned passed = 0;
    for (const auto& s : r.steps())
        if (ct::is_sweep(s)) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(s.detail, ct::kDetailSweep); ++passed; }
    CHECK_EQ(passed, 2u);
    // the record: 61 + 161 points, every one a Request at a 100 mV step of its APDO
    const auto v = r.sweep_view();
    CHECK_EQ(v.rows.size(), 2u);
    CHECK_EQ(v.points.size(), 222u);
    const auto* rw6 = v.find(uint8_t(A::Pps), 6);
    const auto* rw7 = v.find(uint8_t(A::Pps), 7);
    CHECK_EQ(rw6 && rw7, true);
    CHECK_EQ(rw6->n, 61u); CHECK_EQ(rw7->n, 161u);
    // every sweep point went through request_pd: the pos-7 requests step 5000..21000 by 100 mV
    {
        unsigned stepped = 0;
        for (const auto& c : f.choices)
            if (c.pos == 7 && c.mv >= 5000 && c.mv <= 21000 && (c.mv - 5000) % 100 == 0 && c.ma <= 1000) ++stepped;
        CHECK_EQ(stepped >= 161u, true);   // the sweep's points (sample rows ask other levels or more current)
    }
    // the pos-7 summary reads the FH22 shape: peak 19.75 V at 19.9 V, folds back to 19.42 V from 20.0 V - recorded, not a stop
    CHECK_EQ(rw7->sum.points, 161u);
    CHECK_EQ(rw7->sum.peak_mv, 19750u); CHECK_EQ(rw7->sum.peak_at_mv, 19900u);
    CHECK_EQ(rw7->sum.fold_from_mv, 20000u); CHECK_EQ(rw7->sum.fold_mv, 19420u);
    char buf[64];
    const size_t n = ctsweep::format_summary(rw7->sum, buf);
    CHECK_STR(std::string(buf, n).c_str(), "peak 19.75 V at 19.9 V, folds back to 19.42 V from 20.0 V");
    // the pos-6 sweep (5-11 V) tracks clean: peak at the top
    CHECK_EQ(rw6->sum.peak_mv, 10850u); CHECK_EQ(rw6->sum.peak_at_mv, 11000u);
    CHECK_EQ(rw6->sum.fold_mv, 0u);
    // QA 2026-11-10 row B item 3 (red): a finished sweep row's target is the setpoint its peak came at and its
    // measured level is that peak - the old row read "target 5000 mV, measured 20978 mV" for a 5-21 V sweep.
    for (const auto& s : r.steps())
        if (ct::is_sweep(s) && s.pos == 7) {
            CHECK_EQ(s.mv, 19900u);           // peak_at_mv, not the range's first point (5000)
            CHECK_EQ(s.measured_mv, 19750u);  // peak_mv, not the last (folded-back) point
        }
}
// The sweep stops at the first LOST source (a Hard Reset mid-sweep): the row fails there, the points recorded so far
// stay in the record, and the run goes on to its end.
static void test_pps_deep_sweep_stops_at_the_first_lost_source() {
    Fake f;
    f.pd_drop_at_mv = 12000;   // the source hard-resets on the 12.0 V point of the pos-7 sweep
    ct::Runner r(f);
    ct::Options o{.selected = ct::bit(A::Pps)}; o.pps_sweep = true;
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 900000);
    CHECK_EQ(r.state(), ct::State::Done);
    const auto v = r.sweep_view();
    const auto* rw7 = v.find(uint8_t(A::Pps), 7);
    CHECK_EQ(rw7 != nullptr, true);
    if (rw7) {
        CHECK_EQ(rw7->n, 70u);                       // 5.0 .. 11.9 V recorded; the 12.0 V point lost the source
        CHECK_EQ(rw7->sum.points, 70u);
        CHECK_EQ(rw7->sum.peak_mv, 11900u);
    }
    for (const auto& s : r.steps())
        if (ct::is_sweep(s) && s.pos == 7) {
            CHECK_EQ(s.verdict, ct::Verdict::Fail);
            CHECK_EQ(s.reason == ct::Reason::Lost || s.reason == ct::Reason::HardReset, true);   // a lost source stops the sweep
            CHECK_EQ(s.detail, ct::kDetailSweep);   // the marker survives the failure
        }
    // the pos-6 sweep ran to its top before that
    const auto* rw6 = v.find(uint8_t(A::Pps), 6);
    CHECK_EQ(rw6 && rw6->n == 61u, true);
    // no pos-7 request above the lost point went out after the sweep row ended
    unsigned after = 0;
    bool dropped = false;
    for (const auto& c : f.choices) {
        if (c.pos == 7 && c.mv == 12000 && c.ma <= 1000) dropped = true;
        if (dropped && c.pos == 7 && c.mv > 12000 && c.ma <= 1000) ++after;
    }
    CHECK_EQ(dropped, true);
    CHECK_EQ(after, 0u);
}
// The Running view reads the sweep row in flight through Runner::sweep_live(): active only while a sweep row is the step under
// test, the planned point range, the setpoint whose Request is out, and the last recorded point (measured - setpoint).
static void test_sweep_live_follows_the_row_in_flight() {
    Fake f;   // APDOs 5-11 V (pos 6) and 5-21 V (pos 7)
    f.pd_offset_mv = -150;
    ct::Runner r(f);
    ct::Options o{.selected = ct::bit(A::Pps)}; o.pps_sweep = true;
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    CHECK_EQ(r.sweep_live().active, false);   // nothing running yet
    unsigned ticks = 0;
    while (!r.sweep_live().active && ticks++ < 20000) f.tick(r, 100);   // the plan rows run before the sweep rows
    const auto first = r.sweep_live();
    CHECK_EQ(first.active, true);
    CHECK_EQ(first.n, 0u);
    CHECK_EQ(first.set_mv, first.lo_mv);      // the first Request is the row's first point
    CHECK_EQ(first.hi_mv > first.lo_mv, true);
    while (r.sweep_live().n < 5 && ticks++ < 40000) f.tick(r, 100);
    const auto l = r.sweep_live();
    CHECK_EQ(l.active, true);
    CHECK_EQ(l.lo_mv, first.lo_mv); CHECK_EQ(l.hi_mv, first.hi_mv);
    CHECK_EQ(l.n >= 5u, true);
    CHECK_EQ(l.set_mv, l.lo_mv + l.n * ctsweep::kStepMv);   // the point in flight follows the recorded ones
    CHECK_EQ(l.last_err, -150);                              // the fake tracks 150 mV low
    CHECK_EQ(l.last_measured_mv(), int(l.last_set_mv()) - 150);
    // the row's range is the APDO's, clamped like the planner did: the pos-6 APDO (5-11 V) is first, 5000..11000
    CHECK_EQ(l.lo_mv == 5000u && l.hi_mv == 11000u, true);
    f.tick(r, 900000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(r.sweep_live().active, false);   // nothing is running any more
}
// ... and never for a run without a sweep row.
static void test_sweep_live_is_idle_without_a_sweep_row() {
    Fake f; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    for (unsigned i = 0; i < 300; ++i) { f.tick(r, 100); CHECK_EQ(r.sweep_live().active, false); }
}
// The sweep is opt-in: without Options::pps_sweep the plan holds no sweep row at all.
static void test_pps_deep_sweep_is_opt_in() {
    Fake f; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    for (const auto& s : r.steps()) CHECK_EQ(ct::is_sweep(s), false);
    CHECK_EQ(r.sweep_view().rows.size(), 0u);
}
// PD Stress borrows the deep sweep's point buffer: refused while a run is on, empties the view when granted, hands out the whole buffer.
static void test_claim_sweep_record_lends_the_point_buffer() {
    Fake f;
    ct::Runner r(f);
    ct::Options o{.selected = ct::bit(A::Pps)}; o.pps_sweep = true;
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 30);   // running
    CHECK_EQ(r.busy(), true);
    CHECK_EQ(r.claim_sweep_record().size(), 0u);   // refused while a test runs
    f.tick(r, 900000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(r.sweep_view().points.size() > 0, true);
    const auto rec = r.claim_sweep_record();
    CHECK_EQ(rec.size(), size_t(ctsweep::kMaxPoints));
    CHECK_EQ(rec.size() >= 636u, true);   // the PD Stress record (kRecordWords) fits
    CHECK_EQ(r.sweep_view().rows.size(), 0u);   // the old plot is gone, not misread through a foreign record
    CHECK_EQ(r.sweep_view().points.size(), 0u);
    rec[0] = 123;   // the span is writable and stays valid
    CHECK_EQ(r.sweep_view().points.size(), 0u);
}
// ROADMAP "AVS deep sweep": opt-in via Options::avs_sweep, one kDetailSweep row per AVS APDO (algorithm SprAvs /
// EprAvs), every 100 mV across the APDO (the AVS Request granularity) capped at the monitor ceiling, <= 1 A never
// above the APDO's current or the cable. Same record, strip and summary as the PPS sweep.
static void test_avs_deep_sweep_plans_and_runs_per_spr_avs_apdo() {
    Fake f;
    f.caps.num_pdos = 7;
    for (unsigned n = 0; n < 5; ++n) f.caps.pdos[n] = pd::decode_pdo(((5000 + 4000 * n) / 50 << 10) | 325);
    f.caps.pdos[6] = pd::decode_pdo(0xE004B145);   // SPR AVS 9-20 V (pos 7): 3.25 A above 15 V, 180 mA at/below
    ct::Runner r(f);
    ct::Options o{.selected = ct::bit(A::SprAvs)}; o.avs_sweep = true;
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    unsigned sweep_rows = 0;
    for (const auto& s : r.steps())
        if (ct::is_sweep(s)) {
            ++sweep_rows;
            CHECK_EQ(s.algorithm == A::SprAvs && s.pos == 7, true);
            CHECK_EQ(s.detail, ct::kDetailSweep);
            CHECK_EQ(s.avs, true);
            CHECK_EQ(s.mv, 9000u);                       // the row's mv is the first setpoint
            CHECK_EQ(s.ma <= 1000, true);                // 1 A, never above the offer or the cable
        }
    CHECK_EQ(sweep_rows, 1u);   // one row for the AVS APDO - the points are not rows
    f.tick(r, 900000);
    CHECK_EQ(r.state(), ct::State::Done);
    const auto v = r.sweep_view();
    const auto* rw = v.find(uint8_t(A::SprAvs), 7);
    CHECK_EQ(rw != nullptr, true);
    if (rw) {
        CHECK_EQ(rw->n, 111u); CHECK_EQ(rw->lo_mv, 9000u); CHECK_EQ(rw->hi_mv, 20000u);   // 9.0..20.0 V every 100 mV
        CHECK_EQ(rw->sum.points, 111u);
    }
    // every point went through the sink's single gate at a 100 mV step, <= 1 A, never above the ceiling
    // (the APDO's own sample rows land on the same grid, so the count is a floor; the record above is exact)
    unsigned stepped = 0;
    for (const auto& c : f.choices) {
        CHECK_EQ(c.mv <= 36000, true);
        if (c.pos == 7 && c.mv >= 9000 && c.mv <= 20000 && (c.mv - 9000) % 100 == 0 && c.ma <= 1000) ++stepped;
    }
    CHECK_EQ(stepped >= 111u, true);
    // opt-in: without the flag no AVS sweep row at all (the Tests-list entry hides too: the plan counts 0 AVS points)
    Fake f2;
    f2.caps.num_pdos = 7;
    for (unsigned n = 0; n < 5; ++n) f2.caps.pdos[n] = pd::decode_pdo(((5000 + 4000 * n) / 50 << 10) | 325);
    f2.caps.pdos[6] = pd::decode_pdo(0xE004B145);
    ct::Runner r2(f2);
    ct::Options o2{.selected = ct::bit(A::SprAvs)};
    CHECK_EQ(r2.start(f2.caps, o2, f2.in, true), ct::Reason::None);
    for (const auto& s : r2.steps()) CHECK_EQ(ct::is_sweep(s), false);
    CHECK_EQ(k2::ctsweep::plan_total(f2.caps, 36000, k2::ctsweep::Kind::Avs), 111u);   // the entry shows when AVS is offered
}
// The EPR AVS sweep: with EPR entered (the EPR identity present and the entry made), the EPR AVS APDO gets one sweep
// row from 15 V to the APDO top capped at this K2's monitor ceiling (36 V here; 48 V reads "above the K2's monitor"),
// and every point request stays at or below the ceiling. The sweep runs in EPR mode, so the sink's EPR_KeepAlive
// (every 400 ms, test_pd_sink) holds the contract between points, like the PPS keep-alive does for the PPS sweep.
static void test_avs_deep_sweep_epr_capped_at_the_monitor_ceiling() {
    Fake f;
    f.caps.pdos[0] = pd::decode_pdo(f.caps.pdos[0].raw | (1u << 23));   // EPR-capable PDO 1
    f.in.pd.state = pd::SinkState::Ready; f.in.pd.mv = 5000;
    // the Fake's cable is already the EPR 50 V 5 A identity (cable_observed, 50000 mV, 5000 mA)
    f.epr_caps = f.caps; f.epr_caps.epr = true; f.epr_caps.num_pdos = 10;
    f.epr_caps.pdos[9] = pd::decode_pdo(0xD0000000 | (480u << 17) | (150u << 8) | 140);   // EPR AVS 15..48 V (pos 10)
    ct::Runner r(f);
    ct::Options o{.selected = ct::bit(A::EprAvs)}; o.avs_sweep = true;
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    bool epr_seen = false;
    for (unsigned n = 0; n < 2000 && r.state() != ct::State::Done; ++n) {
        f.tick(r, 500);
        epr_seen |= f.in.pd.epr_mode;   // the pos-0 entry put the sink in EPR mode for the sweep
    }
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(epr_seen, true);           // EPR was entered and held through the sweep (EPR_KeepAlive runs there)
    unsigned sweep_rows = 0;
    const ct::Step* sweep_step = nullptr;
    for (const auto& s : r.steps())
        if (ct::is_sweep(s)) {
            ++sweep_rows;
            CHECK_EQ(s.algorithm == A::EprAvs && s.pos == 10, true);
            CHECK_EQ(s.ma <= 1000, true);
            sweep_step = &s;
        }
    CHECK_EQ(sweep_rows, 1u);
    const auto* rw = r.sweep_view().find(uint8_t(A::EprAvs), 10);
    CHECK_EQ(rw != nullptr, true);
    if (rw) {
        CHECK_EQ(rw->n, 211u);                                  // 15.0..36.0 V every 100 mV, capped at the ceiling
        CHECK_EQ(rw->lo_mv, 15000u); CHECK_EQ(rw->hi_mv, 36000u);
        CHECK_EQ(rw->sum.points, 211u);
        // Row B item 3: the sweep row reports the sweep - target is the peak's setpoint, measured is the peak.
        CHECK_EQ(sweep_step != nullptr, true);
        if (sweep_step) {
            CHECK_EQ(sweep_step->mv, rw->sum.peak_at_mv);
            CHECK_EQ(sweep_step->measured_mv, rw->sum.peak_mv);
        }
    }
    unsigned stepped = 0;
    for (const auto& c : f.choices) {
        CHECK_EQ(c.mv <= 36000, true);   // never request above the monitor ceiling
        if (c.pos == 10 && c.mv >= 15000 && c.mv <= 36000 && (c.mv - 15000) % 100 == 0 && c.ma <= 1000) ++stepped;
    }
    CHECK_EQ(stepped >= 211u, true);   // the APDO's sample rows share the grid; the record above is exact
    // without an EPR identity the EPR AVS APDO is skipped with the existing EPR reasons and no sweep row is planned
    Fake g;
    g.caps.pdos[0] = pd::decode_pdo(g.caps.pdos[0].raw | (1u << 23));
    g.in.cable_observed = true; g.in.cable_mv = 20000; g.in.cable_ma = 3000;   // not a 50 V 5 A identity
    g.in.pd.state = pd::SinkState::Ready; g.in.pd.mv = 5000;
    g.info_list = g.caps; g.info_list.epr = true; g.info_list.num_pdos = 10;
    g.info_list.pdos[9] = pd::decode_pdo(0xD0000000 | (480u << 17) | (150u << 8) | 140);
    g.info_mode = 1;
    ct::Runner rg(g);
    ct::Options og{.selected = ct::bit(A::EprAvs)}; og.avs_sweep = true;
    CHECK_EQ(rg.start(g.caps, og, g.in, true), ct::Reason::None);
    g.tick(rg, 90000);
    CHECK_EQ(rg.state(), ct::State::Done);
    for (const auto& s : rg.steps()) CHECK_EQ(ct::is_sweep(s), false);   // no EPR cable: no EPR AVS sweep row
    CHECK_EQ(g.pd_requests, 0);                                          // nothing was requested (informational list only)
}
static void test_pps_step_walk_fails_when_steps_round_to_100mv() {
    Fake f; f.pd_small_step_mv = 100;   // every +20 mV request moves the output by 100 mV
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution) >= 1u, true);
    bool checked = false;
    for (const auto& s : r.steps()) {
        if (s.reason != ct::Reason::StepResolution) continue;
        checked = true;
        CHECK_EQ(int(s.verdict), int(ct::Verdict::Fail));
        CHECK_EQ(s.detail, 100);   // "step resolution 100 mV"
    }
    CHECK_EQ(checked, true);
    CHECK_STR(ct::name(ct::Reason::StepResolution), "step resolution");
}
static void test_pps_step_walk_passes_a_noisy_step_and_fails_a_short_sum() {
    {   // 16 mV a step: inside one step's band, the sum 80 mV is the edge of 100 +- 20 -> pass
        Fake f; f.pd_small_step_mv = 16;
        ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
        f.tick(r, 400000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution), 0u);
    }
    {   // 15 mV a step every time: no single step is wrong, but the walk adds up to 75 mV -> fail, detail = the mean
        Fake f; f.pd_small_step_mv = 15;
        ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
        f.tick(r, 400000);
        CHECK_EQ(r.state(), ct::State::Done);
        bool checked = false;
        for (const auto& s : r.steps()) {
            if (s.reason != ct::Reason::StepResolution) continue;
            checked = true;
            CHECK_EQ(s.detail, 15);
        }
        CHECK_EQ(checked, true);
    }
}
static void test_pps_step_walk_fails_when_steps_are_ignored() {
    Fake f; f.pd_small_step_mv = 0;   // the +20 mV requests move the output not at all
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution) >= 1u, true);
    bool checked = false;
    for (const auto& s : r.steps()) {
        if (s.reason != ct::Reason::StepResolution) continue;
        checked = true;
        CHECK_EQ(int(s.verdict), int(ct::Verdict::Fail));
        CHECK_EQ(s.detail, 0);   // "steps ignored"
    }
    CHECK_EQ(checked, true);
}

// ROADMAP "The PPS step walk fails a good source: its base level is read before the output settles" (UGREEN Nexode
// Air 65 W, 2026-11-10): the base request at 13000 mV overshoots to 13007 mV and settles to 12990 over 900 ms; the
// +20 mV steps after it are followed. The walk must record the base only once VBUS is steady, or the first move
// reads ~0 or negative and a good source fails "steps ignored". Host shape from the row: base 13007 settling to
// 12990, then +20 mV steps.
static void test_pps_step_walk_waits_for_the_base_to_settle() {
    Fake f;
    f.walk_settle_mv = 13000; f.walk_settle_from_mv = 13007; f.walk_settle_to_mv = 12990; f.walk_settle_ms = 900;
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    bool walk7 = false;
    for (size_t i = 0; i + 5 < f.choices.size(); ++i)
        if (walk_requests_at(f, i, 7, 13000)) walk7 = true;
    CHECK_EQ(walk7, true);   // the six walk Requests went out (base, then +20 mV x 5)
    CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution), 0u);   // the base was recorded after it settled
}

// walk-settle, pure: WalkSteady needs kWalkSteadySamples samples inside the window, all within kWalkSteadyMv.
static void test_walk_steady_window() {
    ct::WalkSteady w;
    CHECK_EQ(w.steady(), false);
    w.add(1000, 13007); CHECK_EQ(w.steady(), false);
    w.add(1100, 13004); CHECK_EQ(w.steady(), false);   // only 2 samples
    w.add(1200, 13001); CHECK_EQ(w.steady(), false);   // span 6 mV > 3: still settling
    w.add(1300, 12990); w.add(1400, 12990);
    CHECK_EQ(w.steady(), false);                       // the window still holds the 13001 sample: span 11 mV
    w.add(1600, 12990);                                // the 1200 sample is now older than the window
    CHECK_EQ(w.steady(), true);                        // 12990 x3 inside 300 ms
    w.add(1700, 13004);                                // a ripple back up: span 14 mV
    CHECK_EQ(w.steady(), false);
    w.clear();
    CHECK_EQ(w.steady(), false);
}

static void test_walk_steady_mean() {
    ct::WalkSteady w;
    CHECK_EQ(w.mean(777), 777);                        // empty: the fallback
    w.add(1000, 13040); w.add(1100, 12960);
    CHECK_EQ(w.mean(0), 13000);
    w.add(1200, 13040); w.add(1300, 12960);
    CHECK_EQ(w.mean(0), 13000);
    w.add(1500, 13010);                                // 1000 and 1100 left the 300 ms window: 13040, 12960, 13010 remain
    CHECK_EQ(w.mean(0), 13003);
}
// The steady wait is bounded: a source whose output never sits within the span (a +/- 5 mV ripple every tick)
// still has each level recorded after kWalkSteadyTimeoutMs, and the walk completes and passes.
static void test_pps_step_walk_steady_wait_is_bounded_by_the_timeout() {
    Fake f; f.walk_ripple_mv = 5;
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution), 0u);
    bool walk7 = false;
    for (size_t i = 0; i + 5 < f.choices.size(); ++i)
        if (walk_requests_at(f, i, 7, 13000)) walk7 = true;
    CHECK_EQ(walk7, true);
}
// Review item D: the walk recorded ONE sample per level and the sweep ONE sample per point. A source in burst mode swings every sample
// by tens of mV around a level it holds well, so one sample read a ripple, not the level: the walk's first move read +100 mV (a jump,
// "step resolution") and every sweep point carried the swing as its error. Both now record the mean of the samples of the last
// kWalkSteadyWindowMs. noise_flip puts two neighbouring levels on opposite sides of the swing: one sample each read 100 mV apart.
static void test_pps_step_walk_records_the_mean_of_a_noisy_level() {
    Fake f; f.noise_mv = 40; f.noise_flip = true;
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution), 0u);
    bool walk7 = false;
    for (size_t i = 0; i + 5 < f.choices.size(); ++i)
        if (walk_requests_at(f, i, 7, 13000)) walk7 = true;
    CHECK_EQ(walk7, true);   // all six walk Requests went out
}
static void test_pps_deep_sweep_records_the_mean_of_a_noisy_point() {
    Fake f; f.noise_mv = 40;
    ct::Runner r(f);
    ct::Options o{.selected = ct::bit(A::Pps)}; o.pps_sweep = true;
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 900000);
    CHECK_EQ(r.state(), ct::State::Done);
    const auto v = r.sweep_view();
    const auto* rw7 = v.find(uint8_t(A::Pps), 7);
    CHECK_EQ(rw7 != nullptr, true);
    if (!rw7) return;
    CHECK_EQ(rw7->sum.points, 161u);
    CHECK_EQ(int(rw7->sum.worst_err) >= -10 && int(rw7->sum.worst_err) <= 10, true);   // the level, not the +-40 mV swing
    CHECK_EQ(int(rw7->sum.max_err) <= 10 && int(rw7->sum.min_err) >= -10, true);
}
// ROADMAP "PPS step walk judged by total + trend" (hardware 2026-11-10, runs/2026-11-10-pps13): a good PPS source's
// output keeps creeping up 20-36 mV for ~2 s after each PS_RDY, so one 20 mV step reads anywhere from <= 0 to ~+36 mV
// while the 5-step total is right (~95 mV). The walk is judged by the total and the upward trend of all six levels; only
// a jump of 2 x kStepMv or more (coarse rounding) fails a single step.
static unsigned walk_fail_rows(const ct::Runner& r, int detail) {   // step-resolution rows with this detail
    unsigned n = 0;
    for (const auto& s : r.steps())
        if (s.reason == ct::Reason::StepResolution && int(s.verdict) == int(ct::Verdict::Fail) && s.detail == detail) ++n;
    return n;
}
static void run_walk_script(Fake& f, ct::Runner& r) {
    r.start(f.caps, kPd, f.in, true);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
}
static void test_pps_step_walk_passes_a_drifting_source() {
    // the re-run of the CUKTECH-class source: levels 13035, 13039, 13075, 13079, 13109, 13130 (moves +4 +36 +4 +30 +21,
    // total 95); and a creeping read where one step reads below zero (+25 +36 -3 +30 +12, total 100)
    const std::vector<std::vector<int>> seqs = {{4, 36, 4, 30, 21}, {25, 36, -3, 30, 12}};
    for (const auto& seq : seqs) {
        Fake f; f.walk_deltas = seq;
        ct::Runner r(f); run_walk_script(f, r);
        CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution), 0u);
        bool walk7 = false;
        for (size_t i = 0; i + 5 < f.choices.size(); ++i)
            if (walk_requests_at(f, i, 7, 13000)) walk7 = true;
        CHECK_EQ(walk7, true);   // all six Requests went out and the row passed
    }
}
static void test_pps_step_walk_passes_a_slow_but_correct_source() {
    Fake f; f.walk_deltas = {15, 25, 18, 22, 20};   // every step off by up to 5 mV, the total is exactly 100
    ct::Runner r(f); run_walk_script(f, r);
    CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution), 0u);
}
static void test_pps_step_walk_fails_a_flat_source_as_steps_ignored() {
    Fake f; f.walk_deltas = {0, 0, 0, 0, 0};
    ct::Runner r(f); run_walk_script(f, r);
    CHECK_EQ(walk_fail_rows(r, 0) >= 1u, true);   // detail 0 = the mean move: "steps ignored"
    CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution), walk_fail_rows(r, 0));
    ct::Step s{}; s.reason = ct::Reason::StepResolution; s.detail = 0;
    CHECK_STR(ct::explanation(s), "steps ignored");
}
static void test_pps_step_walk_fails_a_source_that_rounds_to_100mv_on_the_jump() {
    Fake f; f.walk_deltas = {0, 0, 0, 0, 100};   // a 100 mV quantum: nothing, nothing, nothing, nothing, +100
    ct::Runner r(f); run_walk_script(f, r);
    CHECK_EQ(walk_fail_rows(r, 100) >= 1u, true);   // the jump fails the step, detail = the move (the total alone passes)
    CHECK_EQ(count_reason_all(r, ct::Reason::StepResolution), walk_fail_rows(r, 100));
}
// A PD source that goes dark mid-row (its output collapsed or we Hard Reset it): VBUS 0, the offer list gone, no
// session. Lenovo C135, hardware 2026-11-03: VBUS drained 20 V -> 0 in 9 s, then every later row said "offers changed".
static void run_to_pd_row(Fake& f, ct::Runner& r, size_t row) {
    for (unsigned n = 0; n < 600 && !(r.index() == row && r.state() == ct::State::Holding); ++n) f.tick(r, 100);
    CHECK_EQ(r.index(), row); CHECK_EQ(r.state(), ct::State::Holding);
}
static void go_dark(Fake& f, ct::Runner& r, unsigned dark_ms, bool hard_reset) {
    if (hard_reset) ++f.in.pd.source_hard_resets; else ++f.in.session;
    f.caps.valid = false;
    for (unsigned n = 0; n < dark_ms; n += 100) { f.in.vbus_mv = 0; f.in.pd.state = pd::SinkState::Idle; f.in.pd.held = false; f.in.pd.mv = 5000; f.tick(r, 100); }
}
static void come_back(Fake& f) {
    f.in.vbus_mv = 5000; f.caps.valid = true; ++f.in.session;
    f.in.pd.state = pd::SinkState::Ready; f.in.pd.result = pd::SinkResult::Ok; f.in.pd.pos = 1; f.in.pd.mv = 5000;
}
static void test_dark_source_that_returns_is_not_a_change() {
    for (bool hard : {true, false}) for (unsigned dark_ms : {3000u, 6000u, 9000u}) {
        Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
        run_to_pd_row(f, r, 3);
        go_dark(f, r, dark_ms, hard);
        come_back(f);
        f.tick(r, 150000);
        CHECK_EQ(r.state(), ct::State::Done);
        for (const auto& st : r.steps()) { CHECK_EQ(st.reason == ct::Reason::CapsChanged, false); CHECK_EQ(st.verdict == ct::Verdict::Skip, false); }
        unsigned pass = 0;
        for (const auto& st : r.steps()) pass += st.group == ct::Group::Pd && st.verdict == ct::Verdict::Pass;
        CHECK_EQ(pass >= 6, true);   // the row that was cut is asked again, the rows behind it still run
    }
}
// CUKTECH C1, 2026-11-04: the Xiaomi auth unlock re-announces the PPS APDOs mid-run with the current
// limit raised (5 A -> 6 A). Same type and voltage range, only the current grew: the planned rows are
// still covered by the new list, so this is not "offers changed" and the run completes.
static void test_apdo_current_grow_is_not_offers_changed() {
    Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    run_to_pd_row(f, r, 3);
    for (unsigned n = 5; n <= 6; ++n) f.caps.pdos[n] = pd::decode_pdo((f.caps.pdos[n].raw & ~0x7Fu) | 0x78u);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    for (const auto& st : r.steps()) CHECK_EQ(st.reason == ct::Reason::CapsChanged, false);
    unsigned pass = 0;
    for (const auto& st : r.steps()) pass += st.group == ct::Group::Pd && st.verdict == ct::Verdict::Pass;
    CHECK_EQ(pass >= 6, true);
}
static void test_dark_source_that_never_returns_is_source_lost() {
    for (bool hard : {true, false}) {
        Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
        run_to_pd_row(f, r, 3);
        go_dark(f, r, 40000, hard);
        CHECK_EQ(r.state(), ct::State::Stopped);
        CHECK_EQ(r.reason(), ct::Reason::Lost);
        for (const auto& st : r.steps()) CHECK_EQ(st.reason == ct::Reason::CapsChanged, false);   // not "offers changed": nothing is offered
    }
}
static void test_pd_row_cut_twice_by_hard_resets_fails_and_the_run_goes_on() {
    // pattern src-reset-after-pps: the row is cut, asked again, cut again. It fails with the reason, the rest still runs.
    Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    run_to_pd_row(f, r, 3);
    go_dark(f, r, 2500, true); come_back(f);
    for (unsigned n = 0; n < 600 && !(r.index() == 3 && r.state() == ct::State::Holding); ++n) f.tick(r, 100);
    CHECK_EQ(r.index(), 3u);   // the retry is under way
    go_dark(f, r, 2500, true); come_back(f);
    f.tick(r, 150000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(r.steps()[3].verdict, ct::Verdict::Fail);
    CHECK_EQ(r.steps()[3].reason == ct::Reason::HardReset || r.steps()[3].reason == ct::Reason::Lost, true);
    unsigned pass = 0;
    for (const auto& st : r.steps()) pass += st.group == ct::Group::Pd && st.verdict == ct::Verdict::Pass;
    CHECK_EQ(pass >= 5, true);
}
// An ordinary (non-hunt) PD row answered by a Hard Reset the SOURCE sends. The sink counts that in source_hard_resets only (hardware
// 2026-11-07): the runner must still see it, handle it once (one retry, or the HardReset verdict after the second) and never both.
static unsigned requests_of(const Fake& f, const ct::Step& s) {
    unsigned n = 0; for (const auto& c : f.choices) n += c.pos == s.pos && c.mv == s.mv && c.ma == s.ma;
    return n;
}
static void test_ordinary_pd_row_answered_by_a_source_hard_reset_is_retried_once() {
    Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    run_to_pd_row(f, r, 3);
    f.reset_at = f.in.now + 100;
    f.tick(r, 150000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.in.pd.hard_resets, 0u); CHECK_EQ(f.in.pd.source_hard_resets, 1u);
    CHECK_EQ(r.steps()[3].verdict, ct::Verdict::Pass);   // asked again after the source came back: the reset did not fail it
    CHECK_EQ(requests_of(f, r.steps()[3]), 2u);          // the original and exactly one retry, not one per poll that saw the reset
    for (const auto& st : r.steps()) { CHECK_EQ(st.verdict == ct::Verdict::Fail, false); CHECK_EQ(st.reason == ct::Reason::CapsChanged, false); }
}
static void test_ordinary_pd_row_cut_by_a_second_source_hard_reset_fails_with_the_reason() {
    Fake f; ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    run_to_pd_row(f, r, 3);
    f.reset_at = f.in.now + 100;
    for (unsigned n = 0; n < 900 && !(f.in.pd.source_hard_resets == 1 && r.index() == 3 && r.state() == ct::State::Holding); ++n) f.tick(r, 100);
    CHECK_EQ(f.in.pd.source_hard_resets, 1u);
    f.reset_at = f.in.now + 100;   // the retry of the same row is cut as well
    f.tick(r, 150000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.in.pd.source_hard_resets, 2u);
    CHECK_EQ(r.steps()[3].verdict, ct::Verdict::Fail); CHECK_EQ(r.steps()[3].reason, ct::Reason::HardReset);
    CHECK_EQ(requests_of(f, r.steps()[3]), 2u);
    unsigned fail = 0, pass = 0;
    for (const auto& st : r.steps()) { fail += st.verdict == ct::Verdict::Fail; pass += st.group == ct::Group::Pd && st.verdict == ct::Verdict::Pass; }
    CHECK_EQ(fail, 1u); CHECK_EQ(pass >= 5, true);
}
static void test_pps_low_step_landing_below_the_band_fails_only_that_row() {
    // pattern no-psrdy-on-downstep: a PPS list that starts at 3.3 V; the source's output sits at 2.1 V on that step.
    // The row fails on its own, the rows and groups behind it are not skipped.
    Fake f; f.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 64);
    f.low_mv_for_3v3 = 2100;
    ct::Runner r(f); r.start(f.caps, {.selected = ct::bit(A::Pps)}, f.in, true);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    unsigned skipped = 0, failed = 0, low = 0, pass = 0;
    for (const auto& st : r.steps()) {
        skipped += st.verdict == ct::Verdict::Skip && st.group == ct::Group::Pd && st.pos == 6;
        failed += st.verdict == ct::Verdict::Fail; pass += st.verdict == ct::Verdict::Pass;
        low += st.mv == 3300 && st.verdict == ct::Verdict::Fail;
    }
    CHECK_EQ(low, 1u); CHECK_EQ(failed, 1u); CHECK_EQ(skipped, 0u); CHECK_EQ(pass >= 5, true);
}
// pattern latch-off-low-pps: the risky low edge of a programmable range runs last, so a source that dies there costs that row.
static ct::Options kLow3v3 = {.selected = ct::bit(A::Pd3Fixed) | ct::bit(A::Pps) | ct::bit(A::Qc2) | ct::bit(A::Afc) | ct::bit(A::Fcp)};
static void low_pps_caps(Fake& f) { f.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 64); }
static bool low_pps(const ct::Step& s) { return s.group == ct::Group::Pd && s.algorithm == A::Pps && s.mv < 5000; }
static void test_low_programmable_rows_are_planned_last() {
    {
        Fake f; low_pps_caps(f);
        ct::Options o = kLow3v3;
        ct::Runner r(f); r.start(f.caps, o, f.in, true);
        const auto st = r.steps();
        size_t first = st.size(), n_low = 0, last_other = 0;
        for (size_t i = 0; i < st.size(); ++i) {
            if (low_pps(st[i])) { first = std::min(first, i); ++n_low; } else last_other = i;
        }
        CHECK_EQ(n_low >= 1, true);
        // Every low programmable row sits behind every other row (the PdLow unit is the last one of the default order).
        for (size_t i = first; i < st.size(); ++i) CHECK_EQ(low_pps(st[i]), true);
        CHECK_EQ(last_other < first, true);
        CHECK_EQ(st[first].continuous, false);   // starts from a released 5 V contract, not from the previous row
        for (size_t i = first + 1; i < st.size() && low_pps(st[i]); ++i) CHECK_EQ(st[i].continuous, true);
        // the preview lists the same rows, less the 6 A hunt rows this list does not advertise (the run drops them at request time)
        const auto pv = ct::Runner::preview(f.caps, o, f.in);
        CHECK_EQ(pv.total, unsigned(st.size() - hunt_rows(r).size()));
    }
}
static void test_latch_off_at_the_low_step_ends_done_with_a_reason() {
    for (uint32_t delay : {0u, 300u, 800u, 1500u}) {
        Fake f; low_pps_caps(f); f.latch_mv = 3300; f.latch_delay_ms = delay;
        ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
        f.tick(r, 100000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(r.reason(), ct::Reason::None);
        unsigned shut = 0, pass = 0, pending = 0, lost_skips = 0;
        for (const auto& s : r.steps()) {
            if (s.mv == 3300 && s.group == ct::Group::Pd) { CHECK_EQ(s.verdict, ct::Verdict::Fail); CHECK_EQ(s.reason, ct::Reason::ShutOff); }
            shut += s.reason == ct::Reason::ShutOff && s.verdict == ct::Verdict::Fail;
            pass += s.verdict == ct::Verdict::Pass;
            pending += s.verdict == ct::Verdict::Pending || s.verdict == ct::Verdict::Running;
            lost_skips += s.verdict == ct::Verdict::Skip && s.reason == ct::Reason::Lost;
        }
        CHECK_EQ(shut, 1u); CHECK_EQ(pending, 0u); CHECK_EQ(lost_skips, 0u);
        CHECK_EQ(pass >= 8, true);   // everything before the low step was proven
    }
}
static void test_latch_off_that_returns_is_not_shut_off() {
    Fake f; low_pps_caps(f); f.latch_mv = 3300;
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    for (unsigned n = 0; n < 1500 && !f.latched; ++n) f.tick(r, 100);
    CHECK_EQ(f.latched, true);
    f.tick(r, 4000); f.revive();   // a mains replug inside the recovery window
    f.tick(r, 60000);
    CHECK_EQ(r.state(), ct::State::Done);
    for (const auto& s : r.steps()) CHECK_EQ(s.reason == ct::Reason::ShutOff, false);
}
// The C135 on the 3.3 V row: the output is cut, comes back by itself with its own Hard Reset about 3 s later and announces
// its list again a little after that. Hardware run 2026-11-03: the sink asked for PDO 6 into the invalid list ("no source
// caps") and the row was recorded as a request failure. It must wait for the list and say what happened instead.
static void test_cut_that_recovers_on_its_own_is_recorded_as_such() {
    for (uint32_t caps_after : {0u, 700u, 2500u}) {
        Fake f; low_pps_caps(f); f.latch_mv = 3300; f.latch_delay_ms = 300; f.recover_after_ms = 3300; f.caps_after_ms = caps_after;
        ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
        f.tick(r, 200000);
        CHECK_EQ(r.state(), ct::State::Done);
        unsigned cut = 0, request_fail = 0, pass = 0;
        for (const auto& s : r.steps()) {
            if (s.mv == 3300 && s.group == ct::Group::Pd) { CHECK_EQ(s.verdict, ct::Verdict::Fail); CHECK_EQ(s.reason, ct::Reason::CutRecovered); }
            cut += s.verdict == ct::Verdict::Fail && s.reason == ct::Reason::CutRecovered;
            request_fail += s.verdict == ct::Verdict::Fail && s.reason == ct::Reason::Request;
            pass += s.verdict == ct::Verdict::Pass;
            CHECK_EQ(s.verdict == ct::Verdict::Pending || s.verdict == ct::Verdict::Running, false);
        }
        CHECK_EQ(cut, 1u); CHECK_EQ(request_fail, 0u); CHECK_EQ(pass >= 8, true);
    }
}
// The rows behind a recovered cut wait for the list: no PDO is requested while the source announces nothing.
static void test_no_request_into_an_invalid_list_after_a_recovered_cut() {
    Fake f; low_pps_caps(f); f.latch_mv = 3300; f.latch_delay_ms = 300; f.recover_after_ms = 3300; f.caps_after_ms = 2500;
    ct::Runner r(f); r.start(f.caps, kPd, f.in, true);
    f.tick(r, 200000);
    unsigned no_caps = 0;
    for (const auto& s : r.steps()) no_caps += s.detail == 128 + unsigned(pd::Err::NoCaps);
    CHECK_EQ(no_caps, 0u);
}
static void test_every_reason_has_a_name() {
    for (unsigned v = 1; v <= unsigned(ct::Reason::StepResolution); ++v) {   // 0 = None, shown as nothing
        const char* n = ct::name(static_cast<ct::Reason>(v));
        CHECK_EQ(n != nullptr && n[0] != 0, true);
    }
    CHECK_STR(ct::name(ct::Reason::Withdrawn), "withdrawn by source");
    CHECK_STR(ct::name(ct::Reason::WrongVoltage), "wrong voltage on 5V contract");
    CHECK_STR(ct::name(ct::Reason::NeedsLoad), "needs load");
    CHECK_STR(ct::name(ct::Reason::Floored), "floored above target");
    CHECK_STR(ct::name(ct::Reason::StepResolution), "step resolution");
    CHECK_EQ(unsigned(ct::Reason::NeedsLoad), 44u);
    CHECK_EQ(unsigned(ct::Reason::StepResolution), 46u);
}
static unsigned count_reason(const ct::Runner& r, ct::Group g, ct::Reason why) {
    unsigned n = 0;
    for (const auto& s : r.steps()) n += s.group == g && s.verdict == ct::Verdict::Skip && s.reason == why;
    return n;
}
static void test_every_failure_kind_has_a_capture_reason() {
    using R = ct::Reason; using T = k2::transcap::Reason;
    CHECK_EQ(int(ct::capture_reason(R::ShutOff)), int(T::ShutOff));
    CHECK_EQ(int(ct::capture_reason(R::CutRecovered)), int(T::ShutOff));
    CHECK_EQ(int(ct::capture_reason(R::NoReattach)), int(T::Other));   // the C135 3.3 V row: kept nothing before
    CHECK_EQ(int(ct::capture_reason(R::WrongVoltage)), int(T::Other));   // the 5 V contract held at the wrong voltage: the PS_RDY is the evidence
    CHECK_EQ(int(ct::capture_reason(R::NeedsLoad)), int(T::None));       // a skip, not a failure: no capture
    CHECK_EQ(int(ct::capture_reason(R::Floored)), int(T::Voltage));      // the low edge held above its target
    CHECK_EQ(int(ct::capture_reason(R::StepResolution)), int(T::Voltage));   // a +20 mV step not followed
    CHECK_EQ(int(ct::capture_reason(R::Timeout)), int(T::Timeout));
    CHECK_EQ(int(ct::capture_reason(R::HardReset)), int(T::HardReset));
    CHECK_EQ(int(ct::capture_reason(R::Request)), int(T::NoPsRdy));
    CHECK_EQ(int(ct::capture_reason(R::Lost)), int(T::Detach));
    CHECK_EQ(int(ct::capture_reason(R::Voltage)), int(T::Voltage));
    CHECK_EQ(int(ct::capture_reason(R::Cancelled)), int(T::None));     // not a failure of the charger
    CHECK_EQ(int(ct::capture_reason(R::ClassAOnly)), int(T::None));    // a Class A charger declining 20 V is a skip
    CHECK_EQ(int(ct::capture_reason(R::NotUnlocked)), int(T::None));   // a 6 A row the source never unlocked is a skip
    CHECK_STR(ct::name(R::NotUnlocked), "not unlocked");
    CHECK_EQ(int(R::NotUnlocked), 38);                                 // append-only: the wire and the card store the number
}

static ct::Options with_units(uint32_t selected, std::initializer_list<ct::Unit> l) {
    ct::Options o{.selected = selected};
    for (const ct::Unit u : l) o.units.add(u);
    return o;
}
// ---- Running grid order: the rows list the algorithms in the order the plan executes them ------
// A grid row's identity: a protocol row is its algorithm, a PPS / AVS row its algorithm and PDO slot (one row per APDO),
// a Mi 6A probe row 1000 + its level in mV (a hunt step belongs to no protocol row).
static int row_key(A a, bool hunt, unsigned mv, unsigned pos = 0) {
    if (hunt) return 1000 + int(mv);
    if (ct::kPpsAlgorithms & ct::bit(a)) return 100000 + int(a) * 32 + int(pos);
    return int(a);
}
static int step_key(const ct::Step& s) { return row_key(s.algorithm, s.hunt, s.mv, s.pos); }
static bool is_revision_step(const ct::Step& s) { return !s.hunt && s.algorithm >= A::Pd30Revision && s.algorithm <= A::Pd32Revision; }
static void push_first(std::vector<int>& out, int k) { if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k); }
// The rows of the plan in execution order: the step list is walked front to back by index, so first appearance = order of execution
// (revisions are no grid rows).
static std::vector<int> executed_keys(const ct::Runner& r) {
    std::vector<int> out;
    for (const auto& s : r.steps()) if (!is_revision_step(s)) push_first(out, step_key(s));
    return out;
}
// The rows of the Running verdict grid, top to bottom (the way ChargerTestPage::build_running_grid builds it).
static std::vector<int> grid_keys(const ct::Runner& r) {
    namespace rl = k2::result_layout;
    static rl::Layout l;
    rl::OemNotes oem;
    oem.headers = false; oem.fold = false;
    rl::build(l, r.steps(), oem);
    std::vector<int> out;
    for (size_t i = 0; i < l.row_count; ++i) {
        const auto& row = l.rows[i];
        if (row.probe) out.push_back(row_key(A::Count, true, row.mv));
        else if (row.algorithm != A::Count) out.push_back(row_key(row.algorithm, false, 0, row.pos));
    }
    return out;
}
// A row with nothing to show (an EPR step the cable held back) has no grid row, so the grid is the executed order minus such rows:
// every grid row is in `run`, in the same relative order, none twice.
static bool grid_follows(const std::vector<int>& grid, const std::vector<int>& run) {
    size_t at = 0;
    for (const int a : grid) {
        while (at < run.size() && run[at] != a) ++at;
        if (at == run.size()) return false;
        ++at;
    }
    return true;
}
static long key_at(const std::vector<int>& v, int k) { return std::find(v.begin(), v.end(), k) - v.begin(); }
// User report 2026-11-09: the grid listed QC2, QC3, QC3+, QC2B, QC3B, UFCS while the run went QC2, QC2B, QC3, QC3B, QC3+, UFCS
// (hardware journal, steps 23-48): the grid kept its own copy of the order, with the Class B rows after the Class A ones.
static void test_running_grid_lists_the_algorithms_in_the_order_the_plan_runs_them() {
    Fake f; f.caps = cuktech_caps(); ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, ct::Options{}, f.in, true), ct::Reason::None);
    const auto run = executed_keys(r), grid = grid_keys(r);
    CHECK_EQ(run.size() > 12, true);
    CHECK_EQ(grid_follows(grid, run), true);
    CHECK_EQ(grid.size() + 3 > run.size(), true);   // at most a few rows with nothing to show (a cable-limited EPR) are left out
    // the hardware order of the Class B rows, spelled out
    const auto at = [&](A a) { return key_at(run, int(a)); };
    CHECK_EQ(at(A::Qc2) + 1 == at(A::Qc2ClassB) && at(A::Qc2ClassB) < at(A::Qc3), true);
    CHECK_EQ(at(A::Qc3) + 1 == at(A::Qc3ClassB) && at(A::Qc3ClassB) < at(A::Qc3Plus), true);
}
// The user's unit order (remote `charger-units`, ct_start.py --units) is the order of the grid too.
static void test_running_grid_follows_the_users_unit_order() {
    for (const auto& order : {std::initializer_list<ct::Unit>{ct::Unit::Ufcs, ct::Unit::Qc3, ct::Unit::Pd, ct::Unit::Fcp, ct::Unit::Qc2},
                              std::initializer_list<ct::Unit>{ct::Unit::Pd, ct::Unit::Vooc, ct::Unit::Afc, ct::Unit::Qc2, ct::Unit::PdLow},
                              std::initializer_list<ct::Unit>{ct::Unit::Qc2, ct::Unit::Pd, ct::Unit::Qc3}}) {
        Fake f; f.caps = cuktech_caps(); ct::Runner r(f);
        ct::Options o = with_units(ct::kDefaultAlgorithms, order);
        CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
        const auto run = executed_keys(r), grid = grid_keys(r);
        CHECK_EQ(run.size() > 5, true);
        CHECK_EQ(grid_follows(grid, run), true);
        CHECK_EQ(grid.size() + 3 > run.size(), true);
    }
}
// Mi 6A probes are grid rows of their own, at the place the planner runs them: the hunt step follows its own APDO's steps, so with
// two APDOs the 11 V probe stands between the two PPS rows and the 20 V probe after the second one, before the next unit, in plan
// order; the grid holds no other order than the step list's.
static void test_running_grid_places_the_mi_probes_where_the_plan_runs_them() {
    Fake f; f.caps = cuktech_caps(); ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, ct::Options{}, f.in, true), ct::Reason::None);
    const auto run = executed_keys(r), grid = grid_keys(r);
    const int mi11 = row_key(A::Count, true, 11000), mi20 = row_key(A::Count, true, 20000);
    const int pps6 = row_key(A::Pps, false, 0, 6), pps7 = row_key(A::Pps, false, 0, 7);   // 3.3-11V, then 5-20V
    CHECK_EQ(key_at(run, mi11) < long(run.size()) && key_at(run, mi20) < long(run.size()), true);
    CHECK_EQ(key_at(grid, mi11) < long(grid.size()) && key_at(grid, mi20) < long(grid.size()), true);
    CHECK_EQ(key_at(grid, pps6) < key_at(grid, mi11) && key_at(grid, mi11) < key_at(grid, pps7) && key_at(grid, pps7) < key_at(grid, mi20), true);
    // the probes come before the legacy units
    CHECK_EQ(key_at(grid, mi20) < key_at(grid, int(A::Qc2)), true);
    // the same positions in the step list itself
    CHECK_EQ(key_at(run, pps6) < key_at(run, mi11) && key_at(run, mi11) < key_at(run, pps7) && key_at(run, pps7) < key_at(run, mi20)
             && key_at(run, mi20) < key_at(run, int(A::Qc2)), true);
    // the grid has no "Turbo" block ahead of PD: the first row is whatever the plan starts with
    CHECK_EQ(grid.front(), run.front());
    // a custom unit order moves the probes with their unit (the legacy unit first, PD last)
    Fake g; g.caps = cuktech_caps(); ct::Runner q(g);
    ct::Options o = with_units(ct::kDefaultAlgorithms, {ct::Unit::Qc2, ct::Unit::Pd});
    CHECK_EQ(q.start(g.caps, o, g.in, true), ct::Reason::None);
    const auto run2 = executed_keys(q), grid2 = grid_keys(q);
    CHECK_EQ(grid_follows(grid2, run2), true);
    CHECK_EQ(key_at(grid2, int(A::Qc2)) < key_at(grid2, mi11), true);
    CHECK_EQ(key_at(grid2, mi11) < long(grid2.size()), true);
}
// The independent oracle: drive the run and record which step is in flight. The index never goes backwards, so the order in which
// the rows were first worked on is the plan's order, and the grid built from the finished plan lists the rows in that order.
static void test_running_grid_matches_the_order_the_run_worked_through() {
    Fake f; f.caps = cuktech_caps(6000); ct::Runner r(f); f.rn = &r;
    CHECK_EQ(r.start(f.caps, ct::Options{}, f.in, true), ct::Reason::None);
    std::vector<int> worked;
    size_t last = 0; bool backwards = false;
    for (unsigned n = 0; n < 6000 && r.state() != ct::State::Done; ++n) {
        f.tick(r, 100);
        const auto st = r.state();
        if (st != ct::State::Requesting && st != ct::State::Holding) continue;
        const size_t i = r.index();
        if (i < r.steps().size()) {
            backwards |= i < last; last = std::max(last, i);
            if (!is_revision_step(r.steps()[i])) push_first(worked, step_key(r.steps()[i]));
        }
    }
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(backwards, false);
    CHECK_EQ(worked.size() > 8, true);
    CHECK_EQ(grid_follows(worked, grid_keys(r)), true);   // every row the run worked on is a grid row, in the grid's order
    CHECK_EQ(key_at(worked, row_key(A::Count, true, 11000)) < long(worked.size()), true);   // the Mi probes were worked on, among the PD rows
}
// A plan that grows at run time (the source's list arrives late, a re-announced list): the grid is read from the plan as it stands,
// so it still lists the rows in the order of the step list, probes included.
static void test_running_grid_follows_a_plan_that_is_rebuilt_during_the_run() {
    Fake f; f.caps = cuktech_caps(); f.caps.num_pdos = 6;   // the first list has no 5-20 V APDO: one probe row, the second comes with the list
    f.grown = cuktech_caps(6000); f.grow_after_ms = 400; f.contract_delay_ms = 100;
    ct::Runner r(f); f.rn = &r;
    CHECK_EQ(r.start(f.caps, with_units(kPd.selected, {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    const int mi20 = row_key(A::Count, true, 20000);
    const auto hunt20 = [&] { for (const auto& s : r.steps()) if (s.hunt && s.mv == 20000) return s.algorithm; return A::Count; };
    const A before = hunt20();
    const auto before_run = executed_keys(r), before_grid = grid_keys(r);
    CHECK_EQ(before, A::Pd3Fixed);                             // planned against the short list: the 20 V probe rides the fixed 20 V PDO
    CHECK_EQ(key_at(before_grid, mi20) < long(before_grid.size()), true);
    CHECK_EQ(key_at(before_grid, mi20) < key_at(before_grid, row_key(A::Pps, false, 0, 6)), true);   // its row stands among the fixed rows, before the PPS row
    CHECK_EQ(grid_follows(before_grid, before_run), true);
    for (unsigned n = 0; n < 6000 && r.state() != ct::State::Done; ++n) {
        f.tick(r, 100);
        CHECK_EQ(grid_follows(grid_keys(r), executed_keys(r)), true);
    }
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(hunt20(), A::Pps);                                // the rebuilt plan moved it onto the 5-20 V APDO
    const auto grid = grid_keys(r);
    CHECK_EQ(key_at(grid, mi20) < long(grid.size()), true);    // the rebuilt plan brought it, and the grid shows it after its APDO's row
    CHECK_EQ(key_at(grid, row_key(A::Pps, false, 0, 7)) < key_at(grid, mi20), true);
    CHECK_EQ(grid_follows(grid, executed_keys(r)), true);
}

// ---- unit lifecycle (docs/CHARGER_TEST_FLOW.md) -----------------------------------------------
static std::string seq_of(const Fake& f) { return std::string(f.seq.begin(), f.seq.end()); }
static unsigned count_unit_verdict(const ct::Runner& r, ct::Unit u, ct::Verdict v, ct::Reason why = ct::Reason::None, bool any = true) {
    unsigned n = 0;
    for (const auto& s : r.steps()) n += ct::unit_of(s) == u && s.verdict == v && (any || s.reason == why);
    return n;
}
// pattern usb-a-cuts-on-low-dpdm (CUKTECH 120W USB-A port, 2026-11-07): the port sagged VBUS to ~2 V while D+/D- were held low; the last
// row of the Fcp unit failed as Lost and Releasing waited for 5 V. The returning Rp (a new attach session, 80 ms later on hardware) arrives
// after that wait ended, in the Baseline of the next legacy unit: the tail of the same sag, not a new lost source.
static const ct::Options kFcpVooc{.selected = ct::bit(A::Fcp) | ct::bit(A::Vooc)};
static int fcp_rows(const ct::Runner& r) {
    int n = 0;
    for (const auto& s : r.steps()) n += ct::unit_of(s) == ct::Unit::Fcp;
    return n;
}
static void test_usb_a_sag_tail_session_is_not_source_lost() {
    for (const unsigned delay : {100u, 200u, 300u, 700u, 1500u}) {
        Fake f; f.caps = {}; ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, kFcpVooc, f.in, true), ct::Reason::None);
        f.drop_at = fcp_rows(r); f.late_session_ms = delay;   // the sag hits the last Fcp row
        f.tick(r, 120000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(r.reason() == ct::Reason::Lost, false);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Fcp, ct::Verdict::Fail, ct::Reason::Lost, false), 1u);   // the row that met the sag
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Vooc, ct::Verdict::Pass) > 0, true);                      // the next unit still ran
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Vooc, ct::Verdict::Skip, ct::Reason::Lost, false), 0u);
    }
}
// Same sag, but VBUS is back only part of the way (1-4.2 V) when the session shows up inside the window: wait for it, do not fail.
static void test_usb_a_sag_tail_waits_for_vbus_to_climb() {
    Fake f; f.caps = {}; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kFcpVooc, f.in, true), ct::Reason::None);
    f.drop_at = fcp_rows(r); f.late_session_ms = 1000000;   // no tail session of its own: the test injects it
    CHECK_EQ(run_until(f, r, [&] { return f.fc_requests >= f.drop_at && f.low_until == 0 && r.state() == ct::State::Baseline; }, 120000), true);
    f.in.vbus_mv = 3000; ++f.in.session;   // Rp back, VBUS still climbing
    f.tick(r, 1500);
    CHECK_EQ(r.state() == ct::State::Stopped, false);
    f.in.vbus_mv = 5000;
    f.tick(r, 120000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(r.reason() == ct::Reason::Lost, false);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Vooc, ct::Verdict::Pass) > 0, true);
}
// A real loss stays a loss: VBUS under 1 V when the new session shows up, or VBUS still low once the window has passed.
static void test_usb_a_sag_tail_with_dead_or_low_vbus_is_still_lost() {
    for (const int mv : {0, 3000}) {
        Fake f; f.caps = {}; ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, kFcpVooc, f.in, true), ct::Reason::None);
        f.drop_at = fcp_rows(r); f.late_session_ms = 1000000;
        CHECK_EQ(run_until(f, r, [&] { return f.fc_requests >= f.drop_at && f.low_until == 0 && r.state() == ct::State::Baseline; }, 120000), true);
        f.in.vbus_mv = mv; ++f.in.session;
        f.tick(r, ct::kSagAdoptMs + 1000);
        CHECK_EQ(r.state() == ct::State::Releasing || r.state() == ct::State::Stopped, true);   // the run is ending, not running on
        CHECK_EQ(r.reason(), ct::Reason::Lost);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Vooc, ct::Verdict::Pass), 0u);
    }
}
// pattern usb-a-cuts-on-low-dpdm, the mild form (JT-P18 USB-A): the row's own reset sags VBUS and drops the cable's Rp, the row
// still passes (the session change is adopted in the row), and the next reset's tail session lands in the following Baseline.
static void test_usb_a_midrow_session_opens_the_adopt_window() {
    Fake f; f.caps = {}; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kFcpVooc, f.in, true), ct::Reason::None);
    f.glitch_at = fcp_rows(r);   // the last Fcp row: a new session with VBUS still at the row's level (adopted, the row passes)
    CHECK_EQ(run_until(f, r, [&] { return f.fc_requests >= fcp_rows(r) && r.state() == ct::State::Baseline; }, 120000), true);
    ++f.in.session;              // the release's own sag: Rp back a moment later
    f.tick(r, 120000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(r.reason() == ct::Reason::Lost, false);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Fcp, ct::Verdict::Fail), 0u);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Vooc, ct::Verdict::Pass) > 0, true);
}
// The same port's sag can dip the fast VBUS reading under 1 V for an instant (JT-P18: 0.65 V): inside the window that waits too.
static void test_usb_a_sag_dip_under_1v_inside_the_window_waits() {
    Fake f; f.caps = {}; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kFcpVooc, f.in, true), ct::Reason::None);
    f.glitch_at = fcp_rows(r);
    CHECK_EQ(run_until(f, r, [&] { return f.fc_requests >= fcp_rows(r) && r.state() == ct::State::Baseline; }, 120000), true);
    f.in.vbus_mv = 3800; f.tick(r, 600);   // the sag starts with the session unchanged (JT-P18: "5V release failed" here)
    CHECK_EQ(r.state() == ct::State::Unsafe || r.state() == ct::State::Stopped, false);
    f.in.vbus_mv = 600; f.tick(r, 30);   // the dip
    CHECK_EQ(r.state() == ct::State::Stopped, false);
    f.in.vbus_mv = 5000; ++f.in.session;
    f.tick(r, 120000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Vooc, ct::Verdict::Pass) > 0, true);
}
// Outside a recovery nothing is adopted: a session change in a Baseline that no legacy row sag preceded is still source lost.
static void test_session_change_in_baseline_without_a_sag_is_still_lost() {
    Fake f; f.caps = {}; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kFcpVooc, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return f.fc_requests >= fcp_rows(r) && r.state() == ct::State::Baseline; }, 120000), true);
    ++f.in.session;
    f.tick(r, 3000);
    CHECK_EQ(r.state(), ct::State::Stopped);
    CHECK_EQ(r.reason(), ct::Reason::Lost);
}
// An explicit list runs exactly those units, in that order, each one detached, attached in its own mode, run and detached again.
static void test_custom_unit_list_runs_in_order() {
    Fake f; ct::Runner r(f);
    const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3), {ct::Unit::Qc3, ct::Unit::Qc2});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(seq_of(f) == "DLDP", true);   // two Legacy units in one PD-silent attach, then the run's closing detach and Pd re-attach
    CHECK_EQ(f.pd_requests, 0);
    CHECK_EQ(f.protocols.front(), fc::Proto::Qc3);   // Qc3 first, as listed
    CHECK_EQ(f.protocols.back(), fc::Proto::Qc2);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(f.in.vbus_mv, 5000);
}
// A Pd unit attaches in Pd mode and the PD rows take over the START caps only until the first attach that hears a list.
static void test_pd_unit_attaches_with_a_contract() {
    Fake f; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    f.tick(r, 100000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(seq_of(f) == "DPDP", true);
    CHECK_EQ(f.attaches_legacy, 0);
    for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Skip, true);
}
// A unit that fails does not stop the run: the next unit still runs.
static void test_a_failed_unit_does_not_stop_the_run() {
    Fake f; ct::Runner r(f);
    f.request_ok = false;   // the first unit's handshakes all fail; the charger behaves for the next one
    const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3), {ct::Unit::Qc2, ct::Unit::Qc3});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return r.unit() == ct::Unit::Qc3; }), true);
    f.request_ok = true;
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.attaches_legacy, 1);   // the failed unit did not end the PD-silent session
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Fail) > 0, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Fail), 0u);
}
// ---- journal helpers ---------------------------------------------------------------------------
static ct::Reason journaled_start(Fake& f, ct::Runner& r, cj::Journal& j, ct::Options o) {
    f.jr = &j; f.rn = &r; f.jopts = o;
    const auto reason = r.start(f.caps, o, f.in, true);
    j.begin(r.run_id(), o, r.evidence(), r.steps());
    j.meta({.units = o.units});                         // the UI journals the unit order too: a resumed run keeps it
    return reason;
}
// A fresh boot: new Fake and Runner, the journal read back and restored (nothing is requested).
struct JStore {   // the littlefs volume of a boot; the disk is the "flash" that survives a reboot
    k2::fs::Fs fs; cj::Journal j;
    explicit JStore(k2test::RamDisk& d) : fs(d, 96), j(fs) { bool fm = false; fs.mount_or_format(fm); }
};
struct Boot {
    k2test::RamDisk& fl; JStore js; Fake f; ct::Runner r; cj::Journal& j; cj::Replay rep;
    bool planned_ = false;   // this boot's /rd.rec said Reattach: the reboot was the charger test's planned re-attach
    explicit Boot(k2test::RamDisk& flash) : fl(flash), js(flash), r(f), j(js.j) {
        j.replay(rep);
        f.jr = &j; f.rn = &r;
        // rdcomeback::begin() reads the same record on the device; restore() takes the flag as its last argument.
        planned_ = rdintent::planned_reattach(rdintent::load(js.fs).kind);
        if (rep.found && rep.unfinished) {
            j.adopt(rep);   // the journal mirrors the run read back: later changes are written as changes to its section files
            f.jopts = rep.options;
            r.restore(rep.options, rep.evidence, rep.run_id, {rep.steps.data(), rep.count}, rep.phase, rep.phase_index, rep.resumed,
                      rep.attempt_index, rep.attempts, planned_);
        }
    }
};
static const ct::Step* find_step(const ct::Runner& r, size_t idx) { return &r.steps()[idx]; }

// Where a unit sits in the run's order (-1: not in it), and whether the rows follow that order.
static int rank_of(const ct::Runner& r, ct::Unit u) {
    const auto order = r.unit_order();
    for (size_t n = 0; n < order.size(); ++n) if (order[n] == u) return int(n);
    return -1;
}
static bool rows_in_unit_order(const ct::Runner& r) {
    int last = -1;
    for (const auto& s : r.steps()) {
        const int k = rank_of(r, ct::unit_of(s));
        if (k < last) return false;
        last = k;
    }
    return true;
}
// The default order (user decision 2026-11-06): the OEM vendor fast modes by company (Huawei, Samsung, OPPO / OnePlus / vivo),
// then USB PD (its rows end with the Xiaomi Mi 6A hunts), then the legacy / standard D+/D- protocols (QC, UFCS), the risky low
// PPS/AVS rows last of all. The rows of a run are laid out in the same order.
static void test_default_unit_order_is_vendor_modes_then_pd_then_legacy() {
    Fake f; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, ct::Options{.selected = ~0u}, f.in, true), ct::Reason::None);
    const ct::Unit want[] = {ct::Unit::Fcp, ct::Unit::Scp, ct::Unit::Afc, ct::Unit::Vooc, ct::Unit::SuperVooc, ct::Unit::SuperVooc65,
                             ct::Unit::Vivo, ct::Unit::Pd, ct::Unit::Qc2, ct::Unit::Qc3, ct::Unit::Qc3Plus, ct::Unit::Ufcs, ct::Unit::PdLow};
    const auto got = r.unit_order();
    CHECK_EQ(got.size(), std::size(want));
    for (size_t n = 0; n < got.size() && n < std::size(want); ++n) CHECK_EQ(int(got[n]), int(want[n]));
    CHECK_EQ(rows_in_unit_order(r), true);
    // Only the units a selection needs, in the same relative order.
    Fake g; ct::Runner s(g);
    const uint32_t some = ct::bit(A::Qc2) | ct::bit(A::Pd3Fixed) | ct::bit(A::Vooc) | ct::bit(A::Fcp) | ct::bit(A::Ufcs);
    CHECK_EQ(s.start(g.caps, ct::Options{.selected = some}, g.in, true), ct::Reason::None);
    const ct::Unit want2[] = {ct::Unit::Fcp, ct::Unit::Vooc, ct::Unit::Pd, ct::Unit::Qc2, ct::Unit::Ufcs, ct::Unit::PdLow};
    CHECK_EQ(s.unit_order().size(), std::size(want2));
    for (size_t n = 0; n < s.unit_order().size() && n < std::size(want2); ++n) CHECK_EQ(int(s.unit_order()[n]), int(want2[n]));
    CHECK_EQ(rows_in_unit_order(s), true);
    // A custom list keeps what the user chose (a Pd unit first, then a Legacy one).
    Fake h; ct::Runner t(h);
    CHECK_EQ(t.start(h.caps, with_units(ct::bit(A::Qc2) | ct::bit(A::Pd3Fixed), {ct::Unit::Pd, ct::Unit::Qc2}), h.in, true), ct::Reason::None);
    CHECK_EQ(rank_of(t, ct::Unit::Pd), 0); CHECK_EQ(rank_of(t, ct::Unit::Qc2), 1);
    CHECK_EQ(rows_in_unit_order(t), true);
}
// The sessions (docs/CHARGER_TEST_FLOW.md): the units run in the default order and every run of consecutive Legacy units shares one
// PD-silent attach; a detach plus fresh attach happens only where the session mode changes.
static void test_default_order_runs_vendor_modes_before_pd_before_legacy() {
    Fake f; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, ct::Options{.selected = ~0u}, f.in, true), ct::Reason::None);
    std::vector<ct::Unit> order;
    for (unsigned n = 0; n < 400000 && r.state() != ct::State::Done; n += 100) {
        f.tick(r, 100);
        if (r.unit() != ct::Unit::Count && (order.empty() || order.back() != r.unit())) order.push_back(r.unit());
    }
    CHECK_EQ(int(r.state()), int(ct::State::Done));
    const ct::Unit want[] = {ct::Unit::Fcp, ct::Unit::Scp, ct::Unit::Afc, ct::Unit::Vooc, ct::Unit::SuperVooc, ct::Unit::SuperVooc65,
                             ct::Unit::Vivo, ct::Unit::Pd, ct::Unit::Qc2, ct::Unit::Qc3, ct::Unit::Qc3Plus, ct::Unit::Ufcs};
    // (the fake source has no PPS/AVS step below 5 V, so PdLow itself only appears for a list that has such rows)
    CHECK_EQ(order.size(), std::size(want));
    for (size_t n = 0; n < order.size() && n < std::size(want); ++n) CHECK_EQ(int(order[n]), int(want[n]));
    CHECK_EQ(int(order.back()), int(ct::Unit::Ufcs));   // the run ends on a Legacy unit: the closing detach and Pd attach follow it
    CHECK_EQ(seq_of(f).substr(seq_of(f).size() - 2) == "DP", true);
    f.tick(r, 3000);
    CHECK_EQ(f.in.vbus_mv, 5000);
    CHECK_EQ(rows_in_unit_order(r), true);
}
// Vendor modes, then PD, then the standard protocols: three sessions, the PD one in the middle. The run ends on a Legacy unit, so
// its closing detach hands the sink back as a normal Pd attach.
static void test_pd_between_vendor_and_legacy_units() {
    Fake f; ct::Runner r(f);
    const ct::Options o{.selected = ct::bit(A::Fcp) | ct::bit(A::Vooc) | ct::bit(A::Pd3Fixed) | ct::bit(A::Qc2) | ct::bit(A::Qc3)};
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 300000);
    CHECK_EQ(int(r.state()), int(ct::State::Done));
    CHECK_EQ(seq_of(f) == "DLDPDLDP", true);
    int last = -1;
    for (const auto& s : f.records) {
        const int k = rank_of(r, ct::unit_of(s));
        CHECK_EQ(k >= last, true); last = std::max(last, k);
    }
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Fcp, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Pass) > 0, true);
    for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pending || s.verdict == ct::Verdict::Running, false);
    CHECK_EQ(rows_in_unit_order(r), true);
    CHECK_EQ(f.in.vbus_mv, 5000);
}
// The PD rows are planned again from the list the first attach hears. They sit in the middle of the rows now: the rows behind them
// move up or down with the new count, the order stays, and the journal that holds the plan is written again.
static void test_pd_replan_in_the_middle_keeps_the_rows_in_order() {
    Fake f; ct::Runner r(f);
    const pd::SourceCaps full = f.caps;
    f.caps.num_pdos = 2;   // 5 V and 9 V only from the first attach on
    const ct::Options o{.selected = ct::bit(A::Fcp) | ct::bit(A::Pd3Fixed) | ct::bit(A::Qc2)};
    CHECK_EQ(r.start(full, o, f.in, true), ct::Reason::None);
    const size_t planned = r.steps().size();
    f.tick(r, 300000);
    CHECK_EQ(int(r.state()), int(ct::State::Done));
    CHECK_EQ(r.steps().size() < planned, true);
    CHECK_EQ(f.plan_changes >= 1, true);
    CHECK_EQ(rows_in_unit_order(r), true);
    unsigned fixed = 0, qc2_pass = 0;
    for (const auto& s : r.steps()) { fixed += s.algorithm == A::Pd3Fixed && s.verdict != ct::Verdict::Skip; qc2_pass += s.algorithm == A::Qc2 && s.verdict == ct::Verdict::Pass; }
    CHECK_EQ(fixed, 2u); CHECK_EQ(qc2_pass, 3u);
    for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pending || s.verdict == ct::Verdict::Running, false);
}
// A START without a source list plans one placeholder row per PD unit; the first attach that hears a list replaces them and the
// journal follows: a reboot right after reads the same rows (group, algorithm, level) the run holds.
static void test_pd_replan_rewrites_the_journal() {
    k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
    Fake f; ct::Runner r(f);
    const ct::Options o{.selected = ct::bit(A::Fcp) | ct::bit(A::Pd3Fixed) | ct::bit(A::Qc2)};
    f.jr = &j; f.rn = &r; f.jopts = o;
    CHECK_EQ(r.start(pd::SourceCaps{}, o, f.in, true), ct::Reason::None);
    j.begin(r.run_id(), o, r.evidence(), r.steps());
    const size_t placeholder_rows = r.steps().size();
    CHECK_EQ(run_until(f, r, [&] { return f.plan_changes > 0; }, 100000), true);
    CHECK_EQ(r.steps().size() != placeholder_rows, true);
    cj::Replay rep; j.replay(rep);
    CHECK_EQ(rep.count, r.steps().size());
    for (size_t n = 0; n < rep.count && n < r.steps().size(); ++n) {
        CHECK_EQ(int(rep.steps[n].group), int(r.steps()[n].group));
        CHECK_EQ(int(rep.steps[n].algorithm), int(r.steps()[n].algorithm));
        CHECK_EQ(int(rep.steps[n].mv), int(r.steps()[n].mv));
    }
}
// A reboot in the middle of the first vendor unit resumes that unit and goes on in the same order: the rest of the vendor modes,
// PD, then the standard protocols.
static void test_resume_in_a_vendor_unit_goes_on_in_the_default_order() {
    k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
    Fake f; ct::Runner r(f);
    const ct::Options o{.selected = ct::bit(A::Fcp) | ct::bit(A::Vooc) | ct::bit(A::Pd3Fixed) | ct::bit(A::Qc2)};
    journaled_start(f, r, j, o);
    for (int n = 0; n < 4000 && f.phases.empty(); ++n) f.tick(r, 100);
    CHECK_EQ(f.phases.empty(), false);
    if (f.phases.empty()) return;
    CHECK_EQ(int(f.phases[0].first), int(ct::Phase::Handover));
    CHECK_EQ(int(ct::unit_of(r.steps()[f.phases[0].second])), int(ct::Unit::Fcp));
    Boot b(fl);
    CHECK_EQ(b.rep.unfinished, true); CHECK_EQ(b.r.resumable(), true);
    CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::None));
    b.f.tick(b.r, 400000);
    CHECK_EQ(int(b.r.state()), int(ct::State::Done));
    CHECK_EQ(b.r.unit_order().size(), 5u);
    CHECK_EQ(int(b.r.unit_order()[0]), int(ct::Unit::Fcp)); CHECK_EQ(int(b.r.unit_order()[1]), int(ct::Unit::Vooc));
    CHECK_EQ(int(b.r.unit_order()[2]), int(ct::Unit::Pd)); CHECK_EQ(int(b.r.unit_order()[3]), int(ct::Unit::Qc2));
    CHECK_EQ(int(b.r.unit_order()[4]), int(ct::Unit::PdLow));
    int last = -1;
    for (const auto& s : b.f.records) {
        const int k = rank_of(b.r, ct::unit_of(s));
        CHECK_EQ(k >= last, true); last = std::max(last, k);
    }
    CHECK_EQ(int(b.f.records.front().group), int(ct::Group::Fcp));
    CHECK_EQ(count_unit_verdict(b.r, ct::Unit::Pd, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(count_unit_verdict(b.r, ct::Unit::Qc2, ct::Verdict::Pass) > 0, true);
    for (const auto& s : b.r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pending || s.verdict == ct::Verdict::Running, false);
}
// A reboot in a PD request, with the vendor modes behind it and the standard protocols ahead: the finished units are not run again,
// the PD unit goes on in the live contract and the Legacy unit after it still gets its own attach.
static void test_resume_in_the_pd_unit_runs_the_legacy_units_after_it() {
    k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
    Fake f; ct::Runner r(f);
    const ct::Options o{.selected = ct::bit(A::Fcp) | ct::bit(A::Pd3Fixed) | ct::bit(A::Qc2)};
    journaled_start(f, r, j, o);
    auto in_pd_request = [&] {
        for (const auto& p : f.phases) if (p.first == ct::Phase::Request && ct::unit_of(r.steps()[p.second]) == ct::Unit::Pd) return true;
        return false;
    };
    for (int n = 0; n < 8000 && !in_pd_request(); ++n) f.tick(r, 100);   // Fcp's rows first, then the first PD request above 5 V
    CHECK_EQ(in_pd_request(), true);
    if (!in_pd_request()) return;
    Boot b(fl);
    CHECK_EQ(b.rep.unfinished, true); CHECK_EQ(b.r.resumable(), true);
    CHECK_EQ(count_unit_verdict(b.r, ct::Unit::Fcp, ct::Verdict::Pass) > 0, true);   // done before the cut, kept
    CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::None));
    b.f.tick(b.r, 400000);
    CHECK_EQ(int(b.r.state()), int(ct::State::Done));
    for (const auto& s : b.f.records) CHECK_EQ(int(s.group) != int(ct::Group::Fcp), true);   // the vendor unit is not asked again
    CHECK_EQ(count_unit_verdict(b.r, ct::Unit::Pd, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(count_unit_verdict(b.r, ct::Unit::Qc2, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(b.f.attaches_legacy, 1);
    CHECK_EQ(b.f.in.vbus_mv, 5000);
}
static void test_consecutive_legacy_units_share_one_attach() {
    Fake f; ct::Runner r(f);
    const auto o = with_units(ct::bit(A::Fcp) | ct::bit(A::Qc2) | ct::bit(A::Qc3), {ct::Unit::Fcp, ct::Unit::Qc2, ct::Unit::Qc3});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.attaches_legacy, 1);   // one PD-silent attach for all three
    CHECK_EQ(f.detaches, 2);          // the one before it and the run's closing one
    CHECK_EQ(seq_of(f) == "DLDP", true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Pass) > 0, true);
    // A Legacy unit followed by a Pd unit is a session boundary: detach, fresh Pd attach.
    Fake g; ct::Runner s(g);
    const auto p = with_units(ct::bit(A::Qc2) | ct::bit(A::Pd3Fixed), {ct::Unit::Qc2, ct::Unit::Pd});
    CHECK_EQ(s.start(g.caps, p, g.in, true), ct::Reason::None);
    g.tick(s, 200000);
    CHECK_EQ(s.state(), ct::State::Done);
    CHECK_EQ(seq_of(g) == "DLDPDP", true);
}
// The shared session is only kept while the line stays PD-quiet at 5 V: PD traffic between two Legacy units ends it with a
// detach and a fresh attach, so the next unit still runs.
static void test_legacy_session_that_sees_pd_traffic_is_renewed() {
    Fake f; ct::Runner r(f);
    const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3), {ct::Unit::Qc2, ct::Unit::Qc3});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return r.unit() == ct::Unit::Qc2 && f.fc_requests > 0; }, 60000), true);
    f.chatter_until = f.in.now + 3000;   // the charger starts sending caps again
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.attaches_legacy, 2);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Pass) > 0, true);
}
// A source that ignores UFCS but starts PD cable discovery on CC while the engine listens (Baseus FH22 charger, 2026-11-07: SOP' frames
// ~280 ms after the handshake, every run) must not turn the UFCS row into "PD active". The engine's own answer decides: no
// response. The rest of the unit's rows prove nothing either and carry the same reason.
static void test_silent_ufcs_source_reports_no_response_not_pd_active() {
    Fake f; ct::Runner r(f);
    const auto o = with_units(ct::bit(A::Ufcs), {ct::Unit::Ufcs});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return f.fc_requests > 0; }, 60000), true);
    f.in.fc_state = fc::State::Negotiating;
    f.chatter_until = f.in.now + 100000;   // PD frames on CC from now on (the cable query)
    f.tick(r, 300);
    CHECK_EQ(int(r.steps()[0].verdict), int(ct::Verdict::Running));   // the PD frames alone end nothing
    f.in.fc_state = fc::State::Failed; f.in.fc_fail = fc::Fail::NoResponse; f.in.fc_tries = 5; f.in.vbus_mv = 5000;
    f.tick(r, 1100);
    CHECK_EQ(int(r.steps()[0].verdict), int(ct::Verdict::Fail));
    CHECK_EQ(int(r.steps()[0].reason), int(ct::Reason::Request));
    CHECK_EQ(int(r.steps()[0].detail), int(fc::Fail::NoResponse));
    for (const auto& s : r.steps()) CHECK_EQ(int(s.reason), int(s.reason == ct::Reason::PdActive ? ct::Reason::None : s.reason));
    for (const auto& s : r.steps())
        if (s.group == ct::Group::Ufcs && s.verdict == ct::Verdict::Skip) {
            CHECK_EQ(int(s.reason), int(ct::Reason::Request));
            CHECK_EQ(int(s.detail), int(fc::Fail::NoResponse));
        }
}
// Waits: about a second after the PD-silent attach settles before the first handshake, 500 ms of D+/D- discharge between units,
// and 500 ms more before AFC.
static void test_legacy_waits_between_units() {
    Fake f; ct::Runner r(f);
    const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Afc) | ct::bit(A::Qc3), {ct::Unit::Qc2, ct::Unit::Afc, ct::Unit::Qc3});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return f.attaches_legacy == 1 && r.state() == ct::State::Baseline; }, 60000), true);
    const uint32_t t_attached = f.in.now;   // the PD-silent attach has settled
    CHECK_EQ(run_until(f, r, [&] { return f.fc_requests > 0; }, 10000), true);
    CHECK_EQ(f.in.now - t_attached >= ct::kLegacyFirstWaitMs, true);
    CHECK_EQ(f.in.now - t_attached < ct::kLegacyFirstWaitMs + 400, true);
    CHECK_EQ(int(f.protocols.front()), int(fc::Proto::Qc2));
    // AFC: the gap after QC2's release plus the AFC pre-wait.
    CHECK_EQ(run_until(f, r, [&] { return r.unit() == ct::Unit::Afc; }, 60000), true);
    const uint32_t t_afc = f.in.now; const int n0 = f.fc_requests;
    CHECK_EQ(run_until(f, r, [&] { return f.fc_requests > n0; }, 10000), true);
    CHECK_EQ(int(f.protocols.back()), int(fc::Proto::Afc));
    CHECK_EQ(f.in.now - t_afc >= ct::kLegacyGapMs + ct::kAfcPreMs, true);
    // The unit after AFC has no pre-wait beyond the gap.
    CHECK_EQ(run_until(f, r, [&] { return r.unit() == ct::Unit::Qc3; }, 60000), true);
    const uint32_t t_qc3 = f.in.now; const int n1 = f.fc_requests;
    CHECK_EQ(run_until(f, r, [&] { return f.fc_requests > n1; }, 10000), true);
    CHECK_EQ(f.in.now - t_qc3 >= ct::kLegacyGapMs, true);
    CHECK_EQ(f.in.now - t_qc3 < ct::kLegacyGapMs + ct::kAfcPreMs, true);
}
// A charger that keeps PD alive (re-sends its caps for ever): one longer retry, then every row of the unit is "blocked by PD"
// and the next unit still gets its own attach.
static void test_legacy_attach_that_never_falls_back_is_blocked_by_pd() {
    Fake f; ct::Runner r(f);
    f.stays_pd = true;
    const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3), {ct::Unit::Qc2, ct::Unit::Qc3});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    const uint32_t t0 = f.in.now;
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }, 400000), true);
    CHECK_EQ(f.in.now - t0 >= ct::kLegacyGiveUpMs + ct::kLegacyRetryMs, true);   // the retry really waited the longer time
    CHECK_EQ(f.fc_requests, 0);
    const auto qc2 = count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Skip, ct::Reason::BlockedByPd, false);
    CHECK_EQ(qc2 > 0, true);
    CHECK_EQ(qc2, count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Skip));
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Skip, ct::Reason::BlockedByPd, false) > 0, true);
    CHECK_EQ(r.handover().outcome, ct::HandoverOutcome::StayedPd);
    // Qc2: attach, give up, detach, retry attach; Qc3 got its own attach (the retry after a first block is shorter).
    CHECK_EQ(f.attaches_legacy >= 3, true);
    CHECK_EQ(f.detaches >= 4, true);
}
// The same charger that lets go of PD during the longer retry: the unit runs normally.
static void test_legacy_retry_that_falls_back_runs_the_unit() {
    Fake f; ct::Runner r(f);
    f.stays_pd = true;
    const auto o = with_units(ct::bit(A::Qc2), {ct::Unit::Qc2});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return f.attaches_legacy == 2; }, 100000), true);
    f.stays_pd = false;
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }, 200000), true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Skip, ct::Reason::BlockedByPd, false), 0u);
    CHECK_EQ(r.handover().outcome, ct::HandoverOutcome::FellBack);
}
// A source with a CJ21-style give-up (hard-reset cycles before it disables PD) is waited out within the first window.
static void test_legacy_attach_waits_out_hard_reset_cycles() {
    Fake f; ct::Runner r(f);
    f.give_up_ms = 12000;
    const auto o = with_units(ct::bit(A::Qc2), {ct::Unit::Qc2});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.attaches_legacy, 1);   // no retry needed
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(r.handover().outcome, ct::HandoverOutcome::FellBack);
}
// A source that goes quiet in the PD-silent attach and then revives PD on its own: it Hard Resets (VBUS leaves vSafe5V on its own,
// sagging through 3 V or all the way dark) and announces its caps again. That sag is the source's, not an unsafe release: the Baseline
// waits for the output, the caps that follow end the unit as PD-active, the next legacy unit meets the same PD (blocked), and the
// Pd unit still runs. The run is Done, never Unsafe (GaN3 65W, 2026-11-08: "unsafe (5V release failed)" before any row ran).
static void test_silent_attach_source_that_revives_pd_does_not_go_unsafe() {
    for (const uint32_t dark_ms : {600u, 1500u}) {   // how long VBUS sags to 3 V and the list stays dark
        Fake f; ct::Runner r(f); f.reset_dark_ms = dark_ms;
        const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Pd3Fixed), {ct::Unit::Qc2, ct::Unit::Qc3, ct::Unit::Pd});
        CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return f.handed && r.state() == ct::State::Baseline && r.unit() == ct::Unit::Qc2; }, 60000), true);
        f.chatter_until = f.in.now + 1000000;   // from here on the source sends caps again, in every PD-silent attach
        f.reset_at = f.in.now + 100;            // its Hard Reset, 100 ms into the Baseline
        bool unsafe = false;
        for (unsigned n = 0; n < 4000 && r.state() != ct::State::Done; ++n) {
            f.tick(r, 100);
            unsafe = unsafe || r.state() == ct::State::Unsafe;
        }
        CHECK_EQ(unsafe, false);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Skip, ct::Reason::PdActive, false) > 0, true);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Skip, ct::Reason::BlockedByPd, false) > 0, true);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Pass) > 0, true);
        CHECK_EQ(f.fc_requests, 0);
    }
}
// pattern silent-sink-output-off (GaN3 65W A2412-65CC, 2026-11-08): the source switches its output off when the sink stays silent on
// PD in a PD-silent attach, and only a PD attach brings it back. VBUS gone past the Baseline's adopt window before any legacy row
// ran is the charger's answer "PD only", not a lost or unsafe source: every legacy unit of the run is skipped as PdOnly (the later
// ones without another silent attach), the sink comes back as a PD attach and the Pd unit passes. Done, never Unsafe.
static void test_silent_sink_output_off_skips_legacy_units_and_runs_pd() {
    const std::array<std::array<ct::Unit, 3>, 2> orders = {{{ct::Unit::Qc2, ct::Unit::Qc3, ct::Unit::Pd}, {ct::Unit::Pd, ct::Unit::Qc2, ct::Unit::Qc3}}};
    for (const auto& order : orders) {
        Fake f; ct::Runner r(f); f.silent_off_ms = 3600;   // the attach is verified quiet at ~3 s: the output goes off in the first Baseline, before its handshake
        const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Pd3Fixed), {order[0], order[1], order[2]});
        CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
        bool unsafe = false;
        for (unsigned n = 0; n < 4000 && r.state() != ct::State::Done && r.state() != ct::State::Stopped; ++n) {
            f.tick(r, 100);
            unsafe = unsafe || r.state() == ct::State::Unsafe;
        }
        CHECK_EQ(unsafe, false);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Skip, ct::Reason::PdOnly, false) > 0, true);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Skip, ct::Reason::PdOnly, false) > 0, true);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Skip, ct::Reason::Lost, false), 0u);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Pass) > 0, true);
        CHECK_EQ(f.attaches_legacy, 1);   // the second legacy unit meets no second silent attach
        CHECK_EQ(seq_of(f).back(), 'P');  // the sink ends as a PD attach
        CHECK_EQ(f.fc_requests, 0);
    }
}
// The ride-out is bounded: VBUS that stays under vSafe5V past the adopt window ends the run (Unsafe, which then clears as Stopped once
// VBUS has stayed under vSafe5V for kUnsafeGoneMs: nothing elevated is left), and VBUS above vSafe5V never waits and never clears.
static void test_silent_baseline_sag_that_never_returns_is_still_unsafe() {
    for (const int mv : {3000, 7000}) {
        Fake f; ct::Runner r(f);
        const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Pd3Fixed), {ct::Unit::Qc2, ct::Unit::Qc3, ct::Unit::Pd});
        CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return f.handed && r.state() == ct::State::Baseline && r.unit() == ct::Unit::Qc2; }, 60000), true);
        f.in.vbus_mv = mv;
        f.tick(r, 300);
        CHECK_EQ(r.state() == ct::State::Unsafe, mv == 7000);   // 7 V: at once; 3 V: not yet, the window is open
        bool was_unsafe = false;
        for (uint32_t t = 0; t < ct::kRecoverMs + 1000; t += 100) { f.tick(r, 100); was_unsafe = was_unsafe || r.state() == ct::State::Unsafe; }   // bounded by kRecoverMs from the Baseline's start
        CHECK_EQ(was_unsafe, true);
        if (mv == 7000) CHECK_EQ(int(r.state()), int(ct::State::Unsafe));
        else {
            f.tick(r, ct::kUnsafeGoneMs + 500);   // 3 V holds nothing elevated: the run stops, no unplug asked for ever
            CHECK_EQ(int(r.state()), int(ct::State::Stopped));
        }
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Pass), 0u);
    }
}
// Runs a Qc2 -> Qc3 -> Pd order until the second legacy unit's Baseline: a legacy row of the run (Qc2's) has a verdict, none of the
// Qc3 unit's own has run.
static bool run_to_second_legacy_baseline(Fake& f, ct::Runner& r) {
    const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Pd3Fixed), {ct::Unit::Qc2, ct::Unit::Qc3, ct::Unit::Pd});
    if (r.start(f.caps, o, f.in, true) != ct::Reason::None) return false;
    return run_until(f, r, [&] { return f.handed && r.state() == ct::State::Baseline && r.unit() == ct::Unit::Qc3; }, 120000) &&
           count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Pass) > 0;
}
// OnePlus 120 W SUPERVOOC brick on USB-C, 2026-11-11 (runs/2026-11-11-integ-hw/oneplus-usbc): the vendor units ran in the PD-silent
// attach; at the next unit's Baseline the source took its Rp and its output away (VBUS 4.08 V, then bleeding down, no current, the
// session changed). That is the source's own cut, not an elevated source. The Baseline only ever waited that out before the first
// legacy row of the RUN: the earlier units' rows switched the wait off ("UNSAFE: unplug source / 5V release failed", every later unit
// skipped). The window belongs to the unit: this unit has run nothing, so the cut ends the unit as a lost source and the next unit
// gets its own attach.
static void test_later_unit_output_cut_in_its_baseline_is_a_lost_unit_not_unsafe() {
    Fake f; ct::Runner r(f);
    CHECK_EQ(run_to_second_legacy_baseline(f, r), true);
    f.tick(r, 300);
    CHECK_EQ(int(r.state()), int(ct::State::Baseline));
    f.in.pd_quiet_ms = 1000000;   // the session reading stays unchanged, as on hardware (the Rp is not seen as a new attach)
    bool unsafe = false; uint32_t cut_t = 0;
    for (unsigned n = 0; n < 6000 && r.state() != ct::State::Done && r.state() != ct::State::Stopped; ++n) {
        if (r.unit() == ct::Unit::Qc3 && r.state() != ct::State::Detach) {   // 4.08 V, bleeding to 0.6 V over ~6 s
            cut_t += 100;
            f.in.vbus_mv = std::max(600, 4080 - int(cut_t) * 58 / 100);
        }
        f.tick(r, 100);
        unsafe = unsafe || r.state() == ct::State::Unsafe;
    }
    CHECK_EQ(unsafe, false);
    CHECK_EQ(int(r.state()), int(ct::State::Done));
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Skip, ct::Reason::Lost, false) > 0, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Skip, ct::Reason::Release, false), 0u);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Pass) > 0, true);   // what ran before stays
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Pass) > 0, true);    // the next unit got its own attach
}
// What stays: a later unit's Baseline that finds VBUS ABOVE vSafe5V is an elevated source (Unsafe at once), and VBUS that sits under
// vSafe5V for good (a decay that stops at 3 V) still ends the run Unsafe -> Stopped after the window, as for the first unit.
static void test_later_unit_elevated_or_stuck_baseline_still_ends_unsafe() {
    for (const int mv : {3000, 7000}) {
        Fake f; ct::Runner r(f);
        CHECK_EQ(run_to_second_legacy_baseline(f, r), true);
        f.in.vbus_mv = mv;
        f.tick(r, 300);
        CHECK_EQ(r.state() == ct::State::Unsafe, mv == 7000);
        bool was_unsafe = false;
        for (uint32_t t = 0; t < ct::kRecoverMs + 1000; t += 100) { f.tick(r, 100); was_unsafe = was_unsafe || r.state() == ct::State::Unsafe; }
        CHECK_EQ(was_unsafe, true);
        if (mv == 7000) CHECK_EQ(int(r.state()), int(ct::State::Unsafe));
        else {
            f.tick(r, ct::kUnsafeGoneMs + 500);
            CHECK_EQ(int(r.state()), int(ct::State::Stopped));
        }
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Pass), 0u);
    }
}
// A Pd unit whose attach never brings VBUS or the source list back fails its rows with NoReattach; the run still ends. A
// source that is back with its list but has not yet granted the 5 V contract is not a failure: the rows go on.
static void test_pd_attach_without_vbus_or_caps_is_no_reattach() {
    for (int mode = 0; mode < 3; ++mode) {
        Fake f; ct::Runner r(f);
        if (mode == 0) f.no_vbus_after_rd = true;
        if (mode == 1) f.silent_after_rd = true;
        if (mode == 2) f.no_contract = true;
        CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }, 200000), true);
        unsigned noreattach = 0;
        for (const auto& s : r.steps()) {
            CHECK_EQ(s.verdict == ct::Verdict::Pending || s.verdict == ct::Verdict::Running, false);
            if (s.reason == ct::Reason::NoReattach) {
                ++noreattach;
                // a genuine NoReattach records the live VBUS at the fail (5000 with a silent source, 0 when the
                // source never brought VBUS up), not a clamped 0
                CHECK_EQ(s.measured_mv, mode == 0 ? 0u : 5000u);
            }
        }
        if (mode < 2) { CHECK_EQ(noreattach > 0, true); CHECK_EQ(f.pd_requests, 0); }
        else { CHECK_EQ(noreattach, 0u); CHECK_EQ(f.pd_requests > 0, true); }
        CHECK_EQ(f.rd_on, true);
    }
}
// Reason::WrongVoltage (#43, after Withdrawn): a 5 V contract (PS_RDY at PDO 1) held at the wrong voltage
// on VBUS when kAttachContractMs runs out (pattern shared-bus-split).
constexpr ct::Reason kWrongVoltage = ct::Reason::WrongVoltage;
// pattern shared-bus-split (2026-11-09, runs/2026-11-09-splitter): the attach's 5 V contract is granted
// (PS_RDY at PDO 1) but the source holds 7.6 V on VBUS the whole time (the other output's PPS level),
// decaying to 1.63 V before the timeout. When kAttachContractMs runs out the unit's rows must fail with
// WrongVoltage and the HIGHEST VBUS seen during the wait (7600), not the reading at the timeout (1633).
// Today they fail NoReattach with 1633 (red).
static void test_pd_attach_5v_contract_held_at_wrong_voltage_fails_wrong_voltage() {
    Fake f; ct::Runner r(f);
    f.attach_wrong_mv = 7600; f.wrong_decay_rel = 2000; f.wrong_decay_mv = 1633;
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }, 200000), true);
    unsigned wrong = 0;
    for (const auto& s : r.steps()) {
        CHECK_EQ(s.verdict == ct::Verdict::Pending || s.verdict == ct::Verdict::Running, false);
        if (s.group != ct::Group::Pd) continue;
        CHECK_EQ(s.verdict, ct::Verdict::Fail);
        CHECK_EQ(s.reason, kWrongVoltage);
        CHECK_EQ(s.measured_mv, 7600u);   // the highest of the wait, not the 1633 read at the timeout
        ++wrong;
    }
    CHECK_EQ(wrong > 0, true);
    CHECK_EQ(count_reason_all(r, ct::Reason::NoReattach), 0u);
}
// The streamB2 replay (2026-11-09, runs/2026-11-09-splitter/streamB2/timeline.txt): the attach's 5 V contract is
// granted at 7.6 V (PS_RDY at PDO 1, the other output's PPS level) and ~2 s into the 3 s kAttachContractMs wait
// the splitter cuts VBUS and Rp and never comes back. PS_RDY was SEEN during the wait, so the rows must fail
// WrongVoltage with the highest VBUS of the wait (7600) - not NoReattach. Today the teardown wins: the Rp-restart
// re-arms the wait, the contract is gone at the timeout, and the rows fail NoReattach (red).
static void test_pd_attach_5v_contract_then_rp_cut_before_timeout_fails_wrong_voltage() {
    Fake f; ct::Runner r(f);
    f.attach_wrong_mv = 7600;   // the contract lands at 7.6 V
    f.rp_drop_rel = 2700;       // VBUS+Rp cut 2 s into the 3 s wait (the caps arrived at back_ms 700)
    // rp_back_rel = 0: the source never comes back
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }, 200000), true);
    unsigned wrong = 0;
    for (const auto& s : r.steps()) {
        CHECK_EQ(s.verdict == ct::Verdict::Pending || s.verdict == ct::Verdict::Running, false);
        if (s.group != ct::Group::Pd) continue;
        CHECK_EQ(s.verdict, ct::Verdict::Fail);
        CHECK_EQ(s.reason, kWrongVoltage);
        CHECK_EQ(s.measured_mv, 7600u);   // the highest of the wait, latched before the cut
        ++wrong;
    }
    CHECK_EQ(wrong > 0, true);
    CHECK_EQ(count_reason_all(r, ct::Reason::NoReattach), 0u);
}
// No caps at all after the attach: still NoReattach ("no PD after re-attach"), never WrongVoltage - a contract
// that was never granted cannot hold a wrong voltage.
static void test_pd_attach_without_any_caps_stays_no_reattach() {
    Fake f; ct::Runner r(f);
    f.silent_after_rd = true;   // the source answers the Rd with VBUS but never announces a list
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }, 200000), true);
    CHECK_EQ(count_reason_all(r, ct::Reason::NoReattach) > 0, true);
    CHECK_EQ(count_reason_all(r, kWrongVoltage), 0u);
}
// pattern slow-rp-wake: during the attach wait the source's Rp drops (caps gone, no active CC, VBUS dark)
// 1.5 s after the caps and its standby pull-up brings fresh caps back 3.2 s after the caps - past
// kAttachContractMs (3000 ms) from the first caps. The wait restarts once at the drop, so the unit runs.
// Today the timer runs on and the unit fails NoReattach at 3.0 s (red).
static void test_pd_attach_rp_drop_restarts_the_wait_once() {
    Fake f; ct::Runner r(f);
    f.no_contract = true;      // the 5 V contract is still being negotiated when the Rp drops
    f.rp_drop_rel = 2200;      // 1.5 s after the caps (back_ms 700 + 1500)
    f.rp_back_rel = 3900;      // 3.2 s after the caps
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Done; }, 200000), true);
    CHECK_EQ(count_reason_all(r, ct::Reason::NoReattach), 0u);
    CHECK_EQ(count_reason_all(r, kWrongVoltage), 0u);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Pass) > 0, true);   // the unit ran
}
// (B5, pattern shared-bus-split; the streamB2 replay values of runs/2026-11-09-splitter/streamB2 - PS_RDY at PDO 1
// with VBUS held at 7600 - and the B5 run runs/2026-11-10-splitter-wrongv, VBUS 8470): with a live PDO 1 contract and
// VBUS outside vSafe5V, START's preflight refuses WrongVoltage, and the refusal's log line names the measured level
// ("VBUS 8.5 V on a 5 V contract (shared bus)") - not Release with a bare mV. Without the contract (a source holding
// 7.6 V with no PD) it stays Release, and a safe 5 V under the contract runs.
static void test_preflight_pdo1_contract_at_wrong_voltage_refuses_wrong_voltage() {
    Fake f;
    f.in.vbus_mv = 7600;   // streamB2 replay: the PDO 1 contract is granted, VBUS stays at 7.6 V
    f.in.pd.state = pd::SinkState::Ready; f.in.pd.pos = 1; f.in.pd.mv = 5000;
    const auto pv = ct::Runner::preview(f.caps, kPd, f.in);
    CHECK_EQ(pv.refusal, ct::Reason::WrongVoltage);
    CHECK_EQ(pv.refusal_mv, 7600u);   // the measured level rides with the refusal (the card/summary records it)
    {   // the recorded refusal: the remote summary reads reason(), the log line names the level
        ct::Runner r(f);
        g_stub.log_text.clear();
        r.note_preflight_refusal(pv.refusal, 7600);
        CHECK_EQ(r.reason(), ct::Reason::WrongVoltage);
        CHECK_EQ(r.refusal_mv(), 7600u);
        CHECK_EQ(g_stub.log_text.find("[TEST] preflight refused: VBUS 7.6 V on a 5 V contract (shared bus)") != std::string::npos, true);
        char buf[32];
        CHECK_EQ(ct::refusal_footer_text(ct::Reason::WrongVoltage, 8470, buf), 27u);   // the B5 run's 8.47 V reads 8.5 V
        CHECK_STR(buf, "VBUS 8.5 V on a 5V contract");
        CHECK_EQ(ct::refusal_footer_text(ct::Reason::WrongVoltage, 8960, buf), 27u);   // rounds up across the volt: 9.0 V, not 8.0 V
        CHECK_STR(buf, "VBUS 9.0 V on a 5V contract");
    }
    {   // no contract at the same voltage: a source with no PD at 7.6 V is a plain unsafe VBUS
        Fake f2; f2.in.vbus_mv = 7600;
        const auto pv2 = ct::Runner::preview(f2.caps, kPd, f2.in);
        CHECK_EQ(pv2.refusal, ct::Reason::Release);
        CHECK_EQ(pv2.refusal_mv, 7600u);
    }
    {   // a safe 5 V under the PDO 1 contract: nothing to refuse
        Fake f3; f3.in.vbus_mv = 5000;
        f3.in.pd.state = pd::SinkState::Ready; f3.in.pd.pos = 1; f3.in.pd.mv = 5000;
        CHECK_EQ(ct::Runner::preview(f3.caps, kPd, f3.in).refusal, ct::Reason::None);
    }
}
// The PD plan comes from the list the first attach hears, not from START's.
static void test_pd_plan_comes_from_the_first_attach() {
    Fake f; ct::Runner r(f);
    const pd::SourceCaps full = f.caps;
    f.caps.num_pdos = 2;   // what the charger announces from now on: 5 V and 9 V only
    const auto o = with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Qc2), {ct::Unit::Qc2, ct::Unit::Pd});
    CHECK_EQ(r.start(full, o, f.in, true), ct::Reason::None);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    unsigned fixed = 0;
    for (const auto& s : r.steps()) fixed += s.algorithm == A::Pd3Fixed && s.verdict != ct::Verdict::Skip;
    CHECK_EQ(fixed, 2u);
}
// A source that is gone for good ends the run as lost: the units behind it never start and no row is left pending.
static void test_source_gone_in_a_unit_skips_the_remaining_units() {
    Fake f; ct::Runner r(f);
    f.drop_at = 1; f.gone = true;   // the first handshake kills the source for good
    const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Afc), {ct::Unit::Qc2, ct::Unit::Qc3, ct::Unit::Afc});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Stopped);
    CHECK_EQ(f.attaches_legacy, 1);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Pass), 0u);
    CHECK_EQ(count_reason(r, ct::Group::Qc3, ct::Reason::Lost) > 0, true);
    CHECK_EQ(count_reason(r, ct::Group::Afc, ct::Reason::Lost) > 0, true);
    for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pending || s.verdict == ct::Verdict::Running, false);
}
// Cancelling in the middle of a Legacy unit hands the sink back to the PD policy (restore + rescan) and records the row.
static void test_cancel_during_a_unit_restores_the_pd_policy() {
    Fake f; ct::Runner r(f);
    const auto o = with_units(ct::bit(A::Qc2), {ct::Unit::Qc2});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return f.fc_requests == 1; }), true);
    r.cancel(f.in);
    f.tick(r, 20000);
    CHECK_EQ(r.state(), ct::State::Stopped);
    CHECK_EQ(f.restores, 1); CHECK_EQ(f.rescans, 1);
    CHECK_EQ(f.in.vbus_mv, 5000);
}

// ---- reboot resume ---------------------------------------------------------------------------
static void test_resume_reboot_in_request() {
    k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
    Fake f; ct::Runner r(f);
    CHECK_EQ(journaled_start(f, r, j, kPd), ct::Reason::None);
    for (int n = 0; n < 2000 && f.phases.empty(); ++n) f.tick(r, 100);
    CHECK_EQ(f.phases.size(), 1u);
    CHECK_EQ(int(f.phases[0].first), int(ct::Phase::Request));
    const size_t cut = f.phases[0].second;
    CHECK_EQ(r.steps()[cut].mv > 5000, true);
    // Reboot: the journal is all that is left.
    Boot b(fl);
    CHECK_EQ(b.rep.unfinished, true); CHECK_EQ(int(b.rep.phase), int(cj::Phase::Request));
    CHECK_EQ(int(b.r.state()), int(ct::State::Stopped));
    // The first power loss in a request is not a verdict on the charger: the step stays Pending and is asked again.
    CHECK_EQ(int(find_step(b.r, cut)->verdict), int(ct::Verdict::Pending));
    CHECK_EQ(b.r.resumable(), true); CHECK_EQ(b.r.run_id(), r.run_id()); CHECK_EQ(b.r.resumed(), 0);
    // Never an above-5 V request after a reboot until the explicit resume.
    b.f.tick(b.r, 20000);
    CHECK_EQ(b.f.pd_requests + b.f.fc_requests, 0); CHECK_EQ(int(b.r.state()), int(ct::State::Stopped));
    // The loss counter is journaled and the marker is void: a second reboot before the resume does not count another loss.
    Boot again(fl);
    CHECK_EQ(int(find_step(again.r, cut)->verdict), int(ct::Verdict::Pending));
    CHECK_EQ(again.rep.phase == cj::Phase::None, true);
    CHECK_EQ(again.rep.attempt_index, size_t(ct::unit_of(r.steps()[cut]))); CHECK_EQ(int(again.rep.attempts), 1);   // the count belongs to the unit
    // Resume: Baseline, then the pending steps; the earlier results are kept.
    CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::None));
    CHECK_EQ(b.r.resumed(), 1);
    b.f.tick(b.r, 200000);
    CHECK_EQ(int(b.r.state()), int(ct::State::Done));
    CHECK_EQ(int(find_step(b.r, cut)->reason) != int(ct::Reason::RebootLimit), true);
    CHECK_EQ(int(find_step(b.r, cut)->reason) != int(ct::Reason::RebootRequest), true);
    CHECK_EQ(b.f.pd_requests > 0, true); CHECK_EQ(b.f.in.vbus_mv, 5000);
    for (const auto& s : b.r.steps()) CHECK_EQ(int(s.verdict) != int(ct::Verdict::Pending) && int(s.verdict) != int(ct::Verdict::Running), true);
    const auto snap = remote::charger_test_snapshot(b.r);
    CHECK_EQ(snap.resumed, 1);
}
// The planned re-attach (rdintent::plan_reattach, the 6 A hunt on a self-powered K2) reboots the K2 on purpose: it writes the
// Reattach record (/rd.rec) before it drops the Rd, and the comeback boot's journal resumes the run. That resume is the
// planned re-attach, not a power loss: the "resumed" counter must stay 0 (ROADMAP 2026-11-09, "Planned re-attach counts as a
// resume"). An unplanned VBUS loss in the same run still counts 1.
static void test_resume_after_planned_reattach_is_not_counted() {
    k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
    Fake f; ct::Runner r(f);
    CHECK_EQ(journaled_start(f, r, j, kPd), ct::Reason::None);
    for (int n = 0; n < 2000 && f.phases.empty(); ++n) f.tick(r, 100);
    CHECK_EQ(f.phases.empty(), false);
    CHECK_EQ(int(f.phases[0].first), int(ct::Phase::Request));
    const size_t cut = f.phases[0].second;
    CHECK_EQ(r.steps()[cut].mv > 5000, true);
    // The planned re-attach wrote its intent marker before the reboot (rdcomeback::begin_reattach on the device):
    CHECK_EQ(rdintent::save(js.fs, rdintent::Intent{rdintent::IntentKind::Reattach, 2, {}}), true);
    Boot b(fl);   // the comeback boot: the marker says this reboot was planned
    CHECK_EQ(b.planned_, true);
    CHECK_EQ(b.rep.unfinished, true); CHECK_EQ(b.r.resumable(), true); CHECK_EQ(b.r.resumed(), 0);
    CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::None));
    CHECK_EQ(b.r.resumed(), 0);   // the planned re-attach is not counted as a resume
    b.j.resumed(b.r.resumed());   // what the UI journals right after a resume
    const auto snap = remote::charger_test_snapshot(b.r);
    CHECK_EQ(snap.resumed, 0);    // the wire carries no resume
    // Power goes again before the run ends, this time unplanned: by now the follower has rewritten the marker as Session
    // (the contract was Ready), so the next boot is not planned and the loss counts.
    CHECK_EQ(rdintent::save(js.fs, rdintent::Intent{rdintent::IntentKind::Session, 2, {}}), true);
    Boot c(fl);
    CHECK_EQ(c.planned_, false); CHECK_EQ(c.rep.resumed, 0);   // the journal replays the planned re-attach as no resume
    CHECK_EQ(c.r.resumable(), true);
    CHECK_EQ(int(c.r.resume(c.f.in, true)), int(ct::Reason::None));
    CHECK_EQ(c.r.resumed(), 1);   // the unplanned VBUS loss counts
    c.j.resumed(c.r.resumed());
    Boot d(fl);
    CHECK_EQ(d.rep.resumed, 1);
    const auto snap2 = remote::charger_test_snapshot(d.r);
    CHECK_EQ(snap2.resumed, 1);
}
// The reboot budget: a unit (protocol) may take the K2 down kRebootBudget (3) times. Losses 1 and 2 resume the same row, loss 3
// skips what is left of the unit ("rebooted 3x") and the next unit runs: never an endless boot loop on one protocol, and the run
// itself never stops for it. The count lives in the journal (at.rec, keyed by the unit), so it survives every power loss.
static void reboot_budget_case(const ct::Options& o, ct::Phase want, ct::Unit bad, ct::Unit next) {
    k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
    Fake f; ct::Runner r(f);
    journaled_start(f, r, j, o);
    for (int n = 0; n < 4000 && f.phases.empty(); ++n) f.tick(r, 100);
    CHECK_EQ(f.phases.empty(), false);
    if (f.phases.empty()) return;
    CHECK_EQ(int(f.phases[0].first), int(want));
    const size_t cut = f.phases[0].second;
    CHECK_EQ(int(ct::unit_of(r.steps()[cut])), int(bad));
    auto asked = [&](const Fake& g) { for (const auto& p : g.phases) if (p.first == want) return true; return false; };   // (restore() journals the void marker first)
    for (unsigned loss = 1; loss < ct::kRebootBudget; ++loss) {   // losses 1 and 2: resume, the same row is asked again
        Boot b(fl);
        CHECK_EQ(b.rep.resumed, loss - 1);
        CHECK_EQ(int(find_step(b.r, cut)->verdict), int(ct::Verdict::Pending));
        CHECK_EQ(b.r.resumable(), true);
        CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::None));
        b.j.resumed(b.r.resumed());                     // what the UI journals right after a resume
        for (int n = 0; n < 8000 && !asked(b.f); ++n) b.f.tick(b.r, 100);
        CHECK_EQ(asked(b.f), true);
        CHECK_EQ(b.f.phases.back().second, cut);
    }
    Boot c(fl);                                         // power goes a third time
    CHECK_EQ(c.rep.unfinished, true); CHECK_EQ(unsigned(c.rep.attempts), ct::kRebootBudget - 1); CHECK_EQ(c.rep.resumed, ct::kRebootBudget - 1);
    CHECK_STR(ct::name(ct::Reason::RebootLimit), "rebooted 3x");
    CHECK_EQ(int(find_step(c.r, cut)->verdict), int(ct::Verdict::Skip));
    CHECK_EQ(int(find_step(c.r, cut)->reason), int(ct::Reason::RebootLimit));
    for (const auto& s : c.r.steps()) {
        if (ct::unit_of(s) == bad) CHECK_EQ(int(s.verdict) != int(ct::Verdict::Pending), true);      // nothing of the unit is left to run
        if (ct::unit_of(s) == next) CHECK_EQ(int(s.verdict), int(ct::Verdict::Pending));              // the next unit is untouched
    }
    CHECK_EQ(c.r.resumable(), true);
    CHECK_EQ(int(c.r.resume(c.f.in, true)), int(ct::Reason::None));
    c.f.tick(c.r, 400000);
    CHECK_EQ(int(c.r.state()), int(ct::State::Done));
    CHECK_EQ(c.r.resumed(), ct::kRebootBudget);
    CHECK_EQ(int(find_step(c.r, cut)->reason), int(ct::Reason::RebootLimit));
    CHECK_EQ(count_unit_verdict(c.r, next, ct::Verdict::Pass) > 0, true);
    for (const auto& s : c.r.steps()) CHECK_EQ(int(s.verdict) != int(ct::Verdict::Pending) && int(s.verdict) != int(ct::Verdict::Running), true);
}
static void test_reboot_budget_skips_the_unit_on_the_third_loss() {
    reboot_budget_case(with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Qc2), {ct::Unit::Pd, ct::Unit::Qc2}), ct::Phase::Request, ct::Unit::Pd, ct::Unit::Qc2);
    reboot_budget_case(with_units(ct::bit(A::Qc3) | ct::bit(A::Qc2), {ct::Unit::Qc3, ct::Unit::Qc2}), ct::Phase::Handover, ct::Unit::Qc3, ct::Unit::Qc2);
}
// The A-port run of 2026-11-06: UFCS took the K2's power away. The unit is the same story as any other: three losses skip it, the
// next unit runs, the run does not stop.
static void test_reboot_budget_covers_the_ufcs_unit() {
    reboot_budget_case(with_units(ct::bit(A::Ufcs) | ct::bit(A::Qc2), {ct::Unit::Ufcs, ct::Unit::Qc2}), ct::Phase::Handover, ct::Unit::Ufcs, ct::Unit::Qc2);
}
// The "Legacy only" preset (every algorithm except the PD fixed and PPS ones) plans no PD row at all, with or without a PD list.
static void test_legacy_preset_plans_no_pd_rows() {
    const uint32_t legacy = ct::kDefaultAlgorithms & ~(ct::kPdFixedAlgorithms | ct::kPpsAlgorithms);
    for (const bool caps_valid : {true, false}) {
        Fake f; f.caps.valid = caps_valid; ct::Runner r(f);
        const ct::Options o{.selected = legacy};
        CHECK_EQ(int(r.start(f.caps, o, f.in, true)), int(ct::Reason::None));
        unsigned pd_rows = 0;
        for (const auto& s : r.steps()) pd_rows += s.group == ct::Group::Pd || ct::unit_of(s) == ct::Unit::Pd || ct::unit_of(s) == ct::Unit::PdLow;
        CHECK_EQ(pd_rows, 0u);
        f.tick(r, 400000);
        CHECK_EQ(int(r.state()), int(ct::State::Done));
        CHECK_EQ(f.pd_requests, 0);
        for (const auto& s : r.steps()) CHECK_EQ(int(s.verdict) != int(ct::Verdict::Pending) && int(s.verdict) != int(ct::Verdict::Running), true);
    }
}
// A source that never lets go of VBUS (a USB-A port, or a run on the measured VBUS): the default plan finishes, a self-powered K2
// never detaches, and a non-self-powered one does not wait out the long dark window at each unit.
static void test_always_on_source_never_stalls() {
    {
        Fake f; f.in.self_powered = true; ct::Runner r(f);
        CHECK_EQ(int(r.start(f.caps, ct::Options{}, f.in, true)), int(ct::Reason::None));
        f.tick(r, 900000);
        CHECK_EQ(int(r.state()), int(ct::State::Done));
        CHECK_EQ(f.detaches, 0); CHECK_EQ(f.min_vbus >= 4000, true); CHECK_EQ(f.restores, 0);
    }
    {
        Fake f; f.never_dark = true; ct::Runner r(f);
        CHECK_EQ(int(r.start(f.caps, with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Pd3Fixed), {ct::Unit::Qc2, ct::Unit::Qc3, ct::Unit::Pd}), f.in, true)), int(ct::Reason::None));
        f.tick(r, 900000);
        CHECK_EQ(int(r.state()), int(ct::State::Done));
    }
}
// Losses before any phase marker (a step that dies on a 5 V level) are bounded too: past kMaxResumes the run is not offered again.
static void test_resume_count_is_bounded() {
    k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
    Fake f; ct::Runner r(f);
    journaled_start(f, r, j, kPd);
    for (unsigned n = 0; n < ct::kMaxResumes + 2; ++n) {
        Boot b(fl);
        if (!b.r.resumable()) { CHECK_EQ(n >= ct::kMaxResumes, true); break; }
        CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::None));
        // The resumed run dies at once, before any step: the journal only keeps the counter.
        b.j.resumed(b.r.resumed());
        CHECK_EQ(unsigned(b.r.resumed()), n + 1);
    }
    Boot end(fl);
    CHECK_EQ(end.r.resumable(), false);
}
// Self-powered run, hardware 2026-11-06 (brick on the top plug, K2 powered by its VBUS): the resumed boot arms the spoof and
// the cable after resume(), which bumps the attach session. The Baseline took that for a lost source, skipped every row
// "source lost" and the UI closed the journal ("stopped (source lost)", rev 4). A session change at the resume is adopted.
static void test_resumed_baseline_adopts_a_session_change() {
    k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
    Fake f; ct::Runner r(f);
    journaled_start(f, r, j, kPd);
    for (int n = 0; n < 2000 && f.phases.empty(); ++n) f.tick(r, 100);
    Boot b(fl);
    CHECK_EQ(b.r.resumable(), true);
    CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::None));
    ++b.f.in.session;                                   // the page arms the spoof / reads the cable: a new attach session
    b.f.tick(b.r, 300000);
    CHECK_EQ(int(b.r.state()), int(ct::State::Done));
    CHECK_EQ(count_reason(b.r, ct::Group::Pd, ct::Reason::Lost), 0u);
    for (const auto& s : b.r.steps()) CHECK_EQ(int(s.verdict) != int(ct::Verdict::Pending) && int(s.verdict) != int(ct::Verdict::Skip), true);
}
// A self-powered K2 that loses VBUS is dying: the run is not finalized. Nothing is skipped, the rows stay Pending, the state is
// Stopped + suspended (resumable), and the journal still holds the phase marker so the next boot resumes it.
static void test_self_powered_vbus_loss_suspends_the_run() {
    k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
    Fake f; ct::Runner r(f);
    journaled_start(f, r, j, kPd);
    for (int n = 0; n < 2000 && f.phases.empty(); ++n) f.tick(r, 100);
    Boot b(fl);                                          // the first reboot (power loss), then the self-powered resume
    b.f.in.self_powered = true;
    CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::None));
    b.j.resumed(b.r.resumed());
    auto asked = [&] { for (const auto& p : b.f.phases) if (p.first == ct::Phase::Request) return true; return false; };
    for (int n = 0; n < 4000 && !asked(); ++n) b.f.tick(b.r, 100);
    CHECK_EQ(asked(), true);
    b.f.in.vbus_mv = 0;
    b.f.tick(b.r, 3000);
    CHECK_EQ(int(b.r.state()), int(ct::State::Stopped));
    CHECK_EQ(b.r.suspended(), true);
    CHECK_EQ(b.r.resumable(), true);
    CHECK_EQ(count_reason(b.r, ct::Group::Pd, ct::Reason::Lost), 0u);
    bool pending = false;
    for (const auto& s : b.r.steps()) { CHECK_EQ(int(s.verdict) != int(ct::Verdict::Skip), true); pending |= s.verdict == ct::Verdict::Pending; }
    CHECK_EQ(pending, true);
    // Power comes back without a reboot: the same Runner resumes from memory and finishes the run.
    b.f.in.vbus_mv = 5000; ++b.f.in.session;
    CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::None));
    CHECK_EQ(b.r.suspended(), false);
    b.f.tick(b.r, 300000);
    CHECK_EQ(int(b.r.state()), int(ct::State::Done));
    CHECK_EQ(count_reason(b.r, ct::Group::Pd, ct::Reason::Lost), 0u);
}
// A HID-powered K2 is unchanged: the same loss still ends the run as "source lost".
static void test_hid_powered_vbus_loss_still_stops_the_run() {
    Fake f; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kPd, f.in, true), ct::Reason::None);
    for (int n = 0; n < 2000 && !f.pd_requests; ++n) f.tick(r, 100);
    f.in.vbus_mv = 0;
    f.tick(r, 60000);
    CHECK_EQ(r.suspended(), false);
    CHECK_EQ(int(r.state()), int(ct::State::Stopped));
    CHECK_EQ(count_reason(r, ct::Group::Pd, ct::Reason::Lost) > 0, true);
}
// Holding BACK at boot cancels the resume: the run is closed, the next boot starts clean.
static void test_resume_cancel_key() {
    CHECK_EQ(ct::resume_wanted(false, true), true);                // every run resumes by itself: nothing held, gate ready
    CHECK_EQ(ct::resume_wanted(true, true), false);                // BACK held: cancel
    CHECK_EQ(ct::resume_wanted(false, false), false);              // no measurement ready yet: not now
}
// Power-loss injection over a whole PD + legacy plan: the power goes just before each Env call in turn (every request, release,
// detach, attach, phase marker and step record), the K2 reboots into the journal, resumes, and the finished run equals the
// uninterrupted one step by step, plus the resumed counter.
static void expect_same_steps(const ct::Runner& ref, const ct::Runner& got, size_t cut) {
    CHECK_EQ(got.steps().size(), ref.steps().size());
    if (got.steps().size() != ref.steps().size()) { std::printf("  cut %zu: %zu steps, expected %zu\n", cut, got.steps().size(), ref.steps().size()); return; }
    unsigned bad = 0;
    for (size_t i = 0; i < ref.steps().size(); ++i) {
        const auto& a = ref.steps()[i]; const auto& b = got.steps()[i];
        const bool same = a.algorithm == b.algorithm && a.group == b.group && a.pos == b.pos && a.mv == b.mv && a.ma == b.ma &&
                          a.verdict == b.verdict && a.reason == b.reason && a.measured_mv == b.measured_mv && a.continuous == b.continuous;
        if (!same && bad++ < 3)
            std::printf("  cut %zu step %zu: %s/%u %s(%s) -> %s(%s) [pos %u/%u ma %u/%u meas %u/%u]\n", cut, i, ct::name(a.algorithm), a.mv, ct::name(a.verdict),
                        ct::name(a.reason), ct::name(b.verdict), ct::name(b.reason), unsigned(a.pos), unsigned(b.pos), unsigned(a.ma),
                        unsigned(b.ma), unsigned(a.measured_mv), unsigned(b.measured_mv));
    }
    CHECK_EQ(bad, 0u);
}
static void test_power_loss_at_every_event_matches_an_uninterrupted_run() {
    const ct::Options o{};
    Fake rf; ct::Runner ref(rf);
    CHECK_EQ(int(ref.start(rf.caps, o, rf.in, true)), int(ct::Reason::None));
    rf.tick(ref, 400000);
    CHECK_EQ(int(ref.state()), int(ct::State::Done));
    const size_t total = rf.events;
    CHECK_EQ(total > 100, true);
    size_t resumed_runs = 0;
    for (size_t k = 0; k < total; ++k) {
        k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
        Fake f; ct::Runner r(f);
        f.jr = &j; f.rn = &r; f.cut_event = k;
        if (r.start(f.caps, o, f.in, true) != ct::Reason::None) { CHECK_EQ(0, 1); continue; }
        if (f.dead) continue;                          // power lost before the run was ever journaled: nothing to resume
        j.begin(r.run_id(), o, r.evidence(), r.steps());
        for (int n = 0; n < 4000 && !f.dead && r.state() != ct::State::Done; ++n) f.tick(r, 100);
        CHECK_EQ(f.dead, true);
        f.k2_rebooted();
        ct::Runner r2(f); f.rn = &r2;
        cj::Replay rep; j.replay(rep);
        // Power lost after the last row was journaled (the closing detach / re-attach calls): the card is complete, nothing to resume.
        if (rep.found && !rep.unfinished) {
            for (size_t n = 0; n < rep.count; ++n) CHECK_EQ(int(rep.steps[n].verdict) != int(ct::Verdict::Pending), true);
            continue;
        }
        CHECK_EQ(rep.found && rep.unfinished, true);
        if (!rep.found) continue;
        r2.restore(rep.options, rep.evidence, rep.run_id, {rep.steps.data(), rep.count}, rep.phase, rep.phase_index, rep.resumed,
                   rep.attempt_index, rep.attempts);
        CHECK_EQ(r2.resumable(), true);
        CHECK_EQ(int(r2.resume(f.in, true)), int(ct::Reason::None));
        for (int n = 0; n < 4000 && r2.state() != ct::State::Done && r2.state() != ct::State::Stopped; ++n) f.tick(r2, 100);
        CHECK_EQ(int(r2.state()), int(ct::State::Done));
        CHECK_EQ(r2.resumed(), 1);
        expect_same_steps(ref, r2, k);
        f.tick(r2, 3000);                              // the charger's re-attach after the final detach takes its back_ms
        CHECK_EQ(f.in.vbus_mv, 5000);
        ++resumed_runs;
    }
    CHECK_EQ(resumed_runs > total / 2, true);
}
// A run that ends for good tells its environment BEFORE it skips the rest: the journal is closed first, so a power loss anywhere
// in the skip rows cannot leave a stopped run that the next boot resumes. Power lost at every event of a cancel.
static void test_a_final_stop_closes_before_it_skips() {
    const auto setup = [](Fake& f, ct::Runner& r, cj::Journal& j) {
        journaled_start(f, r, j, kPd);
        for (int n = 0; n < 2000 && f.pd_requests < 1; ++n) f.tick(r, 100);
        f.tick(r, 600);
    };
    size_t total = 0;
    {
        k2test::RamDisk fl(96); JStore js(fl);
        Fake f; ct::Runner r(f); setup(f, r, js.j);
        CHECK_EQ(f.pd_requests >= 1, true);
        CHECK_EQ(f.closings.size(), 0u);               // nothing closed while it runs
        const size_t before = f.events;
        r.cancel(f.in);
        for (int n = 0; n < 400 && r.state() != ct::State::Stopped; ++n) f.tick(r, 100);
        CHECK_EQ(int(r.state()), int(ct::State::Stopped));
        total = f.events - before;
        CHECK_EQ(f.closings.size(), 1u);
        for (size_t n = 0; n < f.closings[0] && n < f.records.size(); ++n) CHECK_EQ(int(f.records[n].verdict) != int(ct::Verdict::Skip), true);
        CHECK_EQ(f.records.size() > f.closings[0], true);   // skip rows follow the close
        cj::Replay rep; js.j.replay(rep); CHECK_EQ(rep.found && !rep.unfinished, true);
    }
    CHECK_EQ(total > 3, true);
    size_t cut_before = 0, cut_after = 0;
    for (size_t k = 0; k <= total; ++k) {
        k2test::RamDisk fl(96); JStore js(fl);
        Fake f; ct::Runner r(f); setup(f, r, js.j);
        f.cut_event = f.events + k;
        r.cancel(f.in);
        for (int n = 0; n < 400 && !f.dead && r.state() != ct::State::Stopped; ++n) f.tick(r, 100);
        f.k2_rebooted();
        cj::Replay rep; js.j.replay(rep);
        CHECK_EQ(rep.found, true);
        // Once the close was called the run is closed, wherever in the skip rows the power went.
        if (!f.closings.empty()) { CHECK_EQ(rep.unfinished, false); ++cut_after; }
        else ++cut_before;
    }
    CHECK_EQ(cut_before > 0 && cut_after > 0, true);
}
static void test_resume_refusals_and_abandon() {
    k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
    Fake f; ct::Runner r(f);
    journaled_start(f, r, j, kPd);
    for (int n = 0; n < 2000 && f.phases.empty(); ++n) f.tick(r, 100);
    Boot b(fl);
    b.f.in.inline_load = true;
    CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::Inline));
    CHECK_EQ(int(b.r.state()), int(ct::State::Stopped)); CHECK_EQ(b.r.resumed(), 0);
    b.f.in.inline_load = false; b.f.in.vbus_mv = -1;
    CHECK_EQ(int(b.r.resume(b.f.in, true)), int(ct::Reason::NoMonitor));
    CHECK_EQ(int(b.r.resume(b.f.in, false)) != int(ct::Reason::None), true);
    CHECK_EQ(b.f.pd_requests + b.f.fc_requests + b.f.releases, 0);
    // Abandon: results stay, Finish keeps the next boot from asking again.
    b.j.finish(uint8_t(ct::State::Stopped), uint8_t(ct::Reason::Cancelled));
    Boot next(fl);
    CHECK_EQ(next.rep.unfinished, false); CHECK_EQ(next.rep.found, true);
    CHECK_EQ(int(next.r.state()), int(ct::State::Idle));
    // A finished run is not resumable.
    CHECK_EQ(b.r.resumable(), true);
}

// EPR entry that a charger cuts with a Hard Reset (Baseus 140W, 2026-11-05, pattern epr-entry-hard-reset): the entry is retried
// after the source's list and 5 V contract have settled, at most kEprEntryRetries times; the retries are on the card.
static Fake epr_run_fake() {
    Fake f;
    f.in.cable_mv = 50000; f.in.cable_ma = 5000; f.in.cable_observed = true;
    f.caps.pdos[0] = pd::decode_pdo(f.caps.pdos[0].raw | (1u << 23));   // EPR Mode Capable, SPR list only
    f.in.pd.state = pd::SinkState::Ready; f.in.pd.mv = 5000;
    f.epr_caps = f.caps; f.epr_caps.epr = true; f.epr_caps.num_pdos = 10;
    f.epr_caps.pdos[7] = pd::decode_pdo((560u << 10) | 300);   // 28 V
    f.epr_caps.pdos[8] = pd::decode_pdo((720u << 10) | 300);   // 36 V
    f.epr_caps.pdos[9] = pd::decode_pdo(0xD0000000 | (480u << 17) | (150u << 8) | 140);
    return f;
}
static const ct::Step* epr_row(const ct::Runner& r, unsigned mv) {
    for (const auto& s : r.steps()) if (s.algorithm == A::Epr && s.mv == mv) return &s;
    return nullptr;
}
static void test_epr_row_cut_by_a_source_hard_reset_passes_on_retry() {
    for (int cuts = 0; cuts <= 3; ++cuts) {
        Fake f = epr_run_fake(); f.entry_resets_row = cuts;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Epr)}, f.in, true);
        f.tick(r, 240000);
        CHECK_EQ(r.state(), ct::State::Done);
        const auto* s28 = epr_row(r, 28000); const auto* s36 = epr_row(r, 36000);
        CHECK_EQ(s28 != nullptr && s36 != nullptr, true);
        if (!s28 || !s36) continue;
        // The first row asked from SPR eats the cuts (the baseline entry came first, so the 28 V row is already in EPR: the 36 V row
        // is the one entered from SPR).
        for (const auto* s : {s28, s36}) CHECK_EQ(s->reason == ct::Reason::CapsChanged, false);
        // Each retry request comes only after the cut's partial window, the source's dark time, and the settle with its backoff.
        for (size_t k = 2; k < f.entry_times.size(); ++k)
            CHECK_EQ(f.entry_times[k] - f.entry_times[k - 1] >=
                     f.partial_ms + f.reset_dark_ms + ct::kEprSettleMs + ct::kEprSettleStepMs * uint32_t(k - 2), true);
        CHECK_EQ(f.entry_times.size(), size_t(2 + std::min(cuts, int(ct::kEprEntryRetries))));
        if (cuts <= int(ct::kEprEntryRetries)) {
            CHECK_EQ(s28->verdict == ct::Verdict::Pass && s36->verdict == ct::Verdict::Pass, true);
            CHECK_EQ(std::max(s28->tries, s36->tries), unsigned(cuts ? 1 + cuts : 0));
        } else {
            CHECK_EQ(s28->verdict == ct::Verdict::Pass, true);
            CHECK_EQ(s36->verdict == ct::Verdict::Fail && s36->reason == ct::Reason::HardReset, true);
            CHECK_EQ(s36->tries, unsigned(1 + ct::kEprEntryRetries));
        }
    }
}

// The baseline entry (the first EPR request of the run, position 0) is cut the same way: asked again after the settle, and the first
// EPR row carries the retries. Past the cap the EPR rows are skipped as refused and the run still finishes.
static void test_baseline_epr_entry_cut_by_a_source_hard_reset_is_retried() {
    for (int cuts = 0; cuts <= 3; ++cuts) {
        Fake f = epr_run_fake(); f.entry_resets_base = cuts;
        ct::Runner r(f);
        r.start(f.caps, {.selected = ct::bit(A::Epr)}, f.in, true);
        f.tick(r, 240000);
        CHECK_EQ(r.state(), ct::State::Done);
        for (size_t k = 1; k < f.entry_times.size() && k <= size_t(cuts); ++k)
            CHECK_EQ(f.entry_times[k] - f.entry_times[k - 1] >=
                     f.partial_ms + f.reset_dark_ms + ct::kEprSettleMs + ct::kEprSettleStepMs * uint32_t(k - 1), true);
        const auto* s28 = epr_row(r, 28000);
        if (cuts <= int(ct::kEprEntryRetries)) {
            CHECK_EQ(s28 != nullptr && s28->verdict == ct::Verdict::Pass, true);
            if (s28) CHECK_EQ(unsigned(s28->tries), unsigned(cuts ? 1 + cuts : 0));
        } else {
            for (const auto& s : r.steps())
                if (s.algorithm == A::Epr) CHECK_EQ(s.verdict == ct::Verdict::Pass, false);
        }
        for (const auto& s : r.steps()) CHECK_EQ(s.reason == ct::Reason::CapsChanged, false);
    }
}
// Resume after a reboot (review item A): the resumed run's rows equal the uninterrupted run's. The power goes before each Env call
// in turn, the K2 reboots into the journal and resumes. `make` builds the charger; every run starts from a fresh one.
template <class Make>
static unsigned power_loss_sweep(Make make, const ct::Options& o) {
    Fake rf = make(); ct::Runner ref(rf);
    CHECK_EQ(int(ref.start(rf.caps, o, rf.in, true)), int(ct::Reason::None));
    rf.tick(ref, 400000);
    CHECK_EQ(int(ref.state()), int(ct::State::Done));
    const size_t total = rf.events;
    unsigned resumed_runs = 0;
    for (size_t k = 0; k < total; ++k) {
        k2test::RamDisk fl(96); JStore js(fl); cj::Journal& j = js.j;
        Fake f = make(); ct::Runner r(f);
        f.jr = &j; f.rn = &r; f.cut_event = k;
        if (r.start(f.caps, o, f.in, true) != ct::Reason::None) { CHECK_EQ(0, 1); continue; }
        if (f.dead) continue;
        j.begin(r.run_id(), o, r.evidence(), r.steps());
        j.meta({.units = o.units});                    // the UI journals the unit order too: a resumed run keeps it
        f.jopts = o;
        for (int n = 0; n < 4000 && !f.dead && r.state() != ct::State::Done; ++n) f.tick(r, 100);
        CHECK_EQ(f.dead, true);
        f.k2_rebooted();
        ct::Runner r2(f); f.rn = &r2;
        cj::Replay rep; j.replay(rep);
        if (rep.found && !rep.unfinished) continue;
        CHECK_EQ(rep.found && rep.unfinished, true);
        if (!rep.found) continue;
        j.adopt(rep); f.jopts = rep.options;
        r2.restore(rep.options, rep.evidence, rep.run_id, {rep.steps.data(), rep.count}, rep.phase, rep.phase_index, rep.resumed,
                   rep.attempt_index, rep.attempts);
        if (!r2.resumable()) continue;
        CHECK_EQ(int(r2.resume(f.in, true)), int(ct::Reason::None));
        for (int n = 0; n < 4000 && r2.state() != ct::State::Done && r2.state() != ct::State::Stopped; ++n) f.tick(r2, 100);
        CHECK_EQ(int(r2.state()), int(ct::State::Done));
        expect_same_steps(ref, r2, k);
        ++resumed_runs;
    }
    return resumed_runs;
}
// Item A1: the EPR proof (the source accepted EPR entry over a cable nobody read) is not in the journal as such. A resumed run
// whose EPR rows were still pending asked the cable gate again with no proof and skipped them "cable not 50V 5A".
static void test_resume_keeps_the_epr_proof_of_an_unread_cable() {
    const auto make = [] { Fake f = epr_run_fake(); f.in.cable_observed = false; f.in.cable_mv = 20000; f.in.cable_ma = 3000; return f; };
    const unsigned n = power_loss_sweep(make, {.selected = ct::bit(A::Epr) | ct::bit(A::EprAvs)});
    CHECK_EQ(n > 10u, true);
}
// Item A2: a run started with no list heard plans one placeholder row per PD unit; the real rows come with the first list
// (replan_pd). A reboot in a unit ahead of the PD unit restored the placeholders as the final plan: the PD unit ran one 5 V row.
static void test_resume_replans_the_late_pd_rows() {
    const auto make = [] { Fake f; f.saved = f.caps; f.caps.valid = false; return f; };
    const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Qc2, ct::Unit::Pd});
    const unsigned n = power_loss_sweep(make, o);
    CHECK_EQ(n > 10u, true);
}
// Item A2 with the PD unit first and EPR wanted: the resume lands in the live 5 V contract; the rows and the EPR entry follow.
static void test_resume_replans_the_late_pd_rows_with_epr() {
    const auto make = [] {
        Fake f = epr_run_fake(); f.saved = f.caps; f.caps.valid = false; f.in.pd.state = pd::SinkState::Idle; return f;
    };
    const auto o = with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Epr), {ct::Unit::Pd});
    const unsigned n = power_loss_sweep(make, o);
    CHECK_EQ(n > 10u, true);
}
// A Reject (a refused 6 A hunt row) leaves the source settling: an EPR entry right after it waits kRejectSettleMs.
// The hunt rows only go out against a list that advertises 6 A (decision 2026-11-07), so the 20 V fixed PDO here (the row that sits
// right before the EPR rows) says 6 A (SPR and EPR list alike) and the source refuses the hunt row anyway; before that rule the
// default 3.25 A PDO was enough.
static void test_epr_entry_waits_after_a_rejected_row() {
    Fake f = epr_run_fake(); f.reject_hunts = true;
    f.caps.pdos[4] = f.epr_caps.pdos[4] = pd::decode_pdo((400u << 10) | 600);
    ct::Runner r(f);
    r.start(f.caps, {.selected = ct::bit(A::Pd3Fixed) | ct::bit(A::Epr)}, f.in, true);
    f.tick(r, 240000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.reject_times.empty(), false);
    const auto* s28 = epr_row(r, 28000);
    CHECK_EQ(s28 != nullptr && s28->verdict == ct::Verdict::Pass, true);
    // The entry (pos >= 8 asked from SPR) that follows the Reject comes at least kRejectSettleMs later.
    uint32_t next_entry = 0;
    if (!f.reject_times.empty())
        for (auto t : f.entry_times) if (t > f.reject_times.back()) { next_entry = t; break; }
    CHECK_EQ(next_entry != 0 && !f.reject_times.empty() && next_entry - f.reject_times.back() >= 1500u, true);   // literal: the plain release settle is only ~600 ms
    static_assert(ct::kRejectSettleMs >= 1500);
}

// ---- self-powered unit transitions: the pinned Rd keeps the supply, no unit boundary opens CC --------------------------
// The policy, per unit: a Pd unit re-announces its list with a Soft_Reset; a Legacy unit stays PD-silent behind the pinned Rd;
// the low programmable rows can latch the source's output off, so they are skipped (pattern latch-off-low-pps).
static void test_self_path_policy() {
    using P = ct::SelfPath;
    CHECK_EQ(int(ct::self_path(ct::Unit::Pd)), int(P::SoftReset));
    CHECK_EQ(int(ct::self_path(ct::Unit::PdLow)), int(P::NeedsExternalPower));
    for (const ct::Unit u : {ct::Unit::Qc2, ct::Unit::Qc3, ct::Unit::Qc3Plus, ct::Unit::Afc, ct::Unit::Fcp, ct::Unit::Scp, ct::Unit::Vooc,
                             ct::Unit::SuperVooc, ct::Unit::SuperVooc65, ct::Unit::Ufcs, ct::Unit::Vivo})
        CHECK_EQ(int(ct::self_path(u)), int(P::SilentHold));
    CHECK_EQ(ct::name(ct::Reason::NeedsExternalPower) != nullptr && ct::name(ct::Reason::NeedsExternalPower)[0] != 0, true);
}
// A self-powered Pd unit: Soft_Reset (hold), never a detach; VBUS never drops; the run ends without touching the sink again.
static void test_self_powered_pd_unit_soft_resets() {
    Fake f; f.in.self_powered = true; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    f.tick(r, 100000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(seq_of(f) == "h", true);
    CHECK_EQ(f.detaches, 0); CHECK_EQ(f.attaches_pd + f.attaches_legacy, 0); CHECK_EQ(f.rescans, 0); CHECK_EQ(f.restores, 0);
    CHECK_EQ(f.min_vbus >= 4000, true);
    CHECK_EQ(f.pd_requests > 3, true);
    for (const auto& s : r.steps()) CHECK_EQ(s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Skip, true);
}
// Every Legacy unit: one PD-silent hold, the pinned Rd untouched, then the PD policy back with a Soft_Reset (no rescan, no CC open).
static void test_self_powered_legacy_units_hold_silently() {
    struct Row { A a; ct::Unit u; };
    for (const Row row : {Row{A::Qc2, ct::Unit::Qc2}, Row{A::Qc3, ct::Unit::Qc3}, Row{A::Afc, ct::Unit::Afc}, Row{A::Fcp, ct::Unit::Fcp},
                          Row{A::Scp, ct::Unit::Scp}, Row{A::Vooc, ct::Unit::Vooc}, Row{A::SuperVooc, ct::Unit::SuperVooc},
                          Row{A::Ufcs, ct::Unit::Ufcs}, Row{A::Vivo, ct::Unit::Vivo}}) {
        Fake f; f.in.self_powered = true; ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, with_units(ct::bit(row.a), {row.u}), f.in, true), ct::Reason::None);
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(seq_of(f) == "lh", true);   // the unit's silent hold, then the PD policy back
        CHECK_EQ(f.detaches, 0); CHECK_EQ(f.rescans, 0); CHECK_EQ(f.restores, 0);
        CHECK_EQ(f.min_vbus >= 4000, true);
        CHECK_EQ(f.holds_legacy, 1); CHECK_EQ(f.holds_pd, 1);
    }
}
// Units in the middle of a run: Pd -> Legacy -> end is three holds; Legacy -> Legacy shares the silent hold; Legacy -> Pd re-announces.
static void test_self_powered_unit_boundaries() {
    {
        Fake f; f.in.self_powered = true; ct::Runner r(f);
        const auto o = with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Qc2), {ct::Unit::Pd, ct::Unit::Qc2});
        CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(seq_of(f) == "hlh", true); CHECK_EQ(f.detaches, 0); CHECK_EQ(f.min_vbus >= 4000, true);
    }
    {
        Fake f; f.in.self_powered = true; ct::Runner r(f);
        const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3), {ct::Unit::Qc2, ct::Unit::Qc3});
        CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(seq_of(f) == "lh", true); CHECK_EQ(f.detaches, 0); CHECK_EQ(f.min_vbus >= 4000, true);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Pass) > 0, true);
    }
    {
        Fake f; f.in.self_powered = true; ct::Runner r(f);
        const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Pd3Fixed), {ct::Unit::Qc2, ct::Unit::Pd});
        CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(seq_of(f) == "lh", true); CHECK_EQ(f.detaches, 0); CHECK_EQ(f.min_vbus >= 4000, true);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Pass) > 0, true);
    }
}
// The low programmable rows end the run's PD part: skipped with their own reason, never requested, VBUS never touched.
static void test_self_powered_skips_the_low_programmable_rows() {
    Fake f; low_pps_caps(f); f.in.self_powered = true; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kLow3v3, f.in, true), ct::Reason::None);
    f.tick(r, 400000);
    CHECK_EQ(r.state(), ct::State::Done);
    unsigned low = 0, skipped = 0;
    for (const auto& s : r.steps()) {
        if (ct::unit_of(s) != ct::Unit::PdLow) continue;
        ++low; skipped += s.verdict == ct::Verdict::Skip && s.reason == ct::Reason::NeedsExternalPower;
    }
    CHECK_EQ(low >= 1, true); CHECK_EQ(skipped, low);
    for (const auto& c : f.choices) CHECK_EQ(c.mv >= 5000, true);
    CHECK_EQ(f.detaches, 0); CHECK_EQ(f.rescans, 0); CHECK_EQ(f.min_vbus >= 4000, true);
    CHECK_EQ(f.seq.front(), 'l');
    // the same list on a HID-powered K2 runs the low rows (nothing changes there)
    Fake g; low_pps_caps(g); ct::Runner s(g);
    CHECK_EQ(s.start(g.caps, kLow3v3, g.in, true), ct::Reason::None);
    g.tick(s, 400000);
    bool low_asked = false;
    for (const auto& c : g.choices) low_asked |= c.mv < 5000;
    CHECK_EQ(low_asked, true);
    CHECK_EQ(g.holds, 0); CHECK_EQ(g.detaches > 0, true);
}
// A Soft_Reset the source never answers: the Pd unit fails as "no PD after re-attach", the run still ends, VBUS is never cut.
static void test_self_powered_soft_reset_without_caps_fails_the_unit() {
    Fake f; f.in.self_powered = true; f.silent_after_rd = true; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason(r, ct::Group::Pd, ct::Reason::NoReattach) + count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Fail, ct::Reason::NoReattach, false) > 0, true);
    CHECK_EQ(f.detaches, 0); CHECK_EQ(f.min_vbus >= 4000, true);
}
// A Legacy unit whose charger stays in PD is blocked once and not retried with a CC open: the run goes on, no detach.
static void test_self_powered_blocked_legacy_unit_never_detaches() {
    Fake f; f.in.self_powered = true; f.stays_pd = true; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Qc2) | ct::bit(A::Pd3Fixed), {ct::Unit::Qc2, ct::Unit::Pd}), f.in, true), ct::Reason::None);
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Skip, ct::Reason::BlockedByPd, false) > 0, true);
    CHECK_EQ(f.detaches, 0); CHECK_EQ(f.holds_legacy, 1); CHECK_EQ(f.min_vbus >= 4000, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Pass) > 0, true);
}
// Cancelling in the middle of a silent hold gives the PD policy back (a hold, not a rescan) and ends Stopped.
static void test_self_powered_cancel_in_a_legacy_hold_restores_pd() {
    Fake f; f.in.self_powered = true; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Qc2), {ct::Unit::Qc2}), f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return f.holds_legacy == 1; }), true);
    r.cancel(f.in);
    CHECK_EQ(r.state(), ct::State::Stopped);
    CHECK_EQ(f.holds_pd, 1); CHECK_EQ(f.rescans, 0); CHECK_EQ(f.detaches, 0);
}
// k2/selfpower-pd-contract: a self-powered K2 that took the source's 5 V contract at boot has the caps in hand at START. The
// PD rows are planned from them (no detach, no "not advertised"), and the evidence carries them.
static void test_self_powered_start_plans_from_the_held_caps() {
    Fake f; f.in.self_powered = true; f.in.pd_seen_attach = true; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    CHECK_EQ(r.evidence().num_pdos > 0, true);
    for (const auto& s : r.steps()) if (ct::unit_of(s) == ct::Unit::Pd) CHECK_EQ(s.verdict == ct::Verdict::Skip, false);
    f.tick(r, 100000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.detaches, 0); CHECK_EQ(f.min_vbus >= 4000, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(count_reason(r, ct::Group::Pd, ct::Reason::PdGaveUp), 0u);
}
// The source gave PD up before START (caps heard earlier in this attach, none now): the PD rows say so instead of "not advertised".
static void test_self_powered_start_without_caps_but_pd_seen_says_pd_gave_up() {
    Fake f; f.in.self_powered = true; f.in.pd_seen_attach = true; f.caps.valid = false; f.silent_after_rd = true; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Skip, ct::Reason::PdGaveUp, false) > 0, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Skip, ct::Reason::NoOffer, false), 0u);
    CHECK_EQ(f.detaches, 0); CHECK_EQ(f.min_vbus >= 4000, true);
    CHECK_EQ(ct::name(ct::Reason::PdGaveUp) != nullptr && ct::name(ct::Reason::PdGaveUp)[0] != 0, true);
}
// Not a self-powered K2, or no PD ever seen: unchanged, a missing list is "not advertised".
static void test_missing_caps_without_the_self_power_evidence_stay_not_advertised() {
    for (int mode = 0; mode < 2; ++mode) {
        Fake f; f.caps.valid = false; f.silent_after_rd = true;
        f.in.self_powered = mode == 1;            // 0: HID-powered with PD seen; 1: self-powered, PD never seen
        f.in.pd_seen_attach = mode == 0;
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
        f.tick(r, 200000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_reason(r, ct::Group::Pd, ct::Reason::PdGaveUp), 0u);
        CHECK_EQ(count_unit_verdict(r, ct::Unit::Pd, ct::Verdict::Skip, ct::Reason::NoOffer, false) > 0, true);
    }
}

// ---- the cable identity (Ra mode + spoof) follows the unit mode (Baseus 70W, 2026-11-07, pattern ra-disables-legacy) ----------
// The Ra mode makes a charger ask for the cable's identity, and the Baseus then ignores the D+/D- handshakes for the whole run:
// 27 pass / 16 fail with the cable off, 1 pass / 27 fail with the EPR identity. The identity belongs to the Pd-mode units only:
// every Legacy attach asks for it off, every Pd attach for it on, before the attach itself.
static void test_identity_follows_the_unit_mode() {
    Fake f; f.ra_on = true; f.ra_dead_legacy = true; ct::Runner r(f);   // the START armed the identity; the charger is deaf to D+/D- under it
    const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Pd3Fixed), {ct::Unit::Pd, ct::Unit::Qc2, ct::Unit::Qc3});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.ra_dead_fc, 0);                                                                     // not one handshake went out under the Ra mode
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Fail) + count_unit_verdict(r, ct::Unit::Qc3, ct::Verdict::Fail), 0u);
    CHECK_EQ(f.ra_log == "Rr", true);                                                              // Pd attach on, the one shared Legacy attach off
    CHECK_EQ(f.ra_at_attach.substr(0, 2) == "R-", true);                                           // the state was set BEFORE each attach
}
// A Legacy unit before the Pd one: off for it, on again for the Pd attach (the identity has to answer the cable discovery there).
static void test_identity_is_back_for_the_pd_unit_after_a_legacy_one() {
    Fake f; f.ra_on = true; f.ra_dead_legacy = true; ct::Runner r(f);
    const auto o = with_units(ct::bit(A::Qc2) | ct::bit(A::Pd3Fixed), {ct::Unit::Qc2, ct::Unit::Pd});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(seq_of(f) == "DLDPDP", true);   // Legacy unit, Pd unit, then the closing detach and Pd attach
    CHECK_EQ(f.ra_log == "rR", true);
    CHECK_EQ(f.ra_at_attach.substr(0, 2) == "-R", true);
    CHECK_EQ(count_unit_verdict(r, ct::Unit::Qc2, ct::Verdict::Pass) > 0, true);
    CHECK_EQ(f.ra_on, true);   // the run ends the way its last unit left it; the page's release_run drops the rest
}
// The full default order: every attach finds the identity matching its mode (Pd and PdLow with it, every Legacy attach without).
static void test_identity_matches_every_attach_of_the_default_order() {
    Fake f; f.ra_on = true; f.ra_dead_legacy = true; ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, ct::Options{.selected = ~0u}, f.in, true), ct::Reason::None);
    for (unsigned n = 0; n < 600000 && r.state() != ct::State::Done; n += 100) f.tick(r, 100);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(f.ra_dead_fc, 0);
    std::string modes;   // the attach calls only (a detach is not one)
    for (const char c : f.seq) if (c == 'P' || c == 'L') modes.push_back(c);
    CHECK_EQ(modes.size(), f.ra_at_attach.size());
    // The run's closing Pd attach (end_legacy) is not a unit attach: the identity is not asked for there, so it may find either.
    for (size_t n = 0; n + 1 < modes.size() && n < f.ra_at_attach.size(); ++n)
        CHECK_EQ(f.ra_at_attach[n] == 'R', modes[n] == 'P');
}
// A self-powered K2 never detaches (pinned Rd, no CC open), so the boot-time Ra stays for the whole run: the Runner asks for nothing.
static void test_self_powered_run_never_touches_the_identity() {
    Fake f; f.in.self_powered = true; f.ra_on = true; ct::Runner r(f);
    const auto o = with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Qc2), {ct::Unit::Pd, ct::Unit::Qc2});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(seq_of(f) == "hlh", true);
    CHECK_EQ(f.ra_log.empty(), true);
    CHECK_EQ(f.ra_on, true);
}

// ---- re-announced list after the cable discovery (pattern reannounce-after-cable-discovery) ------
// A Baseus-like 70 W source (EnerCore CJ11, hardware 2026-11-07, top plug, HID-powered): the first list is 5/9/12/15 V 3 A,
// 20 V 3.5 A and one PPS APDO 3.3-11 V 3.3 A. After the SOP' cable query it announces again with the PPS at 5 A (and, in the
// shifted variant, a wider APDO ahead of it).
static constexpr uint32_t kPps11v = 0xc0000000u | (110u << 17) | (33u << 8);   // PPS 3.3-11 V, current field ORed in (50 mA)
static pd::SourceCaps baseus70_caps() {
    pd::SourceCaps c; c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.num_pdos = 6; c.raw_header = 0x7181;
    const unsigned volts[] = {5000, 9000, 12000, 15000, 20000};
    for (unsigned n = 0; n < 5; ++n) c.pdos[n] = pd::decode_pdo(((volts[n] / 50) << 10) | (n == 4 ? 350 : 300));
    c.pdos[5] = pd::decode_pdo(kPps11v | 66);
    return c;
}
// The same list announced again: same fixed PDOs, the PPS APDO at 5 A. `shifted` puts a 3.3-21 V APDO in front of it (PDO 6).
static pd::SourceCaps baseus70_reannounced(bool shifted) {
    pd::SourceCaps c = baseus70_caps();
    if (!shifted) { c.pdos[5] = pd::decode_pdo(kPps11v | 100); return c; }
    c.num_pdos = 7;
    c.pdos[5] = pd::decode_pdo(0xc0000000u | (210u << 17) | (33u << 8) | 66);
    c.pdos[6] = pd::decode_pdo(kPps11v | 100);
    return c;
}
static bool a_pd_row_is_running(const ct::Runner& r) {
    for (const auto& s : r.steps()) if (s.group == ct::Group::Pd && s.verdict == ct::Verdict::Running) return true;
    return false;
}
// Before any PD row has started the plan follows the new list completely: the shifted slot, the wider APDO as rows of its own.
static void test_reannounce_before_the_first_row_replans() {
    Fake f; f.caps = baseus70_caps(); f.grown = baseus70_reannounced(true);
    f.grow_after_ms = 500; f.contract_delay_ms = 100;   // inside the baseline (500 ms settle after the contract), before the first row
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, with_units(kPd.selected, {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    f.tick(r, 170000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    CHECK_EQ(only_pass_or_skip(r), true);
    CHECK_EQ(r.evidence().num_pdos, 7);
    unsigned wide = 0, narrow = 0;
    for (const auto& s : r.steps()) {
        if (s.algorithm != A::Pps || s.hunt) continue;
        wide += s.pos == 6; narrow += s.pos == 7;
        if (s.pos == 7) CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 5000);   // the row carries the current of the list in hand
        CHECK_EQ(s.verdict, ct::Verdict::Pass);
    }
    CHECK_EQ(wide > 0, true); CHECK_EQ(narrow > 0, true);
}
// A row is already out when the list comes again (the SOP' query runs beside the first row): nothing is re-planned, the rows
// that are still to run follow the new slots and currents.
static void test_reannounce_during_the_first_row_follows_the_new_slots() {
    for (const bool shifted : {false, true}) {
        Fake f; f.caps = baseus70_caps();
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, with_units(kPd.selected, {ct::Unit::Pd}), f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r); }), true);
        f.caps = baseus70_reannounced(shifted); f.caps.valid = true;
        f.tick(r, 170000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
        CHECK_EQ(only_pass_or_skip(r), true);
        CHECK_EQ(r.evidence().num_pdos, shifted ? 7 : 6);
        const uint8_t pps_slot = shifted ? 7 : 6;
        for (const auto& s : r.steps()) {
            if (s.algorithm != A::Pps) continue;
            CHECK_EQ(s.pos, pps_slot); CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 5000);
        }
        // No request goes to the slot that now holds the wider APDO (the old PPS slot); the PPS requests all use the new one.
        unsigned at_pps = 0;
        for (const auto& c : f.choices) { if (shifted) CHECK_EQ(c.pos != 6, true); at_pps += c.pos == pps_slot; }
        CHECK_EQ(at_pps > 0, true);
    }
}
// A list that is not a superset of the plan re-plans per row (pattern budget-reannounce-mid-run): the rows it no longer offers
// are withdrawn by the source and the rest follow and run. Only another wire revision is still "offers changed".
static void test_a_genuinely_different_list_replans_per_row_but_another_revision_is_caps_changed() {
    for (unsigned variant = 0; variant < 4; ++variant) {
        Fake f; f.caps = baseus70_caps();
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, with_units(kPd.selected, {ct::Unit::Pd}), f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r); }), true);
        pd::SourceCaps c = baseus70_reannounced(false);
        if (variant == 0) c.pdos[4] = pd::decode_pdo(((20000u / 50) << 10) | 200);          // 20 V 3.5 A -> 2 A
        if (variant == 1) c.pdos[5] = pd::decode_pdo(0xc0000000u | (90u << 17) | (33u << 8) | 100);   // PPS 3.3-9 V
        if (variant == 2) { c.pdos[3] = c.pdos[4]; c.pdos[4] = c.pdos[5]; c.num_pdos = 5; c.pdos[5] = {}; }   // the 15 V PDO is gone
        if (variant == 3) c.spec_rev = 1;
        f.caps = c; f.caps.valid = true;
        f.tick(r, 170000);
        CHECK_EQ(r.state(), ct::State::Done);
        if (variant == 3) {   // another wire revision: a different charger, "offers changed" as before
            CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged) > 0, true);
            continue;
        }
        CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
        if (variant == 0) {
            // 20 V 3.5 A -> 2 A is only a current drop: per the 2026-11-09 decision the fixed row keeps its row and runs
            // at the new, lower current word - nothing is withdrawn.
            CHECK_EQ(count_reason_all(r, ct::Reason::Withdrawn), 0u);
            for (const auto& s : r.steps())
                if (s.mv == 20000 && !s.hunt) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 2000u); }
        } else {
            CHECK_EQ(count_reason_all(r, ct::Reason::Withdrawn) > 0, true);
        }
        CHECK_EQ(only_pass_or_skip(r), true);
        CHECK_EQ(f.replans.empty(), variant == 0);   // a word-only change is not a re-plan
    }
}

// ---- pattern budget-reannounce-mid-run: a mid-run re-announce that drops one offer re-plans the PD rows per row ----
// Anker 2-way splitter behind a Baseus 140 W (2026-11-09, runs/anker-split): first list 5/9/15 V 1.5 A + 20 V 2.5 A; ~4 s into
// the run it re-split its budget with the dock on its other output and re-announced 5 V 1.5 A, 9 V 3 A, 15 V 3 A (20 V gone).
static pd::SourceCaps splitter_caps() {
    pd::SourceCaps c; c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.num_pdos = 4; c.raw_header = 0x7181;
    const unsigned volts[] = {5000, 9000, 15000, 20000}, mas[] = {150, 150, 150, 250};
    for (unsigned n = 0; n < 4; ++n) c.pdos[n] = pd::decode_pdo(((volts[n] / 50) << 10) | mas[n]);
    return c;
}
static pd::SourceCaps splitter_reannounced() {   // 20 V withdrawn, 9 V and 15 V raised to 3 A
    pd::SourceCaps c; c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.num_pdos = 3; c.raw_header = 0x7181;
    const unsigned volts[] = {5000, 9000, 15000}, mas[] = {150, 300, 300};
    for (unsigned n = 0; n < 3; ++n) c.pdos[n] = pd::decode_pdo(((volts[n] / 50) << 10) | mas[n]);
    return c;
}
static pd::SourceCaps splitter_rebalanced_back() {   // the re-announced list with the 20 V PDO back at 2.5 A
    pd::SourceCaps c = splitter_reannounced();
    c.num_pdos = 4;
    c.pdos[3] = pd::decode_pdo((400u << 10) | 250);
    return c;
}
static const ct::Options kSplitterPd = with_units(ct::bit(A::Pd2Fixed) | ct::bit(A::Pd3Fixed), {ct::Unit::Pd});
// The re-announce lands between rows (while the 9 V row is in flight) or while the 15 V row is in flight: the 15 V row follows
// the new list and passes at the new 3 A word, both 20 V rows (the ordinary one and the 6 A hunt) are withdrawn by the source,
// the unit is not ended, one re-plan ran.
static void test_midrun_reannounce_dropping_one_offer_replans_per_row() {
    for (const unsigned when : {0u, 1u}) {
        Fake f; f.caps = splitter_caps();
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
        if (when == 0) CHECK_EQ(run_until(f, r, [&] { return f.pd_requests >= 2; }), true);   // the 5 V row done, the 9 V row in flight
        else CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 15000; }), true);
        f.caps = splitter_reannounced();
        f.tick(r, 200000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
        unsigned v15 = 0, v20 = 0;
        for (const auto& s : r.steps()) {
            if (s.mv == 15000) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 3000u); ++v15; }
            if (s.mv == 20000) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::Withdrawn); ++v20; }
        }
        CHECK_EQ(v15, 1u); CHECK_EQ(v20, 2u);   // the 20 V row and its 6 A hunt
        // The 15 V request went to the new slot (PDO 3), one Request per row, never two in flight.
        unsigned at15 = 0;
        for (const auto& c : f.choices) if (c.pos == 3 && c.mv == 15000) ++at15;
        CHECK_EQ(at15, 1u);
        CHECK_EQ(f.pd_requests, 3u);   // 5 V, 9 V, 15 V: the re-plan sent no Request of its own
        CHECK_EQ(f.replans.size(), 1u);
        CHECK_EQ(f.replans[0][1] >= 2u, true);   // both 20 V rows withdrawn in the one re-plan
        CHECK_EQ(r.evidence().rebalanced, 1u);
        CHECK_EQ(f.plan_changes >= 1, true);
    }
}
// The row in flight when the list changes loses its offer (here the 15 V PDO is withdrawn while the 15 V row runs): it ends as
// withdrawn and is released to 5 V (the sink's single-Request gate serialises that release); the rows still offered follow the
// new list and run.
static void test_midrun_reannounce_withdrawing_the_row_in_flight_ends_it_and_releases() {
    Fake f; f.caps = splitter_caps();
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 15000; }), true);
    const unsigned releases_before = f.releases;
    pd::SourceCaps c; c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.num_pdos = 3; c.raw_header = 0x7181;
    c.pdos[0] = pd::decode_pdo((100u << 10) | 150);   // 5 V 1.5 A
    c.pdos[1] = pd::decode_pdo((180u << 10) | 150);   // 9 V 1.5 A
    c.pdos[2] = pd::decode_pdo((400u << 10) | 250);   // 20 V 2.5 A: the 15 V PDO is withdrawn
    f.caps = c;
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    for (const auto& s : r.steps()) {
        if (s.mv == 15000) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::Withdrawn); }
        if (s.mv == 20000 && !s.hunt) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(s.pos, 3u); }
        if (s.mv < 15000) CHECK_EQ(s.verdict, ct::Verdict::Pass);
    }
    CHECK_EQ(f.releases > releases_before, true);   // the withdrawn row was released to 5 V
    CHECK_EQ(f.in.vbus_mv, 5000);
    CHECK_EQ(f.replans.size(), 1u);
    CHECK_EQ(f.replans[0][1] >= 1u, true);   // the row in flight counted as withdrawn
}
// A withdrawn PDO that comes back revives its rows (pending again, not left as withdrawn), as long as the unit has not ended;
// several re-announces in one run are normal (re-plan each time, no counter that gives up).
static void test_a_withdrawn_pdo_that_comes_back_revives_its_rows() {
    Fake f; f.caps = splitter_caps();
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 9000; }), true);
    f.caps = splitter_reannounced(); f.tick(r, 100);   // 20 V withdrawn
    CHECK_EQ(f.replans.size(), 1u);
    f.caps = splitter_rebalanced_back(); f.tick(r, 100);   // the splitter re-balanced back: the 20 V PDO is offered again
    CHECK_EQ(f.replans.size(), 2u);
    f.caps = splitter_reannounced(); f.tick(r, 100);   // and away again
    CHECK_EQ(f.replans.size(), 3u);
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(r.evidence().rebalanced, 3u);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    CHECK_EQ(f.replans[1][2] >= 2u, true);   // the second re-plan brought both 20 V rows back
    unsigned v20 = 0, v15 = 0;
    for (const auto& s : r.steps()) {
        if (s.mv == 20000) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::Withdrawn); ++v20; }
        if (s.mv == 15000) { CHECK_EQ(s.verdict, ct::Verdict::Pass); ++v15; }
    }
    CHECK_EQ(v20, 2u); CHECK_EQ(v15, 1u);   // the third re-announce withdrew them again before they ran; the 15 V row passed
}
// A re-announce that places no PD row at all - the row in flight included - is still a different charger: "offers changed".
static void test_a_reannounce_that_places_no_row_is_still_offers_changed() {
    Fake f; f.caps = splitter_caps();
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return f.pd_requests >= 1; }), true);   // the 5 V row is in flight
    f.caps = splitter_reannounced(); f.caps.num_pdos = 1;
    f.caps.pdos[0] = pd::decode_pdo((240u << 10) | 300);   // only a 12 V 3 A PDO: no offered voltage matches any row (not even the 5 V row in flight)
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged) > 0, true);
    CHECK_EQ(f.replans.empty(), true);
    CHECK_EQ(r.evidence().rebalanced, 0u);
}
// (A current-only drop on a fixed PDO does NOT count as "places no row": per the 2026-11-09 decision the fixed row whose
// voltage is still offered keeps its row and runs at the new, lower current word - see the fixed-offer test below.)

// ---- review of k2/reannounce-replan (2026-11-09), red first: the re-plan semantics of the mid-run re-announce ----

// A source with one PPS APDO besides the fixed PDOs: 5 V 3 A, 9 V 3 A, APDO lo-hi V at ma50 * 50 mA.
static pd::SourceCaps pps_src_caps(unsigned apdo_ma50 = 100, unsigned apdo_hi_dv = 110, unsigned apdo_lo_dv = 33) {
    pd::SourceCaps c; c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.num_pdos = 3; c.raw_header = 0x7181;
    c.pdos[0] = pd::decode_pdo((100u << 10) | 300);   // 5 V 3 A
    c.pdos[1] = pd::decode_pdo((180u << 10) | 300);   // 9 V 3 A
    c.pdos[2] = pd::decode_pdo(0xc0000000u | (apdo_hi_dv << 17) | (apdo_lo_dv << 8) | apdo_ma50);
    return c;
}
// 5/9/15/20 V, 15 V at v15_ma (10 mA units): a fixed list whose one current can drop.
static pd::SourceCaps four_pdo_caps(unsigned v15_ma) {
    pd::SourceCaps c; c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.num_pdos = 4; c.raw_header = 0x7181;
    const unsigned volts[] = {5000, 9000, 15000, 20000}, mas[] = {300, 300, v15_ma, 300};
    for (unsigned n = 0; n < 4; ++n) c.pdos[n] = pd::decode_pdo(((volts[n] / 50) << 10) | mas[n]);
    return c;
}
// Item 1: only rows of the running unit may be withdrawn or counted as placeable. The APDO disappears during the
// Pd unit and comes back before the PdLow unit runs: the PdLow row stays Pending the whole time (never withdrawn
// for good - revival used to look at the running unit only) and runs when its unit comes.
static void test_a_pps_change_during_the_pd_unit_does_not_withdraw_the_pdlow_rows() {
    {   // the APDO comes back: the PdLow row was never withdrawn and runs
        Fake f; f.caps = pps_src_caps();
        ct::Runner r(f);
        const auto o = with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Pd, ct::Unit::PdLow});
        CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 9000; }), true);
        pd::SourceCaps gone = pps_src_caps(); gone.num_pdos = 2;   // the APDO is withdrawn mid-run
        f.caps = gone;
        f.tick(r, 100);
        for (const auto& s : r.steps())
            if (ct::unit_of(s) == ct::Unit::PdLow) CHECK_EQ(s.verdict, ct::Verdict::Pending);   // not withdrawn here
        f.caps = pps_src_caps();   // and back before the PdLow unit
        f.tick(r, 400000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
        CHECK_EQ(only_pass_or_skip(r), true);
        unsigned low_pass = 0;
        for (const auto& s : r.steps())
            if (ct::unit_of(s) == ct::Unit::PdLow && s.verdict == ct::Verdict::Pass) ++low_pass;
        CHECK_EQ(low_pass, 1u);   // the 3.3 V row ran and passed
    }
    {   // the APDO stays gone: right after the re-announce the PdLow row is still Pending (red today: Skip/Withdrawn)
        Fake f; f.caps = pps_src_caps();
        ct::Runner r(f);
        const auto o = with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Pd, ct::Unit::PdLow});
        CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 9000; }), true);
        pd::SourceCaps gone = pps_src_caps(); gone.num_pdos = 2;
        f.caps = gone;
        const int pc = f.plan_changes;
        f.tick(r, 100);
        for (const auto& s : r.steps())
            if (ct::unit_of(s) == ct::Unit::PdLow) { CHECK_EQ(s.verdict, ct::Verdict::Pending); CHECK_EQ(int(s.reason), int(ct::Reason::None)); }
        CHECK_EQ(f.plan_changes, pc + 1);   // exactly the running unit's own PPS rows were withdrawn; the PdLow rows added no count
    }
}
// Item 2: rebalanced / revision_ / plan_changed() only when a row was withdrawn, came back or moved slot. A
// re-announce that only raises a current (the Xiaomi post-auth 5 -> 6 A) changes nothing for the plan: the count
// stays exactly where it was.
static void test_a_reannounce_that_only_raises_a_current_counts_nothing() {
    Fake f; f.caps = pps_src_caps(100);   // APDO 3.3-11 V 5 A
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 9000; }), true);
    const int pc = f.plan_changes;
    f.caps = pps_src_caps(120);   // the same APDO at 6 A: same slot, same range, more current
    f.tick(r, 100);
    CHECK_EQ(f.plan_changes, pc);            // == : no row moved, withdrew or came back
    CHECK_EQ(r.evidence().rebalanced, 0u);
    CHECK_EQ(f.replans.empty(), true);
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::Withdrawn), 0u);
    unsigned pps_pass = 0;
    for (const auto& s : r.steps())
        if (s.algorithm == A::Pps && s.verdict == ct::Verdict::Pass && s.ma <= 1000) {
            ++pps_pass;
            CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 6000u);   // the rows run at the new, higher word
        }
    CHECK_EQ(pps_pass >= 3u, true);
}
// Item 3: a programmable row is still offered when the type matches, its target voltage is inside the new range
// and its current fits the new limit. A PPS current drop 5 A -> 3 A withdraws only the rows that asked more than
// 3 A (the full-current row); the 1 A rows run at the new word. A range that no longer covers a target withdraws
// that row alone.
static void test_a_pps_change_keeps_the_rows_whose_target_still_fits() {
    {   // APDO 3.3-11 V 5 A -> 3.3-11 V 3 A
        Fake f; f.caps = pps_src_caps(100);
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 9000; }), true);
        f.caps = pps_src_caps(60);   // 3 A
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
        unsigned full_withdrawn = 0, pps_pass = 0;
        for (const auto& s : r.steps()) {
            if (s.algorithm != A::Pps) continue;
            if (s.ma > 3000 && s.reason == ct::Reason::Withdrawn) ++full_withdrawn;   // the full-current row (5 A) and the 6 A hunt: 3 A no longer fits
            if (s.verdict == ct::Verdict::Pass && s.ma <= 1000) {
                ++pps_pass;
                CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 3000u);   // kept, at the new lower word
            }
        }
        CHECK_EQ(full_withdrawn, 2u);
        CHECK_EQ(pps_pass >= 3u, true);
    }
    {   // APDO 3.3-11 V -> 7-11 V: the 5.2 V row's target is outside the new range, the higher ones run
        Fake f; f.caps = pps_src_caps(100, 110, 33);
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps), {ct::Unit::Pd}), f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 9000; }), true);
        f.caps = pps_src_caps(100, 110, 70);   // 7-11 V
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
        unsigned low_out = 0, pps_pass = 0;
        for (const auto& s : r.steps()) {
            if (s.algorithm != A::Pps) continue;
            if (s.mv < 7000 && s.reason == ct::Reason::Withdrawn) ++low_out;
            if (s.verdict == ct::Verdict::Pass) ++pps_pass;
        }
        CHECK_EQ(low_out >= 1u, true);   // the target below the new range is withdrawn
        CHECK_EQ(pps_pass >= 2u, true);
    }
}
// Item 4 (user decision 2026-11-09): a fixed offer whose current drops (15 V 3 A -> 15 V 1.5 A) keeps its row: it
// runs at the new, lower current word and the card notes the reduced current. Only a voltage that is no longer
// offered is Withdrawn. A word-only change is not a re-plan: the count stays 0.
static void test_a_fixed_offer_whose_current_drops_keeps_its_row_at_the_lower_word() {
    Fake f; f.caps = four_pdo_caps(300);   // 15 V 3 A
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return f.pd_requests >= 2; }), true);   // the 5 V row done, the 9 V row in flight
    f.caps = four_pdo_caps(150);   // 15 V 1.5 A: only the current drops
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::Withdrawn), 0u);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    CHECK_EQ(r.evidence().rebalanced, 0u);   // a word-only change is not a re-plan
    CHECK_EQ(f.replans.empty(), true);   // no re-plan was logged for it either
    for (const auto& s : r.steps())
        if (s.mv == 15000 && !s.hunt) {
            CHECK_EQ(s.verdict, ct::Verdict::Pass);
            CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 1500u);   // the reduced current is on the row / the card
        }
    unsigned at15 = 0;
    for (const auto& ch : f.choices) if (ch.pos == 3 && ch.mv == 15000) ++at15;
    CHECK_EQ(at15, 1u);
}
// Item 5: a revived row actually runs and passes (the withdrawn PDO comes back and its rows are pending again).
static void test_a_revived_row_actually_runs_and_passes() {
    Fake f; f.caps = splitter_caps();
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 9000; }), true);
    f.caps = splitter_reannounced(); f.tick(r, 100);   // 20 V withdrawn
    CHECK_EQ(f.replans.size(), 1u);
    f.caps = splitter_rebalanced_back(); f.tick(r, 100);   // 20 V back: the rows are pending again
    CHECK_EQ(f.replans.size(), 2u);
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    for (const auto& s : r.steps())
        if (s.mv == 20000 && !s.hunt) CHECK_EQ(s.verdict, ct::Verdict::Pass);   // the revived row ran and passed
    CHECK_EQ(r.evidence().rebalanced, 2u);
}
// Item 5: a "between rows" re-announce - the list changes while the runner is in Releasing (after a row finished,
// before the next one) or in the Baseline - re-plans the pending rows, with the counts exactly right.
static void test_a_reannounce_between_rows_replans_with_the_counts_exact() {
    {   // a tick in Releasing: the 5 V row is done, its release is in progress
        Fake f; f.caps = splitter_caps();
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Releasing && f.pd_requests >= 1; }), true);
        const int pc = f.plan_changes;
        f.caps = splitter_reannounced();   // 20 V withdrawn, 9/15 V raised to 3 A
        f.tick(r, 100);
        CHECK_EQ(f.replans.size(), 1u);
        CHECK_EQ(f.plan_changes, pc + 1);   // == : exactly one re-plan
        CHECK_EQ(r.evidence().rebalanced, 1u);
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
        unsigned v20 = 0, v15 = 0;
        for (const auto& s : r.steps()) {
            if (s.mv == 20000) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::Withdrawn); ++v20; }
            if (s.mv == 15000) { CHECK_EQ(s.verdict, ct::Verdict::Pass); ++v15; }
        }
        CHECK_EQ(v20, 2u); CHECK_EQ(v15, 1u);
    }
    {   // a tick in the first Baseline (no PD row has started): the plan is rebuilt from the new list, no re-plan count
        Fake f; f.caps = splitter_caps();
        ct::Runner r(f);
        CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
        CHECK_EQ(run_until(f, r, [&] { return r.state() == ct::State::Baseline && f.attaches_pd >= 1 && f.pd_requests == 0; }), true);
        const int pc = f.plan_changes;
        f.caps = splitter_reannounced();
        f.tick(r, 100);
        CHECK_EQ(f.replans.empty(), true);
        CHECK_EQ(r.evidence().rebalanced, 0u);
        f.tick(r, 300000);
        CHECK_EQ(r.state(), ct::State::Done);
        CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
        CHECK_EQ(only_pass_or_skip(r), true);
        unsigned v20 = 0;
        for (const auto& s : r.steps()) if (s.mv == 20000) ++v20;
        CHECK_EQ(v20, 0u);   // replanned from the new list: no 20 V rows in the plan at all
    }
}
// Nit: a row whose offer only moved slot is re-queued, not Withdrawn - the row in flight included (an APDO
// inserted ahead of its fixed PDO moves it from slot 3 to slot 4: the running row finishes at the new slot).
static void test_a_slot_only_move_of_the_row_in_flight_requeues_it() {
    Fake f; f.caps = splitter_caps();   // 5/9/15/20 V at slots 1..4
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 15000; }), true);
    pd::SourceCaps c; c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.num_pdos = 5; c.raw_header = 0x7181;
    c.pdos[0] = pd::decode_pdo((100u << 10) | 150);   // 5 V 1.5 A
    c.pdos[1] = pd::decode_pdo((180u << 10) | 150);   // 9 V 1.5 A
    c.pdos[2] = pd::decode_pdo(0xc0000000u | (210u << 17) | (33u << 8) | 66);   // a PPS APDO inserted at slot 3
    c.pdos[3] = pd::decode_pdo((300u << 10) | 150);   // 15 V 1.5 A, now slot 4
    c.pdos[4] = pd::decode_pdo((400u << 10) | 250);   // 20 V 2.5 A, slot 5
    f.caps = c;
    f.in.pd.pos = 4;   // the sink answered the re-announce by re-requesting the held 15 V at its new slot
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::Withdrawn), 0u);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    for (const auto& s : r.steps())
        if (s.mv == 15000) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(s.pos, 4u); }   // re-queued at the new slot
    CHECK_EQ(r.evidence().rebalanced, 1u);   // the slot move is the one re-plan
    unsigned at15 = 0;
    for (const auto& ch : f.choices) if (ch.mv == 15000) ++at15;
    CHECK_EQ(at15, 1u);   // the row in flight was not ended and re-asked
}

// ---- Restart rules for a mid-run re-announce (user decision 2026-11-09), red first ----

// Rule 1 (settle first): two lists 300 ms apart -> ONE Request for the next row, sent only after the SECOND list has
// been unchanged for kListSettleMs; each list arriving inside the window restarts it.
static void test_two_lists_300ms_apart_send_one_request_after_the_second_settles() {
    Fake f; f.caps = splitter_caps();
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return f.pd_requests >= 1 && r.steps()[r.index()].verdict == ct::Verdict::Pass; }), true);   // the 5 V row passed
    f.caps = splitter_reannounced();   // list 1: 20 V withdrawn, 9/15 V raised to 3 A
    f.tick(r, 300);
    const uint32_t t1 = f.in.now;
    pd::SourceCaps again = splitter_reannounced();
    again.pdos[1] = pd::decode_pdo((180u << 10) | 250);   // list 2, 300 ms after list 1: a word-only change on 9 V
    f.caps = again;
    f.tick(r, 30000);
    unsigned at9 = 0; uint32_t t9 = 0;
    for (unsigned n = 0; n < f.choices.size(); ++n)
        if (f.choices[n].mv == 9000) { ++at9; t9 = f.req_times[n]; }
    CHECK_EQ(at9, 1u);                                   // one Request for the 9 V row, not two
    CHECK_EQ(t9 >= t1 + ct::kListSettleMs, true);        // ... and only after the second list settled
    f.tick(r, 200000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    CHECK_EQ(f.replans.size(), 1u);   // only list 1 (20 V withdrawn) was a re-plan; the word-only list 2 was not logged
    unsigned v20 = 0;
    for (const auto& s : r.steps())
        if (s.mv == 20000) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::Withdrawn); ++v20; }
    CHECK_EQ(v20, 2u);
}
// Item 7 + rule 2: a re-split during row 0 delivered as a COMPLETE EPR_Source_Capabilities (outside the K2's own EPR
// entry/exit - Epr is not selected, epr_stage_ == 0) is a re-announce like any other and goes through the per-row
// re-plan: row 0 restarts at the new current word and passes with tries = 0 (a restart is not a try), the still
// offered rows follow at their new words, only the dropped 20 V is Withdrawn, nothing ends CapsChanged.
static void test_a_complete_epr_reannounce_during_row_0_restarts_the_row() {
    Fake f; f.caps = four_pdo_caps(300);   // 5/9/15/20 V 3 A, SPR list, no EPR bit
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 5000; }), true);
    pd::SourceCaps e; e.valid = true; e.spec_rev = 2; e.active_cc = 1; e.raw_header = 0x7181;
    e.epr = true; e.epr_partial = false; e.num_pdos = 8;   // a complete 8-PDO EPR_Source_Capabilities
    e.pdos[0] = pd::decode_pdo((100u << 10) | 150);   // 5 V 1.5 A
    e.pdos[1] = pd::decode_pdo((180u << 10) | 150);   // 9 V 1.5 A
    e.pdos[2] = pd::decode_pdo((300u << 10) | 150);   // 15 V 1.5 A: the 20 V PDO is gone
    e.pdos[7] = pd::decode_pdo((560u << 10) | 400);   // EPR 28 V 4 A at position 8
    f.caps = e;
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    for (const auto& s : r.steps()) {
        if (s.mv == 5000) {
            CHECK_EQ(s.verdict, ct::Verdict::Pass);
            CHECK_EQ(s.tries, 0u);                             // the restart is not a try
            CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 1500u);     // row 0 runs at the new, lower current word
        }
        if (s.mv == 9000 || s.mv == 15000) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 1500u); }
        if (s.mv == 20000) { CHECK_EQ(s.verdict, ct::Verdict::Skip); CHECK_EQ(s.reason, ct::Reason::Withdrawn); }
    }
    CHECK_EQ(r.evidence().rebalanced, 1u);
    CHECK_EQ(f.replans.size(), 1u);
}
// Item 6 (the A3 run): a re-split during the 15 V row keeps the 15/20/28 V rows at the new, lower current words and
// withdraws only the PPS rows; no row ends CapsChanged.
static void test_a_resplit_during_15v_keeps_15_20_28_at_the_new_words_and_withdraws_pps() {
    Fake f; f.in.cap_mv = 36000; f.in.cable_mv = 50000; f.in.cable_ma = 5000; f.in.cable_observed = true;
    pd::SourceCaps full; full.valid = true; full.spec_rev = 2; full.active_cc = 1; full.raw_header = 0x7181;
    full.epr = true; full.num_pdos = 8;   // 5/9/12/15 V 3 A, 20 V 5 A, PPS 3.3-11 V 5 A (slot 7), EPR 28 V 5 A (slot 8)
    const unsigned volts[] = {5000, 9000, 12000, 15000, 20000};
    for (unsigned n = 0; n < 5; ++n) full.pdos[n] = pd::decode_pdo(((volts[n] / 50) << 10) | (n == 4 ? 500u : 300u));
    full.pdos[5] = pd::decode_pdo(0xc0000000u | (110u << 17) | (33u << 8) | 100);
    full.pdos[7] = pd::decode_pdo((560u << 10) | 500);
    f.caps = full;
    ct::Runner r(f);
    const auto o = with_units(ct::bit(A::Pd3Fixed) | ct::bit(A::Pps) | ct::bit(A::Epr), {ct::Unit::Pd});
    CHECK_EQ(r.start(f.caps, o, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 15000; }), true);
    pd::SourceCaps cut = full;   // the re-split: 15 V 1.5 A, 20 V 4.1 A, the PPS APDO gone, EPR 28 V 4 A
    cut.pdos[3] = pd::decode_pdo((300u << 10) | 150);
    cut.pdos[4] = pd::decode_pdo((400u << 10) | 410);
    cut.pdos[5] = {};
    cut.pdos[7] = pd::decode_pdo((560u << 10) | 400);
    f.caps = cut;
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    unsigned pps_w = 0;
    for (const auto& s : r.steps()) {
        if (s.mv == 15000 && !s.hunt) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 1500u); }
        if (s.mv == 20000 && !s.hunt) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 4100u); }
        if (s.algorithm == A::Epr && s.mv == 28000) { CHECK_EQ(s.verdict, ct::Verdict::Pass); CHECK_EQ(pd::decode_pdo(s.pdo).max_ma, 4000u); }
        if (s.algorithm == A::Pps && s.reason == ct::Reason::Withdrawn) ++pps_w;
    }
    CHECK_EQ(pps_w >= 1u, true);   // only the PPS rows were withdrawn
    CHECK_EQ(r.evidence().rebalanced >= 1u, true);
}
// Rule 4 (end-of-unit sweep): a list restored before the unit ends runs the rows that came back - the Withdrawn 20 V
// rows are re-checked once against the list in hand (after a settle) and run, not left as withdrawn.
static void test_a_list_restored_before_the_unit_ends_runs_the_withdrawn_rows() {
    Fake f; f.caps = splitter_caps();
    ct::Runner r(f);
    CHECK_EQ(r.start(f.caps, kSplitterPd, f.in, true), ct::Reason::None);
    CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 9000; }), true);
    f.caps = splitter_reannounced(); f.tick(r, 100);   // 20 V withdrawn
    CHECK_EQ(f.replans.size(), 1u);
    CHECK_EQ(run_until(f, r, [&] { return a_pd_row_is_running(r) && r.steps()[r.index()].mv == 15000; }), true);
    f.caps = splitter_rebalanced_back();   // the splitter re-balanced back before the unit ended: 20 V offered again
    f.tick(r, 300000);
    CHECK_EQ(r.state(), ct::State::Done);
    CHECK_EQ(count_reason_all(r, ct::Reason::CapsChanged), 0u);
    unsigned v20_pass = 0;
    for (const auto& s : r.steps())
        if (s.mv == 20000 && !s.hunt) { CHECK_EQ(s.verdict, ct::Verdict::Pass); ++v20_pass; }
    CHECK_EQ(v20_pass, 1u);   // the restored 20 V row ran before the unit closed
    CHECK_EQ(r.evidence().rebalanced >= 2u, true);
}

int main() {
    test_an_above_5v_request_waits_for_the_journal_without_blocking();
    test_reannounce_before_the_first_row_replans(); test_reannounce_during_the_first_row_follows_the_new_slots();
    test_a_genuinely_different_list_replans_per_row_but_another_revision_is_caps_changed();
    test_midrun_reannounce_dropping_one_offer_replans_per_row();
    test_midrun_reannounce_withdrawing_the_row_in_flight_ends_it_and_releases();
    test_a_withdrawn_pdo_that_comes_back_revives_its_rows();
    test_a_reannounce_that_places_no_row_is_still_offers_changed();
    test_a_pps_change_during_the_pd_unit_does_not_withdraw_the_pdlow_rows();
    test_a_reannounce_that_only_raises_a_current_counts_nothing();
    test_a_pps_change_keeps_the_rows_whose_target_still_fits();
    test_a_fixed_offer_whose_current_drops_keeps_its_row_at_the_lower_word();
    test_a_revived_row_actually_runs_and_passes();
    test_a_reannounce_between_rows_replans_with_the_counts_exact();
    test_a_slot_only_move_of_the_row_in_flight_requeues_it();
    test_two_lists_300ms_apart_send_one_request_after_the_second_settles();
    test_a_complete_epr_reannounce_during_row_0_restarts_the_row();
    test_a_resplit_during_15v_keeps_15_20_28_at_the_new_words_and_withdraws_pps();
    test_a_list_restored_before_the_unit_ends_runs_the_withdrawn_rows();
    test_identity_follows_the_unit_mode(); test_identity_is_back_for_the_pd_unit_after_a_legacy_one();
    test_identity_matches_every_attach_of_the_default_order(); test_self_powered_run_never_touches_the_identity();
    test_self_powered_start_plans_from_the_held_caps(); test_self_powered_start_without_caps_but_pd_seen_says_pd_gave_up();
    test_missing_caps_without_the_self_power_evidence_stay_not_advertised();
    test_self_path_policy(); test_self_powered_pd_unit_soft_resets(); test_self_powered_legacy_units_hold_silently();
    test_self_powered_unit_boundaries(); test_self_powered_skips_the_low_programmable_rows();
    test_self_powered_soft_reset_without_caps_fails_the_unit(); test_self_powered_blocked_legacy_unit_never_detaches();
    test_self_powered_cancel_in_a_legacy_hold_restores_pd();
    test_baseline_epr_entry_cut_by_a_source_hard_reset_is_retried(); test_epr_entry_waits_after_a_rejected_row();
    test_resume_keeps_the_epr_proof_of_an_unread_cable(); test_resume_replans_the_late_pd_rows(); test_resume_replans_the_late_pd_rows_with_epr();
    test_epr_row_cut_by_a_source_hard_reset_passes_on_retry();
    test_fc_release_is_issued_once_per_row(); test_fcp_rows_after_the_first_are_retargets_in_one_session(); test_5v_row_needs_the_handshake(); test_source_drop_recovers_on_a_non_pd_source(); test_a_lost_row_ends_the_session_so_the_next_row_handshakes_again(); test_session_blip_with_vbus_up_is_not_source_lost(); test_source_gone_stops_the_run(); test_pd_row_survives_one_source_hard_reset(); test_pd_row_gives_up_after_a_second_source_hard_reset();
    test_dark_source_that_returns_is_not_a_change(); test_apdo_current_grow_is_not_offers_changed(); test_dark_source_that_never_returns_is_source_lost(); test_pd_row_cut_twice_by_hard_resets_fails_and_the_run_goes_on(); test_ordinary_pd_row_answered_by_a_source_hard_reset_is_retried_once(); test_ordinary_pd_row_cut_by_a_second_source_hard_reset_fails_with_the_reason(); test_pps_low_step_landing_below_the_band_fails_only_that_row();
    test_low_programmable_rows_are_planned_last(); test_latch_off_at_the_low_step_ends_done_with_a_reason(); test_latch_off_that_returns_is_not_shut_off(); test_cut_that_recovers_on_its_own_is_recorded_as_such(); test_no_request_into_an_invalid_list_after_a_recovered_cut(); test_every_reason_has_a_name(); test_every_failure_kind_has_a_capture_reason();
    test_skip_while_held_releases_before_done(); test_release_retried_until_sink_lets_go();
    test_vivo_plan_runs_the_engine();
    test_pd_bundles();
    test_source_probe_costs_at_most_a_second(); test_source_probe_only_in_a_pd_contract_and_once(); test_source_probe_skipped_without_a_contract();
    test_hunt_runs_on_the_pps_apdo_that_covers_the_voltage(); test_preview_does_not_count_hunt_rows_the_run_drops();
    test_fixed_only_source_keeps_the_fixed_20v_hunt();
    test_fixed_20v_hunt_stays_when_no_apdo_reaches_20v(); test_fixed_20v_hunt_stays_when_the_pps_rows_are_not_selected();
    test_pps_full_current_row_with_a_5a_cable(); test_pps_full_current_row_clamps_to_an_unread_cable();
    test_pps_over_5a_apdo_with_a_5a_cable_is_a_cable_limit_skip(); test_pps_over_5a_apdo_with_an_unread_cable_is_a_cable_limit_skip();
    test_pps_over_5a_apdo_with_a_spoofed_6a_cable_asks_the_advertised_current();
    test_pps_full_current_row_of_a_2a_apdo(); test_pps_full_current_row_follows_the_apdos_rows_and_hunt();
    test_pps_full_current_row_is_not_a_duplicate_of_the_1a_row(); test_pps_full_current_row_off_when_pps_is_not_selected();
    test_pps_full_current_rows_fit_the_plan_of_a_legal_list(); test_pps_full_current_rows_never_overflow_the_plan();
    test_hunt_waits_for_the_xiaomi_auth_chain(); test_unread_cable_limit_follows_the_source_offers();
    test_hunt_the_source_does_not_offer_reads_not_unlocked(); test_source_that_only_asks_our_svids_drops_the_hunt_rows();
    test_hunt_with_a_spoofed_identity_uses_its_cable_limits();
    test_hunt_refused_with_a_proven_cable_and_chain_stays_a_failure();
    test_hunt_accepted_after_the_chain_passes();
    test_hunt_without_the_chain_and_without_6a_sends_no_request_and_leaves_no_row();
    test_hunt_after_the_chain_with_the_list_still_at_5a_is_a_neutral_skip();
    test_hunt_after_the_chain_with_the_list_re_announced_at_6a_asks_for_6a();
    test_preauth_list_after_a_hunt_reset_is_not_offers_changed(); test_a_cut_short_list_that_never_grows_is_still_offers_changed();
    test_a_cut_short_list_without_a_xiaomi_chain_is_still_offers_changed();
    test_hunt_reject_then_the_same_list_again_keeps_the_rows();
    test_hunt_reject_then_a_list_without_the_pps_apdos_skips_only_those_rows();
    test_an_unsolicited_list_change_during_a_normal_row_replans_per_row();
    test_hunt_answered_by_a_hard_reset_reads_not_unlocked(); test_hunt_hard_reset_keeps_the_chain_and_cable_reasons();
    test_hunt_evidence_names_each_refusal_cause(); test_hunt_evidence_records_what_the_sink_saw();
    test_hunt_evidence_marks_a_silent_source_as_a_timeout(); test_hunt_evidence_without_a_trace_is_unexplained();
    test_hunt_evidence_only_for_refused_rows(); test_classify_hunt_precedence();
    test_plan_and_run(); test_an_auto_hold_counts_as_released(); test_quiet_is_exactly_the_row_in_flight(); test_paint_blocked_needs_flight_and_an_engine_window(); test_refusals(); test_hold_keeps_the_baseline_until_released(); test_safe5_baseline();test_cancel_and_failed_release(); test_release_waits_for_the_sink_verification_and_its_hard_reset();
    test_faults(); test_loop_lock_refuses_and_aborts(); test_timeouts_and_skips(); test_one_second_holds(); test_qc2_settles_within_two_percent(); test_qc2_5v_accepts_vsafe5_reading(); test_legacy_5v_accepts_vsafe5_reading(); test_qc3_rows_use_an_asymmetric_band(); test_legacy_rows_use_a_five_percent_band(); test_legacy_5v_row_uses_the_vsafe5_window(); test_scp_4v5_row_judges_the_adapter_readback(); test_qc3plus_without_response_is_a_distinct_skip(); test_unsupported_protocol_skips_its_other_rows(); test_silent_source_fails_every_handshake_row(); test_answered_handshake_passes_the_5v_row(); test_svooc_row_follows_the_adapter_ramp(); test_svooc_detected_is_a_pass(); test_vooc_family_shares_one_session(); test_vivo_no_response_skips_the_other_vivo_rows(); test_pps_min_step_accepts_unloaded_floor(); test_pps_up_step_uses_the_tight_band();
    test_missing_samples_and_wrap();
    test_fixed_band_judges_a_burst_mode_ripple_on_its_mean(); test_fixed_band_still_fails_a_steady_low_or_high_level(); test_fixed_band_keeps_the_per_sample_overvoltage_guards();
    test_pps_renewal();
    test_capped_pps_top_is_recorded_and_the_chain_goes_on(); test_an_uncapped_pps_run_has_no_capped_step(); test_a_pps_stuck_at_5v_is_a_timeout_not_capped();
    test_read_revision();
    test_selection(); test_selected_skip_without_operations(); test_wire_classes_and_feature_evidence();
    test_qc_class_b_is_in_the_default_selection(); test_class_b_on_a_class_a_charger_is_a_neutral_skip(); test_class_b_pass_on_a_class_b_charger_stays_a_pass(); test_class_b_row_on_a_charger_stuck_at_12v_ends_early(); test_qc2_class_b_miss_on_a_qc3_class_b_charger_is_a_fail(); test_qc2_class_b_after_a_failed_qc2_handshake_is_not_class_a_only(); test_class_b_keeps_the_voltage_ceiling_in_front(); test_qc_class_b_rows_are_planned_per_algorithm(); test_qc_rows_after_the_first_are_retargets_in_one_session(); test_a_failed_qc2_retarget_leaves_the_class_b_row_to_run(); test_full_legacy_ranges_and_guards(); test_scp_rows_ignore_the_cable_current(); test_scp_row_aborts_when_a_load_draws_current(); test_scp_rows_follow_the_reported_capabilities(); test_scp_replan_is_announced_once(); test_ufcs_levels_follow_the_reported_modes(); test_ufcs_steps_carry_their_mode(); test_epr_physical_guard_and_spoof_separation(); test_epr_rows_respect_the_36v_ceiling(); test_epr_levels_follow_the_monitor_ceiling(); test_epr_entry_before_rows(); test_epr_info_names_the_levels_without_an_epr_cable(); test_epr_info_refused_or_silent_changes_nothing(); test_epr_info_levels_survive_a_refused_entry(); test_epr_info_padding_word_is_no_offer(); test_epr_info_with_no_epr_pdos_is_named_withheld(); test_epr_info_is_not_asked_when_entry_works(); test_rows_survive_the_exit_to_the_spr_list(); test_overcap_abort_is_the_monitor_ceiling_plus_5_percent();
    test_spr_pps_21v_on_standard_cable();
    test_standard_cable_measured_tolerance();
    test_observed_30v_cable();
    test_avs_current_and_bad_offers(); test_pd_plan_follows_a_list_that_grows_during_the_attach(); test_reannounced_identical_caps_are_not_a_change();
    test_measured_dwell_and_plan_capacity();
    test_usb_a_sag_tail_session_is_not_source_lost(); test_usb_a_sag_tail_waits_for_vbus_to_climb(); test_usb_a_sag_tail_with_dead_or_low_vbus_is_still_lost(); test_session_change_in_baseline_without_a_sag_is_still_lost(); test_usb_a_midrow_session_opens_the_adopt_window(); test_usb_a_sag_dip_under_1v_inside_the_window_waits();
    test_custom_unit_list_runs_in_order(); test_pd_unit_attaches_with_a_contract(); test_a_failed_unit_does_not_stop_the_run();
    test_default_unit_order_is_vendor_modes_then_pd_then_legacy(); test_default_order_runs_vendor_modes_before_pd_before_legacy(); test_pd_between_vendor_and_legacy_units(); test_pd_replan_in_the_middle_keeps_the_rows_in_order(); test_consecutive_legacy_units_share_one_attach(); test_legacy_session_that_sees_pd_traffic_is_renewed(); test_silent_ufcs_source_reports_no_response_not_pd_active(); test_legacy_waits_between_units();
    test_legacy_attach_that_never_falls_back_is_blocked_by_pd(); test_legacy_retry_that_falls_back_runs_the_unit(); test_legacy_attach_waits_out_hard_reset_cycles();
    test_silent_attach_source_that_revives_pd_does_not_go_unsafe(); test_silent_baseline_sag_that_never_returns_is_still_unsafe(); test_silent_sink_output_off_skips_legacy_units_and_runs_pd();
    test_later_unit_output_cut_in_its_baseline_is_a_lost_unit_not_unsafe(); test_later_unit_elevated_or_stuck_baseline_still_ends_unsafe();
    test_unsafe_clears_when_vbus_is_gone_for_the_debounce();
    test_failed_release_at_low_vbus_is_a_lost_source_not_unsafe();
    test_unsafe_clears_when_vbus_stays_under_vsafe5();
    test_pd_attach_without_vbus_or_caps_is_no_reattach(); test_pd_plan_comes_from_the_first_attach(); test_source_gone_in_a_unit_skips_the_remaining_units();
    test_pd_attach_5v_contract_held_at_wrong_voltage_fails_wrong_voltage();
    test_pd_attach_5v_contract_then_rp_cut_before_timeout_fails_wrong_voltage();
    test_pd_attach_without_any_caps_stays_no_reattach();
    test_pd_attach_rp_drop_restarts_the_wait_once();
    test_preflight_pdo1_contract_at_wrong_voltage_refuses_wrong_voltage();
    test_cancel_during_a_unit_restores_the_pd_policy();
    test_resume_reboot_in_request(); test_resume_after_planned_reattach_is_not_counted(); test_pd_replan_rewrites_the_journal(); test_resume_in_a_vendor_unit_goes_on_in_the_default_order(); test_resume_in_the_pd_unit_runs_the_legacy_units_after_it(); test_resume_refusals_and_abandon();
    test_reboot_budget_skips_the_unit_on_the_third_loss(); test_reboot_budget_covers_the_ufcs_unit(); test_legacy_preset_plans_no_pd_rows();
    test_always_on_source_never_stalls(); test_resume_count_is_bounded(); test_resume_cancel_key();
    test_resumed_baseline_adopts_a_session_change(); test_self_powered_vbus_loss_suspends_the_run(); test_hid_powered_vbus_loss_still_stops_the_run();
    test_power_loss_at_every_event_matches_an_uninterrupted_run();
    test_a_final_stop_closes_before_it_skips();
    test_running_grid_lists_the_algorithms_in_the_order_the_plan_runs_them(); test_running_grid_follows_the_users_unit_order();
    test_running_grid_places_the_mi_probes_where_the_plan_runs_them(); test_running_grid_matches_the_order_the_run_worked_through();
    test_running_grid_follows_a_plan_that_is_rebuilt_during_the_run();
    test_pps_accepted_then_dropped_at_no_load_is_needs_load(); test_pps_collapse_with_current_drawn_stays_lost();
    test_fixed_row_collapse_stays_lost(); test_pps_collapse_after_ps_rdy_stays_lost();
    test_low_edge_that_regulates_at_5v_is_a_floored_fail();
    test_pps_step_walk_passes_on_20mv_steps(); test_pps_step_walk_fails_when_steps_round_to_100mv(); test_pps_step_walk_fails_when_steps_are_ignored();
    test_pps_deep_sweep_records_every_point_and_the_fold_back(); test_pps_deep_sweep_stops_at_the_first_lost_source(); test_pps_deep_sweep_is_opt_in();
    test_sweep_live_follows_the_row_in_flight(); test_sweep_live_is_idle_without_a_sweep_row();
    test_claim_sweep_record_lends_the_point_buffer();
    test_avs_deep_sweep_plans_and_runs_per_spr_avs_apdo(); test_avs_deep_sweep_epr_capped_at_the_monitor_ceiling();
    test_pps_step_walk_passes_a_noisy_step_and_fails_a_short_sum();
    test_pps_step_walk_waits_for_the_base_to_settle();
    test_walk_steady_window();
    test_pps_step_walk_steady_wait_is_bounded_by_the_timeout();
    test_walk_steady_mean(); test_pps_step_walk_records_the_mean_of_a_noisy_level(); test_a_capped_row_does_not_wait_the_full_timeout(); test_keep_alive_windows_do_not_restart_the_capped_plateau(); test_a_ledge_shorter_than_the_capped_plateau_still_passes_through_keep_alive(); test_a_long_unanswered_keep_alive_restarts_the_plateau(); test_pps_deep_sweep_records_the_mean_of_a_noisy_point();
    test_pps_step_walk_passes_a_drifting_source(); test_pps_step_walk_passes_a_slow_but_correct_source();
    test_ps_rdy_late_is_noted_and_never_judged(); test_ps_rdy_inferred_is_noted_and_never_judged();
    test_ps_rdy_out_of_band_is_noted_and_never_judged(); test_ps_rdy_timing_is_noted_per_walk_level();
    test_pps_step_walk_fails_a_flat_source_as_steps_ignored(); test_pps_step_walk_fails_a_source_that_rounds_to_100mv_on_the_jump();
    return test_report("test_charger_test");
}
