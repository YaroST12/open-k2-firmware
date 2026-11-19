// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check engine: a pure state machine that runs the no-load conformance checks against a live PD source and judges each
// measurement against its spec limit (result.hpp). It never touches hardware: the Env seam is the only way out (the firmware binds it
// to fusb302::sink_request, the transition capture and friends, so the sink's own guards - TX-OK gate, monitor ceiling, one Request at
// a time - always apply). poll(now_ms) runs from the main loop. Host-tested in tests/test_pdspec_engine.cpp with a fake Env.
//
// Families, in run order, each one ending back at a 5 V contract (the shared restore):
//   Transitions  every ordered pair of fixed levels (plan.hpp walk): Accept / PS_RDY timing, VBUS start, PS_RDY in band, rise and
//                fall time, overshoot, settled accuracy. Down-steps carry the PS_RDY-early and fall-time checks (no load: the
//                source's own discharge).
//   Pps          the programmable sweep (PPS APDOs, then the SPR AVS APDO; plan.hpp Sweep): per window steps of 20 / 100 / 500 mV
//                (AVS 100 / 500 / 1000 mV) up to the maximum and back, then the two slams. Monotonic, accuracy, in band and
//                PS_RDY within tPpsSrcTrans* / tAvsSrcTrans*, overshoot (vPpsValid / vAvsValid), transient; Get_PPS_Status at the
//                top and the end of every PPS pass. A window the Deep charger test swept itself runs a short cover instead
//                (plan.hpp make_cover_sweep: three large steps up and back, the resolution steps onto both ends), then the slams.
//   PpsKeepAlive a PPS contract with the sink's keep-alive silenced: when does the source Hard Reset.
//   EprKeepAlive the same in EPR mode (needs EPR and the cable proof).
//   HardReset    the K2 Hard Resets the top fixed level: VBUS to vSafe0V, off time, turn-on, first capabilities.
//   Detach       the K2 takes its Rd away at the top fixed level: VBUS to vSafe0V (the bleeder), then Rd back and a 5 V contract.
//
// Safety: only advertised levels at or below the monitor ceiling, through the sink's guards; a measured VBUS above the ceiling + 5 %,
// a load on the output, an inline link or a source that does not come back stop the run; every stop silences nothing, keeps the Rd on
// and releases to 5 V (finish()). The keep-alive silence is also bounded by the sink itself (k2/pd/pd_sink.hpp).
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>

#include "k2/pd/pd_sink.hpp"
#include "k2/pd/prog_track.hpp"
#include "k2/pd/pd_spec_timing.hpp"
#include "k2/pdspec/analyze.hpp"
#include "k2/pdspec/plan.hpp"
#include "k2/pdspec/prog_judge.hpp"
#include "k2/pdspec/result.hpp"

