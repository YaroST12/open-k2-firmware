// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check, resident half: binds the pure engine (k2/pdspec/engine.hpp) to the K2. It owns the engine (and with it the result),
// never draws (the Ui reads footer() for the one line a run puts on whatever page is up) and lives in Ui for as long as the K2 runs,
// so a run goes on while any page is shown and its result stays readable afterwards. Built with K2_PDSPEC=1 only.
//
// The seam: Env::request is fusb302::sink_request (the sink's own guards stay in force: the TX-OK gate, the monitor ceiling, one
// Request at a time), the snapshot is fusb302::sink_status() plus the monitor's latest VBUS (vbus_fast_mv), the capture is the
// transition recorder (k2/pd/transcap.hpp: armed and triggered with the step's own post window, analysed in place with the
// k2/pdspec/analyze.hpp templates once frozen), the Hard Reset is fusb302::sink_hard_reset, the detach gap rd_glitch_begin/end and
// the keep-alive silence fusb302::sink_keepalive_silence (bounded by the sink itself).
//
// Reboot safety: a K2 powered by the source under test (fed through the A port, or a boot-pinned Rd) is refused at start, so the
// run never cuts its own supply. A K2 that reboots anyway (a crash, the reboot chord) comes up passive: a plain boot presents no Rd,
// the source sees a detach and drops VBUS to vSafe0V; nothing of the run resumes (no journal), the result is lost.
//
// The Tools-page entry is PdSpecPage (Menu::PdSpec): it picks the families, starts after a two-press confirm, reads engine() while a
// run goes on and the result afterwards. The remote (Cmd::PdSpec) uses start() / cancel() / status() / result_read().
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "k2/pdspec/engine.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

// The footer a run puts on the page that is up, 8 px a character in 220 px: every form must fit (the widest family, two-digit counts).
inline constexpr const char* kPdSpecFooters[] = {
    "PD Spec: Transitions 56/56",
    "PD Spec: PPS/AVS 16/16",
    "PD Spec: stopping",
};
static_assert(all_fit(kPdSpecFooters, 8, 220), "a PD Spec Check footer is wider than the footer band");

class PdSpecSession final : private pdspec::Env {
public:
    explicit PdSpecSession(UiContext& c) : ctx_(c) {}

    // Starts a run of the families in `families` (a bit per pdspec::Family, 0 = all) with the options in `opts` (pdspec::kOptFull20);
    // the caller has done any confirming. Ok = running; anything else = refused, nothing changed (a previous result stays).
    pdspec::StartResult start(std::uint8_t families, std::uint32_t now_ms, std::uint8_t opts = 0);
    // The charger test's Deep section: the families of specsec::kFamilyMask, no options, started INSIDE a test run: the test owns the
    // ports (no foreign-busy check), the run's Rd and the run-owner flag, so neither is taken or given back here and no footer is shown.
    // The engine stays readable through engine_for_test() until another run replaces it.
    pdspec::StartResult start_in_test(std::uint32_t now_ms);
    const pdspec::Engine* engine_for_test() const { return test_run_ && engine_ ? &*engine_ : nullptr; }
    // The run ends at the next poll: the keep-alives come back, the Rd is on and the source is released to 5 V first. No-op when
    // nothing runs.
    void cancel();
    [[gnu::noinline]] void poll(std::uint32_t now_ms);

    bool running() const { return engine_ && engine_->running(); }
    bool active() const { return running(); }
    bool has_result() const { return engine_.has_value(); }
    const pdspec::Engine* engine() const { return engine_ ? &*engine_ : nullptr; }
    // A new number (never 0) each time a run starts: the page keeps the one it has shown.
    std::uint16_t run_no() const { return run_no_; }
    // The result blob (pdspec/result.hpp) from byte `off`; 0 when there is no result or `off` is past the end.
    std::size_t result_read(std::size_t off, std::span<std::uint8_t> out) const;
    std::uint16_t blob_len() const;
    std::uint32_t elapsed_ms(std::uint32_t now_ms) const { return engine_ ? engine_->elapsed_ms(now_ms) : 0; }

    // The status the remote op serves.
    enum class State : std::uint8_t { Idle, Running, Finished };
    struct Status {
        State state = State::Idle;
        std::uint8_t stop = 0;                 // pdspec::Stop
        std::uint8_t start = 0;                // pdspec::StartResult of the last start() call
        std::uint8_t family = 0, item = 0, items = 0, families = 0;
        std::uint8_t fails = 0, passes = 0, skips = 0;
        std::uint8_t flags = 0;                // bit0: the end-of-run 5 V restore was not confirmed
        std::uint16_t score = 0, elapsed_s = 0, blob_len = 0;
    };
    Status status(std::uint32_t now_ms) const;

    // The line for the footer of the page that is up; nullptr when no run goes on. Info tone.
    const char* footer() const { return foot_.c_str()[0] ? foot_.c_str() : nullptr; }
    const char* busy_reason() const { return running() ? "PD Spec: stop it" : nullptr; }
    // Another tool on the ports, which start() refuses (nullptr = free). The holds and the contract a run makes itself do not count.
    const char* foreign_busy() const;

private:
    // pdspec::Env
    pdspec::Snapshot snapshot() override;
    const pd::SourceCaps& caps() override;
    std::uint32_t cap_mv() override { return ctx_.cap_mv; }
    bool epr_proof() override;
    pd::Err request(const pd::Choice& c) override;
    void release() override;
    bool hard_reset() override;
    void rd_off(bool off) override;
    void keepalive_silence(pdspec::Silence s) override;
    pd::Err ask_pps_status() override;
    bool capture_start(std::uint32_t post_us) override;
    bool capture_frozen() override;
    pdspec::Edges capture_edges(std::uint32_t from_us) override;
    pdspec::TransitionOut capture_transition(const pdspec::TransitionIn& in) override;
    pdspec::StepOut capture_step(const pdspec::StepIn& in) override;
    void capture_stop() override;
    void on_item(pdspec::Family f, std::uint8_t idx, std::uint8_t total) override;
    void on_note(const char* why) override;
    void on_finish(pdspec::Stop why) override;

    pdspec::StartResult start_run(std::uint8_t families, std::uint32_t now_ms, std::uint8_t opts, bool in_test);
    bool self_supplied() const;
    void update_footer();

    UiContext& ctx_;
    std::optional<pdspec::Engine> engine_;
    bool captured_ = false;              // the capture is ours (armed by capture_start, not yet stopped)
    bool rd_away_ = false;               // the detach gap is open (rd_off(true) without the matching rd_off(false))
    bool cancel_asked_ = false;
    bool test_run_ = false;              // the engine's run is the charger test's (start_in_test)
    std::uint8_t last_start_ = 0;        // pdspec::StartResult of the last start() call
    std::uint16_t run_no_ = 0;
    std::uint8_t foot_fam_ = 0xFF, foot_item_ = 0xFF, foot_items_ = 0xFF, foot_kind_ = 0xFF;
    StrBuf<28> foot_;
};

}  // namespace k2
