// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress, resident half: binds the pure engine (k2/pdstress/engine.hpp) to the K2. It owns the engine, the journal image and the
// reboot-resume decision; it never draws (the Ui reads footer() for the one line a run puts on whatever page is up). It lives in Ui for
// as long as the K2 runs, like ChargerTestSession, so a run goes on while any page is shown and its result stays readable afterwards.
//
// The seam: Env::request is fusb302::sink_request (the sink's own guards stay in force), the snapshot is fusb302::sink_status() plus the
// monitor's latest VBUS, and the record buffer is lent by the charger test's deep-sweep point buffer (ChargerTestSession::claim_record),
// so the tool costs no second 1.3 KB of .bss. A charger test that starts afterwards takes the buffer back: a finished result is dropped
// then (has_result() turns false).
//
// A K2 powered by the charger under test (a pinned Rd, or fed through the A port) writes its journal at every sequence boundary on a quiet
// link and, after a reboot, resumes by itself once VBUS has been steady (pdstress/journal.hpp). It presents no Rd of its own: a resume
// needs the Rd the boot already has, and a host-powered K2 never journals (a charger cut does not reboot it).
//
// Adding a Tools-page entry later needs: a page class (menu Menu::PdStress) with a RowList of the sequences (mask), the cycle scale and
// the pace, a START ActionCell that calls start() with the confirm arming of the other tools, a Running view reading status() and
// result_read(), and a row in the Tools list. None of it touches this class.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "k2/pdstress/engine.hpp"
#include "k2/pdstress/journal.hpp"
#include "k2/pdstress/result.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/chargertest/charger_journal.hpp"

namespace k2 {

// The footer a run puts on the page that is up, 8 px a character in 220 px: every form must fit (worst case with two-digit counts).
inline constexpr const char* kPdStressFooters[] = {
    "PD Stress: seq 12 of 12",
    "PD Stress: resuming",
    "PD Stress: stopping",
};
static_assert(all_fit(kPdStressFooters, 8, 220), "a PD Stress footer is wider than the footer band");

class PdStressSession final : private pdstress::Env {
public:
    // A boot that found a journal waits this long for the charger to come back before it gives the run up.
    static constexpr std::uint32_t kRestoreWaitMs = 60000;
    // The journal is written only when the PD link has been quiet this long (a flash write holds the main loop up to ~250 ms).
    static constexpr std::uint32_t kQuietMs = 300;

    PdStressSession(UiContext& c, ChargerTestSession& ct) : ctx_(c), ct_(ct) {}

    // Starts a run (the caller has done any confirming). Ok = running. NoMonitor / NoStore / Busy ... = refused, nothing changed.
    pdstress::StartResult start(const pdstress::Options& o, std::uint32_t now_ms);
    // The run ends at the next poll: the source is released to 5 V first, like the charger test's cancel. No-op when nothing runs.
    void cancel();
    void poll(std::uint32_t now_ms);

    // Called once from Ui::init: a journal of a run that never finished is loaded and waits for VBUS. `cancel` = BACK held at power-up:
    // the journal is dropped. True = a restore is pending.
    bool boot_restore(bool cancel, std::uint32_t now_ms);

    bool running() const { return engine_ && engine_->running(); }
    bool restoring() const { return restore_ != Restore::None; }
    bool active() const { return running() || restoring(); }
    bool has_result() const { return engine_.has_value(); }
    const pdstress::Engine* engine() const { return engine_ ? &*engine_ : nullptr; }
    // The result blob (pdstress/result.hpp) from byte `off`; 0 when there is no result or `off` is past the end.
    std::size_t result_read(std::size_t off, std::span<std::uint8_t> out) const;
    // Elapsed run time: frozen when the run ended.
    std::uint32_t elapsed_ms(std::uint32_t now_ms) const;

    // The line for the footer of the page that is up; nullptr when there is none (no run, restore not pending). Info tone.
    const char* footer() const { return foot_.c_str()[0] ? foot_.c_str() : nullptr; }
    // Why a charger test / identity / trigger must not start now (nullptr = free); the Ui folds it into busy_reason().
    const char* busy_reason() const;
    // A charger test took the point buffer: a finished result is dropped. The Ui calls this each tick.
    void yield_record();

private:
    enum class Restore : std::uint8_t { None, Waiting };

    // pdstress::Env
    pdstress::Snapshot snapshot() override;
    const pd::SourceCaps& caps() override;
    std::uint32_t cap_mv() override { return ctx_.cap_mv; }
    bool epr_proof() override;
    const char* busy() override;
    pd::Err request(const pd::Choice& c) override;
    bool get_caps() override;
    bool soft_reset() override;
    void release() override;
    bool boundary_ok(std::uint32_t now_ms) override;
    void on_seq_start(pdstress::Seq s, std::uint8_t idx, std::uint8_t total, std::uint16_t steps) override;
    void on_seq_end(pdstress::Seq s, const pdstress::Counts& c, bool skipped) override;
    void on_event(pdstress::Seq s, pdstress::Event e, std::uint32_t detail) override;
    void on_note(const char* why) override;
    void on_finish(pdstress::Stop why) override;

    void restore_step(std::uint32_t now_ms);
    void drop_restore(const char* why);
    bool self_supplied() const;
    bool journaling() const { return journal_on_; }
    bool save_journal(std::uint32_t now_ms);
    pdstress::result::Info info(std::uint32_t now_ms) const;
    void update_footer();

    UiContext& ctx_;
    ChargerTestSession& ct_;
    std::optional<pdstress::Engine> engine_;
    pdstress::journal::Image img_{};     // the saved record: written at boundaries, read back by a restore
    charger_journal::ResumeGate gate_;
    Restore restore_ = Restore::None;
    std::uint32_t restore_t0_ = 0;
    bool journal_on_ = false;            // this run writes a journal (self-supplied K2, the store worked)
    bool cancel_asked_ = false;
    bool frozen_ = false;                // the run ended: elapsed_ms is fixed
    std::uint32_t frozen_ms_ = 0;
    std::uint32_t last_now_ = 0;
    std::uint8_t footer_seq_ = 0xFF, footer_total_ = 0xFF, footer_kind_ = 0xFF;
    StrBuf<28> foot_;
};

}  // namespace k2