namespace k2::pdspec {

// The fixed transitions and the programmable passes share one record pool (RAM: the K2_PDSPEC build's stack budget). The Transitions
// family runs first and fills it from the front; the passes follow its last record. Fixed levels and programmable windows both come
// from SPR positions 1..7 (plan.hpp scan) plus at most one EPR node, so the largest mix is 7 SPR + 1 EPR fixed levels: 56 transitions
// and no pass. Every APDO takes a fixed level's place, and 4 passes (112 B) fit where its 2n transitions (40n B) were.
constexpr std::size_t pool_need(std::size_t spr_fixed, std::size_t prog, bool epr) {
    const std::size_t n = spr_fixed + (epr ? 1 : 0);
    return (n > 1 ? n * (n - 1) : 0) * sizeof(TransRec) + prog * kClasses * sizeof(PassRec);
}
inline constexpr std::size_t kPoolBytes = pool_need(kMaxNodes - 1, 0, true);
constexpr bool pool_fits_every_list() {
    for (std::size_t s = 1; s <= kMaxNodes - 1; ++s)
        for (std::size_t p = 0; s + p <= kMaxNodes - 1 && p <= kMaxProg; ++p)
            for (const bool e : {false, true})
                if (pool_need(s, p, e) > kPoolBytes) return false;
    return true;
}
static_assert(pool_fits_every_list(), "the record pool holds the transitions and passes of every SPR list");

struct Snapshot {
    bool attached = false;        // a PD source with valid Source_Capabilities is on the line
    bool inline_link = false;     // a sink sits behind the K2: nothing may be sent
    bool self_powered = false;    // VBUS is the K2's own supply (or a boot-pinned Rd)
    pd::SinkState state = pd::SinkState::Idle;
    pd::SinkResult result = pd::SinkResult::None;
    std::uint8_t pos = 0;
    std::uint16_t mv = 0;
    bool epr_mode = false;
    std::uint32_t req_us = 0, accept_us = 0, ps_rdy_us = 0;   // the sink's edge stamps, bit 0 set when real
    std::uint32_t accept_at = 0, ps_rdy_at = 0;               // the sink's ms stamps
    std::uint32_t hard_resets = 0, source_hard_resets = 0;    // K2-sent / source-sent
    std::uint16_t rejects_rx = 0, waits_rx = 0;               // Reject / Wait received, saturating counts
    std::uint32_t epr_ka_at = 0;                              // the sink's ms stamp of its last EPR_KeepAlive (0 = none yet)
    std::uint32_t caps_rx = 0;
    // The sink's last Get_PPS_Status (pd::Sink::request_pps_status): its state, a sequence number bumped when an ask ends, the ask's
    // TX_START and the answer's RX edge (us, bit 0 set when real), and the raw PPSSDB fields.
    pd::PpsAsk pps_ask = pd::PpsAsk::None;
    std::uint8_t pps_seq = 0;
    std::uint32_t pps_ask_us = 0, pps_rx_us = 0;
    std::uint16_t pps_mv_raw = 0xFFFF;
    std::uint8_t pps_ma_raw = 0xFF, pps_flags = 0;
    std::int32_t vbus_mv = 0, ibus_ma = 0;
    std::uint32_t now_us = 0;
};

enum class Stop : std::uint8_t { None, Done, Cancelled, Overvoltage, Load, NoRecovery, Stuck, NoPd, Inline, Timeout };
constexpr const char* stop_name(Stop s) {
    switch (s) {
        case Stop::None: return "running";
        case Stop::Done: return "done";
        case Stop::Cancelled: return "cancelled";
        case Stop::Overvoltage: return "VBUS over the limit";
        case Stop::Load: return "load on the output";
        case Stop::NoRecovery: return "source did not come back";
        case Stop::Stuck: return "step stuck";
        case Stop::NoPd: return "no PD contract";
        case Stop::Inline: return "link inline";
        case Stop::Timeout: return "run time cap";
    }
    return "?";
}

enum class StartResult : std::uint8_t { Ok, Busy, NoCaps, Inline, SelfPowered, NoMonitor, Running };
constexpr const char* start_name(StartResult r) {
    switch (r) {
        case StartResult::Ok: return "ok";
        case StartResult::Busy: return "ports busy";
        case StartResult::NoCaps: return "no PD source";
        case StartResult::Inline: return "link inline";
        case StartResult::SelfPowered: return "K2 powered by the charger";
        case StartResult::NoMonitor: return "no power monitor";
        case StartResult::Running: return "already running";
    }
    return "?";
}

// Which keep-alive the sink holds back: one kind at a time, so an EPR contract keeps its EPR_KeepAlive while the PPS timeout is timed.
using Silence = pd::KaSilence;

enum class Family : std::uint8_t { Transitions, Pps, PpsKeepAlive, EprKeepAlive, HardReset, Detach, Count };
inline constexpr std::size_t kFamilies = static_cast<std::size_t>(Family::Count);
inline constexpr std::uint8_t kAllFamilies = (1u << kFamilies) - 1;
constexpr const char* family_name(Family f) {
    switch (f) {
        case Family::Transitions: return "Transitions";
        case Family::Pps: return "PPS/AVS";
        case Family::PpsKeepAlive: return "PPS timeout";
        case Family::EprKeepAlive: return "EPR timeout";
        case Family::HardReset: return "Hard Reset";
        case Family::Detach: return "Detach";
        case Family::Count: break;
    }
    return "?";
}

class Env {
public:
    virtual Snapshot snapshot() = 0;
    virtual const pd::SourceCaps& caps() = 0;
    virtual std::uint32_t cap_mv() = 0;                    // the monitor ceiling a request may ask for
    virtual bool epr_proof() = 0;                          // the cable proof EPR levels need
    virtual pd::Err request(const pd::Choice& c) = 0;      // the sink's own guards apply; Ok = queued
    virtual void release() = 0;                            // back to 5 V (PDO 1)
    virtual bool hard_reset() = 0;                         // a K2 Hard Reset on the wire; false = refused
    virtual void rd_off(bool off) = 0;                     // the detach gap: Rd away (true) / back as a fresh attach (false)
    virtual void keepalive_silence(Silence s) = 0;         // the sink holds that keep-alive back (bounded by the sink itself)
    virtual pd::Err ask_pps_status() = 0;                  // a Get_PPS_Status through the sink's guards; Ok = queued
    // The transition capture: arm and trigger now with a post window of post_us (false = no capture: busy or no monitor), frozen yet,
    // the PD edges from its markers, the analysis of the frozen window, and release.
    virtual bool capture_start(std::uint32_t post_us) = 0;
    virtual bool capture_frozen() = 0;
    virtual Edges capture_edges(std::uint32_t from_us) = 0;
    virtual TransitionOut capture_transition(const TransitionIn& in) = 0;
    virtual StepOut capture_step(const StepIn& in) = 0;
    virtual void capture_stop() = 0;
    // Reporting hooks (the firmware logs [PDSPEC] lines; the defaults do nothing).
    virtual std::span<std::uint8_t> record_pool() { return {}; }
    virtual void on_item(Family, std::uint8_t /*idx*/, std::uint8_t /*total*/) {}
    virtual void on_note(const char*) {}
    virtual void on_finish(Stop) {}
    // One measurement of a programmable move (its APDO in m.a & 15), the Accept and Get_PPS_Status included: a caller that keeps
    // per-APDO results (the Deep charger test's range rows) folds it in; the merged aggregates are unchanged by it.
    virtual void on_apdo(Check, const Meas&) {}

protected:
    ~Env() = default;   // never deleted through the interface (a virtual destructor would pull operator delete into the link)
};

// Timing of the engine itself (ms).
inline constexpr std::uint32_t kStepMaxMs = 10000;       // a Request not resolved by the sink after this: Stuck
inline constexpr std::uint32_t kBusyMs = 3000;           // a refused-for-now Request is retried this long
inline constexpr std::uint32_t kSettleMaxMs = 12000;     // a setup level waits this long for VBUS (an unloaded fall is slow)
inline constexpr std::uint32_t kSettleQuietMs = 300;     // ... and VBUS must have been in its band this long
inline constexpr std::uint32_t kFrozenMaxMs = 3000;      // the capture must freeze by then after the step
inline constexpr std::uint32_t kLiveBandMaxMs = 15000;   // a fall that leaves the capture window is followed live this long
inline constexpr std::uint32_t kLiveBandPolls = 3;       // ... and counts as in the band after this many polls inside it
inline constexpr std::uint32_t kPpsKaWaitMs = pd::spec::kPpsTimeoutMaxMs + 5000;   // no Hard Reset by then: none
inline constexpr std::uint32_t kEprKaWaitMs = 3000;
inline constexpr std::uint32_t kResetWatchMs = 6000;     // a Hard Reset / detach path is followed this long
inline constexpr std::uint32_t kDetachHoldMs = 200;      // Rd stays away this long after VBUS reached vSafe0V
inline constexpr std::uint32_t kDetachMaxMs = 3000;      // ... and never longer than this
inline constexpr std::uint32_t kRecoverMs = 10000;       // after a reset: a 5 V contract again by then, or NoRecovery
inline constexpr std::uint32_t kRecoverAskMs = 3000;      // a source back without a contract of ours: ask for 5 V after this
inline constexpr std::uint32_t kReleaseMs = 12000;       // the 5 V restore must be confirmed by then
inline constexpr std::uint32_t kDarkFinalMs = 3000;      // a stop on a dark line ends once it has stayed dark this long
inline constexpr std::uint32_t kStartMs = 10000;         // a contract must exist by then
inline constexpr std::uint32_t kMaxRunMs = 1200000;      // 20 min
inline constexpr std::uint32_t kPostFixedUs = 1500000;   // capture window after a fixed step's Request
inline constexpr std::uint32_t kPostStepUs = 300000;     // ... after a small programmable step's Request
// ... after a large one (a slam, an AVS step over 1 V): the 275 / 325 ms limit plus the 50 ms tail the level is read from, with room
// to see how late a slow one is (an unloaded fall to the minimum).
inline constexpr std::uint32_t kPostLargeUs = 700000;
inline constexpr std::int32_t kLoadAbortMa = 100;
inline constexpr std::uint8_t kLoadPolls = 5;
inline constexpr std::uint8_t kOvPolls = 3;
inline constexpr std::int32_t kMonoAllowMv = 8;          // a 20 mV step may read this much the wrong way (monitor noise)
inline constexpr std::int32_t kRestoreMaxMv = 5500;      // a 5 V restore is confirmed at or under this
inline constexpr std::uint32_t kSuspectAcceptUs = 300;   // an edge-stamp Accept faster than this is not credible
// The millisecond fallback (no usable edge stamp, no capture): the Accept's read lags the real edge by up to a service pass, the Request
// leaves up to a pass after send(); each side of a difference of two read times is off by about one pass.
inline constexpr std::int32_t kMsFallbackAcceptGapUs = 5000, kMsFallbackRdyGapUs = 2000;
// The age a live VBUS reading can have (the INA226's normal mode refreshes every ~4.5 ms): added to the bracket of every value timed
// from live polling (the capture's own samples carry their real spacing instead).
inline constexpr std::uint32_t kVbusPeriodMs = 5;

class Engine {
public:
    // One engine per run: the session emplaces a fresh one for every start (no reset path that could leave a field behind).
    explicit Engine(Env& env) : env_(&env), pool_(env.record_pool()) {}

