// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress, resident half (see k2/ui/pd_stress_session.hpp).
#include "k2/ui/pd_stress_session.hpp"

#include <algorithm>

#include "k2/core/log.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/pd/power_source.hpp"
#include "k2/pdstress/journal_hw.hpp"
#include "k2/ui/ui.hpp"

namespace k2 {

namespace ps = pdstress;

bool PdStressSession::self_supplied() const { return fusb302::self_powered() || power_source::bus_powered(); }

// ---- the Env seam -------------------------------------------------------------------------------------------------------------------

ps::Snapshot PdStressSession::snapshot() {
    ps::Snapshot s;
    const auto& st = fusb302::sink_status();
    const auto& c = fusb302::caps();
    s.attached = c.valid && c.active_cc != 0;
    s.inline_link = ctx_.inline_mode;
    s.self_powered = self_supplied();
    s.state = st.state; s.result = st.result; s.pos = st.pos; s.mv = st.mv; s.ma = st.ma;
    s.epr_mode = st.epr_mode; s.verifying = st.verifying;
    s.req_us = st.req_us; s.accept_us = st.accept_us; s.ps_rdy_us = st.ps_rdy_us;
    s.accept_at = st.accept_at; s.ps_rdy_at = st.ps_rdy_at; s.ps_rdy_inferred = st.ps_rdy_inferred;
    s.waits_rx = st.waits_rx; s.rejects_rx = st.rejects_rx; s.soft_resets_rx = st.soft_resets_rx; s.caps_unsolicited = st.caps_unsolicited;
    s.hard_resets = st.hard_resets; s.source_hard_resets = st.source_hard_resets;
    s.caps_rx = fusb302::caps_count();
    s.vbus_mv = ctx_.pm_online ? static_cast<std::int32_t>(ctx_.vbus_fast_mv) : -1;
    std::int32_t ibus = ctx_.pm_online ? static_cast<std::int32_t>(std::min<std::uint32_t>(power::magnitude(ctx_.pm_avg.current_ma), INT32_MAX)) : 0;
    // No USB host: the K2 runs on this VBUS and its own draw crosses the shunt; only what is above it is a load.
    if (!usb::configured()) ibus = std::max<std::int32_t>(0, ibus - pd::kSelfSupplyAllowanceMa);
    s.ibus_ma = ibus;
    s.now_us = systick::micros();
    return s;
}

const pd::SourceCaps& PdStressSession::caps() { return fusb302::caps(); }

// The cable proof for the EPR rows: an e-marker read that says 5 A and above 20 V (the spoof does not count: it is an answer we give).
bool PdStressSession::epr_proof() {
    const auto& p = fusb302::physical_cable();
    return p.observed && p.max_mv > 20000 && p.max_ma >= 5000;
}

// Only another tool counts here: the holds, the contract and the sink state this run causes itself are not reasons to refuse.
const char* PdStressSession::busy() { return ctx_.ui->foreign_busy(); }

pd::Err PdStressSession::request(const pd::Choice& c) { return fusb302::sink_request(c, true); }
bool PdStressSession::get_caps() { return fusb302::refresh_source_caps() == pd::Err::Ok; }
bool PdStressSession::soft_reset() { return fusb302::sink_soft_reset(); }
void PdStressSession::release() { fusb302::sink_release(); }

// ---- the journal --------------------------------------------------------------------------------------------------------------------

bool PdStressSession::save_journal(std::uint32_t now_ms) {
    img_.opt = engine_->options();
    img_.resume = engine_->resume();
    img_.resume.next = engine_->seq_index();
    img_.resume.elapsed_ms = engine_->elapsed_ms(now_ms);
    img_.next_seq = static_cast<std::uint8_t>(engine_->seq());
    img_.caps_crc = ps::journal::caps_crc(fusb302::caps());
    img_.finished = false;
    img_.stop = ps::Stop::None;
    ps::journal::capture_counts(img_, engine_->record());
    ps::journal::Journal j(ps::journal_hw::store());
    return j.save(img_);
}

// A sequence is about to start: write the record first, on a quiet link (the engine waits for this at most kBoundaryMs).
bool PdStressSession::boundary_ok(std::uint32_t now_ms) {
    if (!journal_on_) return true;
    if (fusb302::pd_quiet_ms() < kQuietMs) return false;
    if (!save_journal(now_ms)) {
        journal_on_ = false;   // no unattended resume without a journal: the run goes on, a reboot ends it
        K2LOG("[STRESS] journal off: the record could not be written\n");
    }
    return true;
}

// ---- reporting hooks -----------------------------------------------------------------------------------------------------------------

void PdStressSession::on_seq_start(ps::Seq s, std::uint8_t idx, std::uint8_t total, std::uint16_t steps) {
    K2LOG("[STRESS] seq %u/%u %s start steps=%u\n", unsigned(idx), unsigned(total), ps::kSeqs[static_cast<std::size_t>(s)].name,
          unsigned(steps));
}
void PdStressSession::on_seq_end(ps::Seq s, const ps::Counts& c, bool skipped) {
    K2LOG("[STRESS] seq %s end trans=%u points=%u score_cp=%u skipped=%u\n", ps::kSeqs[static_cast<std::size_t>(s)].name,
          unsigned(c.transitions), unsigned(ps::penalty_points(c)), unsigned(ps::score_cp(c)), unsigned(skipped));
}
void PdStressSession::on_event(ps::Seq s, ps::Event e, std::uint32_t detail) {
    K2LOG("[STRESS] event %s seq=%s detail=%u\n", ps::event_name(e), ps::kSeqs[static_cast<std::size_t>(s)].name, unsigned(detail));
}
void PdStressSession::on_note(const char* why) { K2LOG("[STRESS] note %s\n", why); }
void PdStressSession::on_finish(ps::Stop why) {
    frozen_ = true;
    frozen_ms_ = engine_->elapsed_ms(last_now_);
    const ps::Counts t = engine_->total();
    K2LOG("[STRESS] finish %s trans=%u points=%u score_cp=%u elapsed_ms=%u reboots=%u\n", ps::stop_name(why), unsigned(t.transitions),
          unsigned(ps::penalty_points(t)), unsigned(ps::score_cp(t)), unsigned(frozen_ms_), unsigned(engine_->reboots()));
    ps::journal::Journal j(ps::journal_hw::store());
    j.clear();   // a finished run never resumes
    journal_on_ = false;
}

// ---- start, cancel, restore ----------------------------------------------------------------------------------------------------------

ps::StartResult PdStressSession::start(const ps::Options& o, std::uint32_t now_ms) {
    const ps::StartResult r = start_run(o, now_ms);
    last_start_ = static_cast<std::uint8_t>(r);
    return r;
}

ps::StartResult PdStressSession::start_run(const ps::Options& o, std::uint32_t now_ms) {
    if (running()) return ps::StartResult::Running;
    if (restoring()) return ps::StartResult::Busy;
    if (!ctx_.pm_online) return ps::StartResult::NoMonitor;
    // Refuse before the previous result is replaced: the engine's own checks come after the emplace.
    if (busy()) return ps::StartResult::Busy;
    const ps::Snapshot s = snapshot();
    if (s.inline_link) return ps::StartResult::Inline;
    if (!s.attached || !fusb302::caps().valid) return ps::StartResult::NoCaps;
    const auto rec = ct_.claim_record();
    if (rec.empty()) return ps::StartResult::NoStore;
    last_now_ = now_ms;
    engine_.emplace(static_cast<ps::Env&>(*this), ps::Record(rec));
    journal_on_ = self_supplied();
    cancel_asked_ = false;
    frozen_ = false;
    K2LOG("[STRESS] start mask=0x%x cycles_pct=%u pace=%u max_s=%u max_trans=%u journal=%u\n", unsigned(o.mask), unsigned(o.cycles_pct),
          unsigned(o.pace), unsigned(o.max_ms / 1000), unsigned(o.max_transitions), unsigned(journal_on_));
    const ps::StartResult r = engine_->start(o, now_ms);
    if (r != ps::StartResult::Ok) {
        K2LOG("[STRESS] start refused: %s\n", ps::start_name(r));
        engine_.reset();
        journal_on_ = false;
    }
    // The run owns the link from here: a pd-reread attach it starts on (Rd, PDO 1 contract) is the run's, and the re-read's
    // 30 s release must not fire inside a Soft_Reset step (hardware 2026-11-11). A refused start takes nothing over.
    if (r == ps::StartResult::Ok) fusb302::reread_take_over();
    update_footer();
    return r;
}

void PdStressSession::cancel() {
    if (restoring()) { drop_restore("cancelled"); return; }
    if (!running()) return;
    cancel_asked_ = true;
    engine_->cancel();
    update_footer();
}

bool PdStressSession::boot_restore(bool cancel, std::uint32_t now_ms) {
    ps::journal::Journal j(ps::journal_hw::store());
    if (!j.load(img_)) return false;
    if (cancel || img_.finished) {
        K2LOG("[STRESS] journal dropped (%s)\n", cancel ? "BACK held at power-up" : "run was finished");
        j.clear();
        return false;
    }
    restore_ = Restore::Waiting;
    restore_t0_ = now_ms;
    gate_.reset();
    K2LOG("[STRESS] journal found: next=%u losses=%u, waiting for a steady VBUS\n", unsigned(img_.resume.next), unsigned(img_.resume.losses));
    update_footer();
    return true;
}

void PdStressSession::drop_restore(const char* why) {
    K2LOG("[STRESS] resume dropped: %s\n", why);
    ps::journal::Journal j(ps::journal_hw::store());
    j.clear();
    restore_ = Restore::None;
    update_footer();
}

void PdStressSession::restore_step(std::uint32_t now_ms) {
    if (static_cast<std::int32_t>(now_ms - restore_t0_) > static_cast<std::int32_t>(kRestoreWaitMs)) { drop_restore("the charger did not come back"); return; }
    gate_.update(now_ms, ctx_.pm_online ? static_cast<int>(ctx_.vbus_fast_mv) : -1);
    const auto& c = fusb302::caps();
    if (!power_source::decided() || !gate_.ready(now_ms) || !c.valid || c.active_cc == 0 || ctx_.inline_mode) return;
    if (busy()) return;   // another tool has the ports; the wait above bounds this
    const auto v = ps::journal::derive(img_, ps::journal::caps_crc(c), false);
    if (v == ps::journal::Verdict::Stale) { drop_restore("another charger or list"); return; }
    const auto rec = ct_.claim_record();
    if (rec.empty()) { drop_restore("no record buffer"); return; }
    ps::Record r(rec);
    ps::journal::apply_counts(img_, r);
    restore_ = Restore::None;
    last_now_ = now_ms;
    engine_.emplace(static_cast<ps::Env&>(*this), ps::Record(rec));
    journal_on_ = self_supplied();
    cancel_asked_ = false;
    frozen_ = false;
    K2LOG("[STRESS] resume at sequence %u after reboot %u%s\n", unsigned(img_.resume.next), unsigned(img_.resume.losses),
          v == ps::journal::Verdict::RunLost ? " (budget spent)" : "");
    const ps::StartResult sr = engine_->start(img_.opt, now_ms, &img_.resume);
    if (sr != ps::StartResult::Ok) {
        K2LOG("[STRESS] resume refused: %s\n", ps::start_name(sr));
        engine_.reset();
        journal_on_ = false;
        ps::journal::Journal j(ps::journal_hw::store());
        j.clear();
    }
    update_footer();
}

// ---- tick ---------------------------------------------------------------------------------------------------------------------------

void PdStressSession::yield_record() {
    // The charger test reuses the buffer the record sits in: a finished result is gone once one runs.
    if (engine_ && !engine_->running() && ct_.busy()) engine_.reset();
}

void PdStressSession::poll(std::uint32_t now_ms) {
    last_now_ = now_ms;
    yield_record();
    if (restoring()) {
        fusb302::set_run_owner(true);   // the idle wake must not act on the dark gap of a charger coming back
        restore_step(now_ms);
    }
    if (running()) {
        fusb302::set_run_owner(true);   // after ChargerTestSession::poll, which clears it when no test runs
        engine_->poll(now_ms);
    }
    update_footer();
}

const char* PdStressSession::busy_reason() const {
    if (running()) return "PD Stress: stop it";
    if (restoring()) return "PD Stress: resuming";
    return nullptr;
}

std::uint32_t PdStressSession::elapsed_ms(std::uint32_t now_ms) const {
    if (!engine_) return 0;
    return frozen_ ? frozen_ms_ : engine_->elapsed_ms(now_ms);
}

// ---- result --------------------------------------------------------------------------------------------------------------------------

PdStressSession::Status PdStressSession::status(std::uint32_t now_ms) const {
    Status s;
    s.start = last_start_;
    if (!engine_) {
        s.state = restoring() ? State::Restoring : State::Idle;
        return s;
    }
    const ps::Engine& e = *engine_;
    s.state = restoring() ? State::Restoring : e.running() ? State::Running : State::Finished;
    s.stop = static_cast<std::uint8_t>(e.stop());
    s.seq_idx = e.seq_index();
    s.seq_total = e.seq_total();
    s.seq_cur = static_cast<std::uint8_t>(e.seq());
    s.reboots = e.reboots();
    s.transitions = static_cast<std::uint16_t>(std::min<std::uint32_t>(e.transitions(), 0xFFFF));
    s.elapsed_ms = elapsed_ms(now_ms);
    s.score_cp = ps::score_cp(e.total());
    s.blob_len = static_cast<std::uint16_t>(ps::result::kTotalBytes);
    return s;
}

ps::result::Info PdStressSession::info(std::uint32_t now_ms) const {
    ps::result::Info i;
    const ps::Engine& e = *engine_;
    i.opt = e.options();
    i.stop = e.stop();
    i.running = e.running();
    i.finished = e.finished();
    i.release_unconfirmed = e.release_unconfirmed();
    i.n_order = e.seq_total();
    i.reboots = e.reboots();
    i.transitions = static_cast<std::uint16_t>(std::min<std::uint32_t>(e.transitions(), 0xFFFF));
    i.elapsed_ms = elapsed_ms(now_ms);
    i.stamp_suspect = e.stamp_suspect();
    for (std::uint8_t k = 0; k < e.seq_total() && k < ps::kMaxSeqs; ++k) i.order[k] = static_cast<std::uint8_t>(e.order(k));
    for (std::size_t q = 0; q < ps::kSeqCount; ++q) {
        const ps::Seq s = static_cast<ps::Seq>(q);
        i.seq_flags[q] = static_cast<std::uint8_t>((e.seq_done(s) ? ps::result::kSeqDone : 0) | (e.seq_skipped(s) ? ps::result::kSeqSkipped : 0) |
                                                   (e.given_up(s) ? ps::result::kSeqGivenUp : 0) | (static_cast<unsigned>(e.na(s)) << 4));
    }
    return i;
}

std::size_t PdStressSession::result_read(std::size_t off, std::span<std::uint8_t> out) const {
    if (!engine_) return 0;
    return ps::result::read(info(last_now_), engine_->record(), off, out);
}

// ---- footer --------------------------------------------------------------------------------------------------------------------------

void PdStressSession::update_footer() {
    std::uint8_t kind = 0, a = 0, b = 0;
    if (restoring()) kind = 2;
    else if (running()) {
        if (cancel_asked_) kind = 3;
        else { kind = 1; a = static_cast<std::uint8_t>(engine_->seq_index() + 1); b = engine_->seq_total(); }
    }
    if (kind == footer_kind_ && a == footer_seq_ && b == footer_total_) return;
    footer_kind_ = kind; footer_seq_ = a; footer_total_ = b;
    foot_.clear();
    switch (kind) {
        case 1: foot_.put("PD Stress: seq ").put_u32(a).put(" of ").put_u32(b); break;
        case 2: foot_.put(kPdStressFooters[1]); break;
        case 3: foot_.put(kPdStressFooters[2]); break;
        default: break;
    }
}

}  // namespace k2
