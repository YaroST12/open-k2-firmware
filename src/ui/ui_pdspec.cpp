// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check, resident half (see k2/ui/pd_spec_session.hpp). K2_PDSPEC=1 only.
#include "k2/ui/pd_spec_session.hpp"

#include <algorithm>

#include "k2/core/log.hpp"
#include "k2/core/loop_guard.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/pd/power_source.hpp"
#include "k2/pd/transcap.hpp"
#include "k2/ui/ui.hpp"

namespace k2 {

namespace sc = pdspec;

bool PdSpecSession::self_supplied() const { return fusb302::self_powered() || power_source::bus_powered(); }

// Another tool on the ports (a PD Stress run and a twin proof included: foreign_busy() leaves those to their own callers).
const char* PdSpecSession::foreign_busy() const {
    if (const char* b = ctx_.ui->foreign_busy()) return b;
    if (ctx_.ui->twin_live_busy()) return "Twin proof: stop it";
    if (ctx_.ui->pdstress_active()) return "PD Stress: stop it";
    return nullptr;
}

// ---- the Env seam -------------------------------------------------------------------------------------------------------------------

sc::Snapshot PdSpecSession::snapshot() {
    sc::Snapshot s;
    const auto& st = fusb302::sink_status();
    const auto& c = fusb302::caps();
    s.attached = c.valid && c.active_cc != 0;
    s.inline_link = ctx_.inline_mode;
    s.self_powered = self_supplied();
    s.state = st.state; s.result = st.result; s.pos = st.pos; s.mv = st.mv;
    s.epr_mode = st.epr_mode;
    s.req_us = st.req_us; s.accept_us = st.accept_us; s.ps_rdy_us = st.ps_rdy_us;
    s.accept_at = st.accept_at; s.ps_rdy_at = st.ps_rdy_at;
    s.hard_resets = st.hard_resets; s.source_hard_resets = st.source_hard_resets;
    s.rejects_rx = st.rejects_rx; s.waits_rx = st.waits_rx;
    s.epr_ka_at = st.epr_ka_at;
    const pd::PpsAnswer& pa = fusb302::pps_answer();
    s.pps_ask = pa.ask; s.pps_seq = pa.seq; s.pps_ask_us = pa.ask_us; s.pps_rx_us = pa.rx_us;
    s.pps_mv_raw = pa.mv_raw; s.pps_ma_raw = pa.ma_raw; s.pps_flags = pa.flags;
    s.caps_rx = fusb302::caps_count();
    // The monitor's latest sample (never the display filter or the 128-sample average): the test steps are judged on it.
    s.vbus_mv = ctx_.pm_online ? static_cast<std::int32_t>(ctx_.vbus_fast_mv) : -1;
    std::int32_t ibus = ctx_.pm_online ? static_cast<std::int32_t>(std::min<std::uint32_t>(power::magnitude(ctx_.pm_avg.current_ma), INT32_MAX)) : 0;
    // No USB host: the K2 runs on this VBUS and its own draw crosses the shunt; only what is above it is a load.
    if (!usb::configured()) ibus = std::max<std::int32_t>(0, ibus - pd::kSelfSupplyAllowanceMa);
    s.ibus_ma = ibus;
    s.now_us = systick::micros();
    return s;
}

const pd::SourceCaps& PdSpecSession::caps() { return fusb302::caps(); }

// The cable proof for the EPR levels: an e-marker read that says 5 A and above 20 V (the spoof does not count: it is our own answer).
bool PdSpecSession::epr_proof() {
    const auto& p = fusb302::physical_cable();
    return p.observed && p.max_mv > 20000 && p.max_ma >= 5000;
}

pd::Err PdSpecSession::request(const pd::Choice& c) { return fusb302::sink_request(c, true); }
void PdSpecSession::release() { fusb302::sink_release(); }
bool PdSpecSession::hard_reset() { return fusb302::sink_hard_reset(); }

void PdSpecSession::rd_off(bool off) {
    if (off == rd_away_) return;
    rd_away_ = off;
    if (off) fusb302::rd_glitch_begin();
    else fusb302::rd_glitch_end();
}

void PdSpecSession::keepalive_silence(sc::Silence s) { fusb302::sink_keepalive_silence(s); }

pd::Err PdSpecSession::ask_pps_status() {
    return fusb302::request_pps_status();   // no log line: the Black Box shows the message
}

// The capture: a fresh recording, triggered at once with the step's own post window, so it freezes on its own once the window is
// over. Refused (no capture: the engine falls back to the sink's stamps and live polling) while anyone else holds the recorder.
bool PdSpecSession::capture_start(std::uint32_t post_us) {
    capture_stop();
    if (transcap::active()) return false;
    if (transcap::arm() != transcap::ArmResult::Ok) return false;
    captured_ = true;
    if (!transcap::trigger(transcap::Reason::Manual, post_us)) { capture_stop(); return false; }
    return true;
}

bool PdSpecSession::capture_frozen() { return captured_ && transcap::frozen() != nullptr; }

sc::Edges PdSpecSession::capture_edges(std::uint32_t from_us) {
    const transcap::Rec* r = captured_ ? transcap::frozen() : nullptr;
    return r ? sc::find_edges(*r, from_us) : sc::Edges{};
}

sc::TransitionOut PdSpecSession::capture_transition(const sc::TransitionIn& in) {
    const transcap::Rec* r = captured_ ? transcap::frozen() : nullptr;
    return r ? sc::analyze_transition(*r, in) : sc::TransitionOut{};
}

sc::StepOut PdSpecSession::capture_step(const sc::StepIn& in) {
    const transcap::Rec* r = captured_ ? transcap::frozen() : nullptr;
    return r ? sc::analyze_step(*r, in) : sc::StepOut{};
}

void PdSpecSession::capture_stop() {
    if (!captured_) return;
    captured_ = false;
    transcap::disarm();
}

// ---- reporting hooks -----------------------------------------------------------------------------------------------------------------

void PdSpecSession::on_item(sc::Family f, std::uint8_t idx, std::uint8_t total) {
    K2LOG("[PDSPEC] item %s %u/%u\n", sc::family_name(f), unsigned(idx) + 1, unsigned(total));
}

void PdSpecSession::on_note(const char* why) { K2LOG("[PDSPEC] note %s\n", why); }

void PdSpecSession::on_finish(sc::Stop why) {
    const sc::Engine& e = *engine_;
    // The checks, the programmable passes and the caps note are in the blob only (`k2_remote pd-spec result`, report.txt): no log line
    // per check, for the flash budget of the K2_PDSPEC build.
    K2LOG("[PDSPEC] finish %s score=%u elapsed_ms=%u restore=%s\n", sc::stop_name(why), unsigned(e.score()),
          unsigned(e.elapsed_ms(0)), e.release_unconfirmed() ? "unconfirmed" : "5V");
    capture_stop();
    rd_off(false);   // the engine already put the Rd back; this only closes a gap the session still thinks open
    // Every end of a run comes through here, after the 5 V release. The run gives the line back the way the charger test does: the
    // sleeping-port wake may act again, and the run's Rd goes off (sink_release keeps the 5 V contract for its linger first).
    fusb302::set_run_owner(false);
    fusb302::rd_for_run(false);
}

// ---- start, cancel, tick ---------------------------------------------------------------------------------------------------------------

sc::StartResult PdSpecSession::start(std::uint8_t families, std::uint32_t now_ms, std::uint8_t opts) {
    const sc::StartResult r = start_run(families, now_ms, opts);
    last_start_ = static_cast<std::uint8_t>(r);
    return r;
}

sc::StartResult PdSpecSession::start_run(std::uint8_t families, std::uint32_t now_ms, std::uint8_t opts) {
    if (running()) return sc::StartResult::Running;
    if (!ctx_.pm_online) return sc::StartResult::NoMonitor;
    // Refuse before the previous result is replaced: the engine's own checks come after the emplace.
    if (const char* b = foreign_busy()) {
        K2LOG("[PDSPEC] start refused: %s\n", b);
        return sc::StartResult::Busy;
    }
    const sc::Snapshot s = snapshot();
    if (s.inline_link) return sc::StartResult::Inline;
    if (s.self_powered) return sc::StartResult::SelfPowered;
    if (!s.attached || !fusb302::caps().valid) return sc::StartResult::NoCaps;
    engine_.emplace(static_cast<sc::Env&>(*this));
    run_no_ = static_cast<std::uint16_t>(run_no_ + 1 ? run_no_ + 1 : 1);
    cancel_asked_ = false;
    captured_ = false;
    rd_away_ = false;
    const sc::StartResult r = engine_->start(now_ms, families, opts);
    if (r != sc::StartResult::Ok) {
        K2LOG("[PDSPEC] start refused: %s\n", sc::start_name(r));
        engine_.reset();
    } else {
        const sc::Levels& L = engine_->levels();
        // (The move count is the host's to work out from the caps: sc::family_moves; not computed here, for the flash budget.)
        K2LOG("[PDSPEC] start families=0x%x fixed=%u pps=%u prog=%u full20=%u cap_mv=%u\n", unsigned(engine_->families()),
              unsigned(L.n_fixed), unsigned(L.n_pps), unsigned(L.n_prog), unsigned(opts & sc::kOptFull20), unsigned(ctx_.cap_mv));
        // The run owns the link from here: a pd-reread attach it starts on is the run's, and the re-read's release must not fire
        // inside a step.
        fusb302::reread_take_over();
        // The run presents its own Rd for its whole length, as a charger-test START does; on_finish() gives it back. A run
        // started on a contract (a lingering 5 V one, `pd attach` on a list already there) had Rd only through the sink's
        // demand, which a Hard Reset ends with the contract: the Rd went 300 ms later and the source, back from its reset,
        // saw no sink (Anker IQ4, hardware 2026-11-14). With it the source's caps are acked and PDO 1 requested (the START
        // path), the sink's resume asks for the contract back.
        fusb302::rd_for_run(true);
    }
    update_footer();
    return r;
}

void PdSpecSession::cancel() {
    if (!running()) return;
    cancel_asked_ = true;
    engine_->cancel();
    update_footer();
}

void PdSpecSession::poll(std::uint32_t now_ms) {
    if (running()) {
        if (!ctx_.pm_online || loopguard::locked()) cancel();   // no VBUS to judge by / the HID-cable loop 5 V lock
        fusb302::set_run_owner(true);   // after ChargerTestSession::poll, which clears it when no test runs
        engine_->poll(now_ms);
    }
    update_footer();
}

// ---- result --------------------------------------------------------------------------------------------------------------------------

std::uint16_t PdSpecSession::blob_len() const {
    return engine_ ? static_cast<std::uint16_t>(sc::blob_len(engine_->trans().size(), engine_->passes().size())) : 0;
}

PdSpecSession::Status PdSpecSession::status(std::uint32_t now_ms) const {
    Status s;
    s.start = last_start_;
    if (!engine_) return s;
    const sc::Engine& e = *engine_;
    s.state = e.running() ? State::Running : State::Finished;
    s.stop = static_cast<std::uint8_t>(e.stop());
    s.family = static_cast<std::uint8_t>(e.family());
    s.item = e.item();
    s.items = e.items();
    s.families = e.families();
    for (std::size_t k = 0; k < sc::kChecks; ++k) {
        switch (sc::verdict(static_cast<sc::Check>(k), e.agg()[k])) {
            case sc::Verdict::Pass: ++s.passes; break;
            case sc::Verdict::Fail: ++s.fails; break;
            case sc::Verdict::Skip: ++s.skips; break;
            case sc::Verdict::Pending:
            case sc::Verdict::Unverified: break;   // in none of the three: the remote reads the blob for those
        }
    }
    s.flags = e.release_unconfirmed() ? 1 : 0;
    s.score = e.score();
    s.elapsed_s = static_cast<std::uint16_t>(std::min<std::uint32_t>(e.elapsed_ms(now_ms) / 1000, 0xFFFF));
    s.blob_len = blob_len();
    return s;
}

std::size_t PdSpecSession::result_read(std::size_t off, std::span<std::uint8_t> out) const {
    if (!engine_) return 0;
    const sc::Engine& e = *engine_;
    sc::BlobHead h;
    h.state = static_cast<std::uint8_t>(e.running() ? State::Running : State::Finished);
    h.stop = static_cast<std::uint8_t>(e.stop());
    h.n_trans = static_cast<std::uint8_t>(e.trans().size());
    h.n_pass = static_cast<std::uint8_t>(e.passes().size());
    h.caps_note = e.caps_note();
    h.opts = e.opts();
    h.score = e.score();
    h.elapsed_s = static_cast<std::uint16_t>(std::min<std::uint32_t>(e.elapsed_ms(systick::millis()) / 1000, 0xFFFF));
    h.flags = e.release_unconfirmed() ? 1 : 0;
    const sc::BlobReader r(h, e.agg(), e.timers(), e.trans(), e.passes());
    return r.read(off, out);
}

// ---- footer --------------------------------------------------------------------------------------------------------------------------

void PdSpecSession::update_footer() {
    std::uint8_t kind = 0, fam = 0, item = 0, items = 0;
    if (running()) {
        if (cancel_asked_) kind = 2;
        else {
            kind = 1;
            fam = static_cast<std::uint8_t>(engine_->family());
            item = engine_->item();
            items = engine_->items();
        }
    }
    if (kind == foot_kind_ && fam == foot_fam_ && item == foot_item_ && items == foot_items_) return;
    foot_kind_ = kind; foot_fam_ = fam; foot_item_ = item; foot_items_ = items;
    foot_.clear();
    switch (kind) {
        case 1:
            foot_.put("PD Spec: ").put(sc::family_name(static_cast<sc::Family>(fam)));
            if (items > 1) foot_.put(" ").put_u32(std::min<unsigned>(item + 1u, items)).put("/").put_u32(items);
            break;
        case 2: foot_.put(kPdSpecFooters[2]); break;
        default: break;
    }
}

}  // namespace k2