    // families: a bit per Family (1 << Family), 0 = all of them. The ones left out read "not selected". opts: kOptFull20 sweeps the
    // whole window at 20 mV (else three 10-step windows). covered: a bit per APDO position whose steps the charger test already handed
    // in (judge_test_move): such a window runs a short cover (plan.hpp make_cover_sweep) and its slams, with their status asks.
    StartResult start(std::uint32_t now_ms, std::uint8_t families = 0, std::uint8_t opts = 0, std::uint8_t covered = 0);
    // One programmable move of a Deep charger-test run, captured and analyzed by the caller, judged into the aggregates before start().
    // chain: the move followed a judged move of the same walk, whose level is its monotonic reference.
    void judge_test_move(const ProgMove& mv, bool chain, const Edges& e, const StepOut& o, const Snapshot& s);
    void cancel() { if (running_ && !finishing_) cancel_ = true; }
    void poll(std::uint32_t now_ms);

    bool running() const { return running_; }
    bool finished() const { return !running_ && stop_ != Stop::None; }
    Stop stop() const { return stop_; }
    bool release_unconfirmed() const { return release_unconfirmed_; }
    Family family() const { return fam_; }
    std::uint8_t item() const { return item_; }
    std::uint8_t items() const { return items_; }
    std::uint8_t covered() const { return covered_; }
    std::uint32_t elapsed_ms(std::uint32_t now_ms) const { return running_ ? now_ms - t0_ : end_ms_ - t0_; }
    const std::array<Agg, kChecks>& agg() const { return agg_; }
    const Agg& agg(Check c) const { return agg_[static_cast<std::size_t>(c)]; }
    const Timers& timers() const { return tm_; }
    std::span<const TransRec> trans() const {
        return n_tr_ ? std::span<const TransRec>{std::launder(reinterpret_cast<const TransRec*>(pool_.data())), n_tr_}
                     : std::span<const TransRec>{};
    }
    std::span<const PassRec> passes() const {
        return n_pass_
                   ? std::span<const PassRec>{std::launder(reinterpret_cast<const PassRec*>(pool_.data() + n_tr_ * sizeof(TransRec))),
                                              n_pass_}
                   : std::span<const PassRec>{};
    }
    CapsNote caps_note() const { return note_; }
    std::uint8_t opts() const { return full20_ ? kOptFull20 : 0; }
    std::uint16_t score() const { return pdspec::score(agg_); }
    const Levels& levels() const { return L_; }
#if K2_SPECROWS
    // The level of fixed position pos in mV (0 = not a level of this run).
    std::uint16_t fixed_mv(std::uint8_t pos) const {
        for (std::uint8_t k = 0; k < L_.n_fixed; ++k) if (L_.fixed[k] == pos) return fixed_mv_[k];
        return 0;
    }
#endif
    std::uint8_t families() const { return mask_; }

private:
    enum class Ph : std::uint8_t {
        FamStart, Goto, Settle, Measure, Frozen, LiveBand, Status, KaWait, ResetWatch, DetachGap, Recover, Restore, Finishing, Done
    };
    enum class After : std::uint8_t { Measure, PpsSteps, PpsNext, PpsKa, EprKa, HardReset, Detach, Family };
    struct Lat { bool acc = false, rdy = false; std::uint32_t acc_us = 0, rdy_us = 0; };

    // ---- helpers ----
    static bool due(std::uint32_t now, std::uint32_t t) { return static_cast<std::int32_t>(now - t) >= 0; }
    const pd::Pdo& pdo(std::uint8_t pos) const { return env_->caps().pdos[pos - 1]; }
    std::int32_t level_mv(std::uint8_t pos, std::uint16_t mv) const { return mv ? mv : pdo(pos).max_mv; }
    bool programmable(std::uint8_t pos) const { return pos && pdo(pos).type != pd::PdoType::Fixed; }
    void meas(Check c, const Meas& m);   // out of line (engine.cpp): the record in flight (K2_SPECROWS) and Env::on_apdo
    void judge_move(const ProgMove& mv, const StepOut& o, const Edges& e, const Snapshot& s, PassRec& r);   // judge_prog, reported per APDO
    void skip(Check c, Skip why) { auto& a = agg_[static_cast<std::size_t>(c)]; if (a.n == 0 && a.skip == Skip::None) a.skip = why; }
    void skip_family(Family f, Skip why);
    void next_family();
    void begin_family(std::uint32_t now);
    void go(std::uint8_t pos, std::uint16_t mv, After after, std::uint32_t now);
    bool send(std::uint32_t now);
    int outcome(const Snapshot& s) const;   // 0 pending, 1 Ok, -1 refused (old contract kept), -2 failed (contract gone)
    bool settled(const Snapshot& s) const;
    void measure_begin(std::uint32_t now);
    void measure_done(const Snapshot& s, std::uint32_t now);
    void judge_transition(const TransitionOut& o, const Edges& e, std::uint32_t now);
    void judge_prog(const ProgMove& mv, const StepOut& o, const Edges& e, const Snapshot& s, PassRec& r);
    void next_item(std::uint32_t now);
    void restore(std::uint32_t now);
    void finish(Stop why, std::uint32_t now);
    bool guards(const Snapshot& s, std::uint32_t now);
    Lat latencies(const Edges& e, const Snapshot& s, Check rdy_c, std::int32_t rdy_lim_us, std::uint8_t a, std::uint8_t b);
    void after(const Snapshot& s, std::uint32_t now);
    void refused(std::uint32_t now);
    void lost(const Snapshot& s, std::uint32_t now);
    void live_band(const Snapshot& s, std::uint32_t now);
    void ka_wait(const Snapshot& s, std::uint32_t now);
    void reset_watch(const Snapshot& s, std::uint32_t now);
    void detach_gap(const Snapshot& s, std::uint32_t now);
    void recover(const Snapshot& s, std::uint32_t now);
    const Prog& prog() const;
    bool cover() const { return covered_ >> prog().pos & 1; }
    void pass_begin(std::uint32_t now);
    void prog_next(std::uint32_t now);
    void pass_end(std::uint32_t now);
    std::array<Check, 6> prog_checks_meter() const;
    void status_begin(std::uint32_t now);
    void status_poll(const Snapshot& s, std::uint32_t now);
    static std::int32_t pps_status_faults(const Snapshot& s, std::int32_t target_mv);
    std::uint32_t request_origin(const Snapshot& s) const;
    void reattach_judge(std::uint32_t now);
    void track_gap(std::uint32_t now);
    std::uint8_t node(std::size_t i) const { return L_.fixed[walk_.node[i]]; }

    Env* env_;
    bool running_ = false, finishing_ = false, cancel_ = false, release_unconfirmed_ = false;
    bool hr_epr_ = false;   // the K2's Hard Reset went out in EPR mode
    bool prog_meas_ = false;   // the measurements in flight are a programmable move's: Env::on_apdo gets them too
    Stop stop_ = Stop::None;
    std::uint32_t t0_ = 0, end_ms_ = 0;
    Levels L_{};
    Walk walk_{};
    Family fam_ = Family::Transitions;
    std::uint8_t mask_ = kAllFamilies;
    Ph ph_ = Ph::FamStart;
    After after_ = After::Family;
    std::uint8_t item_ = 0, items_ = 0;
    // The level being asked for and its Request.
    std::uint8_t tgt_pos_ = 0;
    std::uint16_t tgt_mv_ = 0;
    bool sent_ = false;
    std::uint32_t send_t0_ = 0, phase_t_ = 0, band_since_ = 0;
    std::uint32_t accept0_ = 0, hard0_ = 0;
    std::uint16_t rej0_ = 0, wait0_ = 0;
    std::uint8_t bad_ = 0;              // walk nodes the source refused (bit per node): pairs with them are left out
    std::int32_t allowed_mv_ = 5000;    // the highest level the step in flight may show (the overvoltage guard)
    std::int32_t top_mv_ = 5000;        // the highest level of the run (the guard between steps: resets, recovery)
#if K2_SPECROWS
    std::array<std::uint16_t, kMaxNodes> fixed_mv_{};   // the mV of L_.fixed[k]
#endif
    std::uint8_t ov_polls_ = 0;
    // The measurement in flight.
    std::uint32_t from_us_ = 0;
    std::int32_t from_mv_ = 0;
    bool captured_ = false;
    TransRec cur_{};
    std::uint32_t live_t_ = 0, live_n_ = 0, last_poll_ = 0, poll_gap_ = 0;
    bool live_first_ = false, live_unseen_ = false;
    // The programmable sweep.
    std::uint8_t w_ = 0, cls_ = 0;      // the window (L_.prog) and the step class of the pass in flight
    std::uint8_t covered_ = 0;          // start(): the windows the charger test swept
    Sweep sw_{};
    Move pend_{};                       // a move held back while Get_PPS_Status is asked
    bool have_pend_ = false, asked_top_ = false, asked_end_ = false, full20_ = false;
    std::int8_t dir_ = 0;               // the direction of the last measured move
    std::int32_t cur_mv_ = 0;           // the level last requested
    std::int32_t prev_mv_ = 0;          // the last settled level the next move's monotonic check compares with
    bool prev_valid_ = false;           // prev_mv_ was reached by a measured step of the pass (a setup move or a jump leaves it false)
    std::int32_t ema_mv_ = 0;
    bool st_sent_ = false;
    std::uint8_t st_seq0_ = 0;
    CapsNote note_ = CapsNote::None;
    // Timers.
    ResetTrack track_{};
    std::uint32_t ka_t0_ = 0, ka_extra_ = 0, rd_back_ = 0, off_at_ = 0, dark_since_ = 0;
    bool rd_off_ = false, silenced_ = false, asked_ = false, left_ready_ = false;
    std::uint32_t hr0_ = 0, caps0_ = 0, src_resets_ = 0;
    std::uint8_t load_polls_ = 0;
    // Results.
    std::array<Agg, kChecks> agg_{};
    Timers tm_{};
    // The record pool (borrowed via Env::record_pool(), up to kPoolBytes): n_tr_ TransRec from the front, then n_pass_ PassRec. Empty
    // when no pool was lent (the charger test's spec section reads agg() only).
    std::span<std::uint8_t> pool_{};
    std::size_t n_tr_ = 0;
    std::size_t n_pass_ = 0;
    void push_tr(const TransRec& r) {
        if (n_pass_ == 0 && (n_tr_ + 1) * sizeof(TransRec) <= pool_.size()) ::new (pool_.data() + n_tr_++ * sizeof(TransRec)) TransRec(r);
    }
    // The storage of the pass in flight (index n_pass_), or nullptr when the pool is full; pass_slot() is the record pass_begin made there.
    std::uint8_t* pass_raw() {
        const std::size_t off = n_tr_ * sizeof(TransRec) + n_pass_ * sizeof(PassRec);
        return off + sizeof(PassRec) <= pool_.size() ? pool_.data() + off : nullptr;
    }
    PassRec* pass_slot() {
        std::uint8_t* m = pass_raw();
        return m ? std::launder(reinterpret_cast<PassRec*>(m)) : nullptr;
    }
};


// ---------------------------------------------------------------------------------------------------------------------------------------

// The checks a family owns (the skip reasons go to these).
constexpr bool owns(Family f, Check c) {
    switch (f) {
        case Family::Transitions: return c <= Check::Accuracy;
        case Family::Pps: return (c >= Check::PpsMonotonic && c <= Check::PpsSettle) || (c >= Check::PpsBand && c <= Check::AvsTransient) || c == Check::PpsTrack || c == Check::AvsTrack;
        case Family::PpsKeepAlive: return c == Check::PpsKeepAlive;
        case Family::EprKeepAlive: return c == Check::EprKeepAlive;
        case Family::HardReset: return c >= Check::HrSafe0V && c <= Check::HrCaps;
        case Family::Detach: return c == Check::DetachSafe0V || c == Check::Reattach || c == Check::ReattachCaps;
        case Family::Count: break;
    }
    return false;
}


// The fault bits of the last answer (result.hpp kPs*). PPSSDB: Output Voltage u16 in 20 mV (0xFFFF = not supported), Output Current u8
// in 50 mA (0xFF = not supported), Real Time Flags u8 (bit 0 reserved, bits 2..1 PTF, bit 3 OMF, bits 7..4 reserved).
constexpr std::int32_t pps_status_faults(pd::PpsAsk ask, std::uint16_t raw_mv, std::uint8_t raw_ma, std::uint8_t flags, std::int32_t target_mv) {
    if (ask == pd::PpsAsk::NotSupported) return kPsNotSupported;
    if (ask != pd::PpsAsk::Got) return kPsNoAnswer;
    std::int32_t bits = 0;
    if (raw_mv != 0xFFFF) {
        const std::int32_t tol = (target_mv * std::int32_t(pd::spec::kPpsStatusVoltPermille) / 1000 + 10) / 20 * 20;
        const std::int32_t mv = std::int32_t(raw_mv) * 20;
        if (mv < target_mv - tol || mv > target_mv + tol) bits |= kPsVoltage;
    }
    if (raw_ma > 3 && raw_ma != 0xFF) bits |= kPsCurrent;
    if (flags & 0x08) bits |= kPsOmf;
    if ((flags & 0xF1) || ((flags >> 1) & 3) > 1) bits |= kPsFlags;
    return bits;
}

}  // namespace k2::pdspec
