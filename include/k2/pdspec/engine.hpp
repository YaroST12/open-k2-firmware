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
//                top and the end of every PPS pass.
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

#include "k2/pd/pd_sink.hpp"
#include "k2/pd/prog_track.hpp"
#include "k2/pd/pd_spec_timing.hpp"
#include "k2/pdspec/analyze.hpp"
#include "k2/pdspec/plan.hpp"
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
    virtual void on_item(Family, std::uint8_t /*idx*/, std::uint8_t /*total*/) {}
    virtual void on_note(const char*) {}
    virtual void on_finish(Stop) {}

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
    explicit Engine(Env& env) : env_(&env) {}

    // families: a bit per Family (1 << Family), 0 = all of them. The ones left out read "not selected". opts: kOptFull20 sweeps the
    // whole window at 20 mV (else three 10-step windows).
    StartResult start(std::uint32_t now_ms, std::uint8_t families = 0, std::uint8_t opts = 0) {
        if (running_) return StartResult::Running;
        const Snapshot s = env_->snapshot();
        if (s.inline_link) return StartResult::Inline;
        if (s.self_powered) return StartResult::SelfPowered;
        if (!s.attached || !env_->caps().valid) return StartResult::NoCaps;
        running_ = true;
        t0_ = now_ms;
        L_ = scan(env_->caps(), env_->cap_mv(), env_->epr_proof());
        walk_ = make_walk(L_.n_fixed);
        full20_ = (opts & kOptFull20) != 0;
        note_ = pdspec::caps_note(env_->caps());
        hr0_ = s.source_hard_resets;
        caps0_ = s.caps_rx;
        for (std::uint8_t k = 0; k < L_.n_fixed; ++k) {
#if K2_SPECROWS
            fixed_mv_[k] = static_cast<std::uint16_t>(pdo(L_.fixed[k]).max_mv);   // the rows name the levels after the capabilities are gone
#endif
            if (pdo(L_.fixed[k]).max_mv > top_mv_) top_mv_ = pdo(L_.fixed[k]).max_mv;
        }
        for (std::uint8_t k = 0; k < L_.n_prog; ++k) if (L_.prog[k].hi > top_mv_) top_mv_ = L_.prog[k].hi;
        fam_ = Family::Transitions;
        ph_ = Ph::FamStart;
        mask_ = families ? families : kAllFamilies;
        return StartResult::Ok;
    }
    void cancel() { if (running_ && !finishing_) cancel_ = true; }
    void poll(std::uint32_t now_ms);

    bool running() const { return running_; }
    bool finished() const { return !running_ && stop_ != Stop::None; }
    Stop stop() const { return stop_; }
    bool release_unconfirmed() const { return release_unconfirmed_; }
    Family family() const { return fam_; }
    std::uint8_t item() const { return item_; }
    std::uint8_t items() const { return items_; }
    std::uint32_t elapsed_ms(std::uint32_t now_ms) const { return running_ ? now_ms - t0_ : end_ms_ - t0_; }
    const std::array<Agg, kChecks>& agg() const { return agg_; }
    const Agg& agg(Check c) const { return agg_[static_cast<std::size_t>(c)]; }
    const Timers& timers() const { return tm_; }
    std::span<const TransRec> trans() const { return {std::launder(reinterpret_cast<const TransRec*>(pool_.data())), n_tr_}; }
    std::span<const PassRec> passes() const {
        return {std::launder(reinterpret_cast<const PassRec*>(pool_.data() + n_tr_ * sizeof(TransRec))), n_pass_};
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
#if K2_SPECROWS
    // A fixed transition's measurement also marks the record in flight (cur_): measured, and failed (the aggregate's own judgement, the
    // bracket counted), so the per-transition rows (chargertest/spec_rows.hpp) agree with the check's cell.
    void meas(Check c, const Meas& m) {
        const bool bad = agg_[static_cast<std::size_t>(c)].add(def(c).kind, m);
        if (const std::uint8_t b = tr_bit(c)) {
            cur_.seen = static_cast<std::uint8_t>(cur_.seen | b);
            if (bad) cur_.fails = static_cast<std::uint8_t>(cur_.fails | b);
        }
    }
#else
    void meas(Check c, const Meas& m) { agg_[static_cast<std::size_t>(c)].add(def(c).kind, m); }
#endif
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
    void judge_prog(const StepOut& o, const Edges& e, const Snapshot& s);
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
    void pass_begin(std::uint32_t now);
    void prog_next(std::uint32_t now);
    void pass_end(std::uint32_t now);
    bool large_step(std::int32_t step_mv) const;
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
    // The record pool (kPoolBytes): n_tr_ TransRec from the front, then n_pass_ PassRec. A record that does not fit is dropped (its
    // measurements still count in agg_); the static_assert above says none is for a list the scan can produce.
    alignas(4) std::array<std::uint8_t, kPoolBytes> pool_{};
    std::size_t n_tr_ = 0;
    std::size_t n_pass_ = 0;
    void push_tr(const TransRec& r) {
        if (n_pass_ == 0 && (n_tr_ + 1) * sizeof(TransRec) <= kPoolBytes) ::new (pool_.data() + n_tr_++ * sizeof(TransRec)) TransRec(r);
    }
    // The storage of the pass in flight (index n_pass_), or nullptr when the pool is full; pass_slot() is the record pass_begin made there.
    std::uint8_t* pass_raw() {
        const std::size_t off = n_tr_ * sizeof(TransRec) + n_pass_ * sizeof(PassRec);
        return off + sizeof(PassRec) <= kPoolBytes ? pool_.data() + off : nullptr;
    }
    PassRec* pass_slot() {
        std::uint8_t* m = pass_raw();
        return m ? std::launder(reinterpret_cast<PassRec*>(m)) : nullptr;
    }
};


// ---------------------------------------------------------------------------------------------------------------------------------------

inline constexpr std::int32_t kNever = 99999999;   // a value for an event that never came (fails any Max limit, reads "none")

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

inline void Engine::skip_family(Family f, Skip why) {
    for (std::size_t k = 0; k < kChecks; ++k) if (owns(f, Check(k))) skip(Check(k), why);
}

inline void Engine::next_family() {
    fam_ = Family(static_cast<std::uint8_t>(fam_) + 1);
    item_ = items_ = 0;
    ph_ = Ph::FamStart;
}

inline void Engine::go(std::uint8_t pos, std::uint16_t mv, After a, std::uint32_t now) {
    tgt_pos_ = pos;
    tgt_mv_ = mv;
    after_ = a;
    phase_t_ = now;
    band_since_ = 0;
    const Snapshot s = env_->snapshot();
    ema_mv_ = s.vbus_mv;
    {
        const std::int32_t cur = s.pos ? level_mv(s.pos, s.mv) : 5000, want = level_mv(pos, mv);
        allowed_mv_ = cur > want ? cur : want;
    }
    if (s.state == pd::SinkState::Ready && s.pos == pos && (mv == 0 || s.mv == mv)) { ph_ = Ph::Settle; return; }
    sent_ = false;
    ph_ = Ph::Goto;
}

// true = queued. A Busy sink is retried for kBusyMs; any other refusal (over the ceiling, bad choice) is a refused level.
inline bool Engine::send(std::uint32_t now) {
    const Snapshot s = env_->snapshot();
    pd::Choice c;
    c.pos = tgt_pos_;
    c.mv = tgt_mv_;
    const pd::Err e = env_->request(c);
    if (e == pd::Err::Ok) {
        sent_ = true;
        left_ready_ = false;
        send_t0_ = now;
        accept0_ = s.accept_at;
        hard0_ = s.hard_resets;
        rej0_ = s.rejects_rx;
        wait0_ = s.waits_rx;
        return true;
    }
    if (e != pd::Err::Busy || now - phase_t_ > kBusyMs) refused(now);
    return false;
}

inline int Engine::outcome(const Snapshot& s) const {
    if (s.hard_resets != hard0_) return -2;   // the K2 Hard Reset the source (no Accept / PS_RDY in time)
    if (!s.attached) return -2;
    if (s.state == pd::SinkState::Ready) {
        if (s.result == pd::SinkResult::Ok && s.accept_at != accept0_ && s.pos == tgt_pos_) return 1;
        // The counters, not a state change: a Reject can come and go between two polls.
        if (s.rejects_rx != rej0_ || (s.result == pd::SinkResult::WaitExhausted && s.waits_rx != wait0_)) return -1;
        return 0;
    }
    if (s.state == pd::SinkState::Failed || (left_ready_ && s.state == pd::SinkState::Idle)) return -2;
    return 0;
}

inline bool Engine::settled(const Snapshot& s) const {
    const std::int32_t t = level_mv(tgt_pos_, tgt_mv_);
    if (programmable(tgt_pos_)) return s.vbus_mv >= pd::programmable_floor_mv(t) && s.vbus_mv <= pd::programmable_ceiling_mv(t);
    return in_band(s.vbus_mv, t);
}

// A setup level the source would not give (Reject / Wait / refused by the sink): the family ends here, back to 5 V.
inline void Engine::refused(std::uint32_t now) {
    env_->on_note("level refused");
    env_->capture_stop();
    if (after_ == After::Family && ph_ == Ph::Goto) { finish(Stop::Stuck, now); return; }   // even 5 V refused: nothing to restore to
    if (fam_ == Family::Transitions) {   // that level is left out, the walk goes on with the pairs that do not touch it
        for (std::uint8_t k = 0; k < L_.n_fixed; ++k) if (L_.fixed[k] == tgt_pos_) bad_ = static_cast<std::uint8_t>(bad_ | 1u << k);
        if (ph_ == Ph::Measure) {
            cur_.flags |= kTrRefused;
            push_tr(cur_);
            next_item(now);
        } else {
            measure_begin(now);
        }
        return;
    }
    skip_family(fam_, Skip::Refused);
    restore(now);
}

// The contract went (the K2's own Hard Reset after a missing answer, a detach): wait for a 5 V contract, then the next family.
inline void Engine::lost(const Snapshot& s, std::uint32_t now) {
    env_->on_note("contract lost");
    // The K2 Hard Reset the source after an Accept with no PS_RDY in time (tPSTransition): that is the measurement.
    if ((ph_ == Ph::Measure || ph_ == Ph::Frozen) && s.hard_resets != hard0_ && s.accept_at != accept0_) {
        const bool epr = (cur_.flags & kTrEpr) != 0;
        if (fam_ == Family::Pps) {   // a programmable step: its own PS_RDY check and window
            const bool avs = prog().kind == ProgKind::Avs, large = large_step(std::abs(tgt_mv_ - from_mv_));
            const std::uint32_t ms = avs ? (large ? pd::spec::kAvsLargeStepMs : pd::spec::kAvsSmallStepMs)
                                         : (large ? pd::spec::kPpsLargeStepMs : pd::spec::kPpsSmallStepMs);
            meas(avs ? Check::AvsPsRdy : Check::PpsPsRdy, {kNever, 0, 0, std::int32_t(ms * 1000), tgt_pos_, static_cast<std::uint8_t>(tgt_mv_ / 100)});
        } else {
            meas(Check::PsRdyTime, {kNever, 0, 0, std::int32_t((epr ? pd::spec::kSrcTransReqEprMs : pd::spec::kSrcTransReqSprMs) * 1000), tgt_pos_, 0});
        }
    }
    if (silenced_) { env_->keepalive_silence(Silence::Off); silenced_ = false; }
    env_->capture_stop();
    skip_family(fam_, Skip::Stopped);
    after_ = After::Family;
    ph_ = Ph::Recover;
    phase_t_ = now;
    asked_ = false;
}

inline void Engine::restore(std::uint32_t now) { go(1, 0, After::Family, now); }

inline void Engine::begin_family(std::uint32_t now) {
    if (fam_ != Family::Count && !(mask_ >> static_cast<std::uint8_t>(fam_) & 1)) {
        skip_family(fam_, Skip::NotSelected);
        next_family();
        return;
    }
    switch (fam_) {
        case Family::Transitions:
            if (L_.n_fixed < 2) { skip_family(fam_, Skip::OneLevel); next_family(); return; }
            items_ = static_cast<std::uint8_t>(walk_.len - 1);
            env_->on_item(fam_, 0, items_);
            measure_begin(now);
            return;
        case Family::Pps: {
            bool pps = false, avs = false;
            for (std::uint8_t k = 0; k < L_.n_prog; ++k) (L_.prog[k].kind == ProgKind::Avs ? avs : pps) = true;
            for (std::size_t k = 0; k < kChecks; ++k)
                if (owns(fam_, Check(k))) {
                    if (avs_check(Check(k)) && !avs) skip(Check(k), Skip::NoAvs);
                    if (!avs_check(Check(k)) && !pps) skip(Check(k), Skip::NoPps);
                }
            if (L_.n_prog == 0) { next_family(); return; }
            items_ = static_cast<std::uint8_t>(L_.n_prog * kClasses);
            w_ = cls_ = 0;
            env_->on_item(fam_, 0, items_);
            go(prog().pos, static_cast<std::uint16_t>(prog().lo), After::PpsSteps, now);
            return;
        }
        case Family::PpsKeepAlive: {
            // The first PPS window, at 5 V when it reaches it (else its nearest end): the level the source keeps longest.
            const Prog* p = nullptr;
            for (std::uint8_t k = 0; k < L_.n_prog && !p; ++k) if (L_.prog[k].kind == ProgKind::Pps) p = &L_.prog[k];
            if (!p) { skip_family(fam_, Skip::NoPps); next_family(); return; }
            const std::int32_t mv = kPpsKaMv < p->lo ? p->lo : kPpsKaMv > p->hi ? p->hi : kPpsKaMv;
            items_ = 1;
            env_->on_item(fam_, 0, 1);
            go(p->pos, static_cast<std::uint16_t>(mv), After::PpsKa, now);
            return;
        }
        case Family::EprKeepAlive:
            if (L_.epr == 0) { skip_family(fam_, L_.epr_offered ? Skip::NoEprProof : Skip::NoEpr); next_family(); return; }
            items_ = 1;
            env_->on_item(fam_, 0, 1);
            go(L_.epr, 0, After::EprKa, now);
            return;
        case Family::HardReset:
            items_ = 1;
            env_->on_item(fam_, 0, 1);
            go(L_.top_spr ? L_.top_spr : 1, 0, After::HardReset, now);
            return;
        case Family::Detach:
            items_ = 1;
            env_->on_item(fam_, 0, 1);
            go(L_.top_spr ? L_.top_spr : 1, 0, After::Detach, now);
            return;
        case Family::Count:
            finish(Stop::Done, now);
            return;
    }
}

inline void Engine::after(const Snapshot& s, std::uint32_t now) {
    switch (after_) {
        case After::Measure: measure_begin(now); return;
        // A point reached by a move that is not of the pass's own step class (the window entry, a jump between windows, the landing of a
        // slam) is NOT a reference for the monotonic / track comparison: a source may land such a jump up to vPpsValid off and hold the
        // landing until the next request (UGREEN Nexode Air: 95 mV high), and the first in-window step then looks like a fall of it. The
        // first in-window result (judge_prog) is the first reference; the entry point is only a place to start from.
        case After::PpsSteps:   // at the window's minimum: the first pass starts here
            cur_mv_ = tgt_mv_;
            prev_valid_ = false;
            pass_begin(now);
            return;
        case After::PpsNext:    // a setup move, or a measured one that ended outside its band, has settled
            cur_mv_ = tgt_mv_;
            prev_valid_ = false;
            prog_next(now);
            return;
        case After::PpsKa:
            env_->keepalive_silence(Silence::Pps);
            silenced_ = true;
            ka_t0_ = request_origin(s);   // tPPSTimeout runs from the GoodCRC of the last Request
            ka_extra_ = 0;
            hr0_ = s.source_hard_resets;
            ph_ = Ph::KaWait;
            phase_t_ = now;
            return;
        case After::EprKa:
            if (!s.epr_mode) { skip_family(fam_, Skip::Refused); restore(now); return; }
            env_->keepalive_silence(Silence::Epr);
            silenced_ = true;
            // The source's timer runs from the last EPR_KeepAlive: the sink's stamp, or (none known) a bracket of one keep-alive period.
            ka_t0_ = s.epr_ka_at ? s.epr_ka_at : now;
            ka_extra_ = s.epr_ka_at ? 0 : pd::kEprKeepAliveMs;
            hr0_ = s.source_hard_resets;
            ph_ = Ph::KaWait;
            phase_t_ = now;
            return;
        case After::HardReset:
            tm_.hr_from_mv = static_cast<std::uint16_t>(s.vbus_mv < 0 ? 0 : s.vbus_mv);
            hr_epr_ = s.epr_mode;   // a reset that leaves EPR mode has the longer tSrcRecover
            caps0_ = s.caps_rx;
            track_.start(now);
            if (!env_->hard_reset()) { skip_family(fam_, Skip::Refused); restore(now); return; }
            ph_ = Ph::ResetWatch;
            phase_t_ = now;
            return;
        case After::Detach:
            tm_.det_from_mv = static_cast<std::uint16_t>(s.vbus_mv < 0 ? 0 : s.vbus_mv);
            track_.start(now);
            env_->rd_off(true);
            rd_off_ = true;
            off_at_ = 0;
            ph_ = Ph::DetachGap;
            phase_t_ = now;
            return;
        case After::Family: next_family(); return;
    }
}

// ---- fixed transitions ----

inline void Engine::measure_begin(std::uint32_t now) {
    // Pairs that touch a refused level are left out (each counted once, as refused).
    while (item_ + 1 < walk_.len && ((bad_ >> walk_.node[item_] & 1) || (bad_ >> walk_.node[item_ + 1] & 1))) {
        ++item_;
        if (item_ < items_) env_->on_item(fam_, item_, items_);   // past the last item there is nothing to announce
    }
    if (item_ + 1 >= walk_.len) { restore(now); return; }
    const std::uint8_t from = node(item_), to = node(item_ + 1);
    const Snapshot s = env_->snapshot();
    if (s.state != pd::SinkState::Ready || s.pos != from) { go(from, 0, After::Measure, now); return; }   // a setup step (after a refusal)
    cur_ = {};
    cur_.from = from;
    cur_.to = to;
    if (from >= 8 || to >= 8) cur_.flags |= kTrEpr;
    from_mv_ = pdo(from).max_mv;
    captured_ = env_->capture_start(kPostFixedUs);
    if (!captured_) {   // the capture is busy (another tool holds it): the timings come from the sink's stamps and live polling
        cur_.flags |= kTrNoCapture;
        for (const Check c : {Check::SrcStart, Check::PsRdyEarly, Check::PsRdyLow, Check::Overshoot}) skip(c, Skip::NoCapture);
    }
    from_us_ = s.now_us;
    tgt_pos_ = to;
    tgt_mv_ = 0;
    allowed_mv_ = from_mv_ > pdo(to).max_mv ? from_mv_ : pdo(to).max_mv;
    phase_t_ = now;
    sent_ = false;
    ph_ = Ph::Measure;
}

// Request -> Accept and Accept -> PS_RDY: the sink's edge stamps when they are real and credible, else the capture's markers. The PS_RDY
// goes to rdy (PsRdyTime for a fixed step, PpsPsRdy / AvsPsRdy for a programmable one) against rdy_lim_us; returned for the records.
inline Engine::Lat Engine::latencies(const Edges& e, const Snapshot& s, Check rdy_c, std::int32_t rdy_lim_us, std::uint8_t a, std::uint8_t b) {
    Lat l;
    std::uint32_t req = e.req_us, acc = e.accept_us, rdy = e.psrdy_us;
    bool have_acc = e.accept, have_rdy = e.psrdy;
    if ((s.req_us & 1) && (s.accept_us & 1) && s.accept_us - s.req_us >= kSuspectAcceptUs && s.accept_us - s.req_us < 1000000) {
        req = s.req_us; acc = s.accept_us; have_acc = true;
        if (s.ps_rdy_us & 1) { rdy = s.ps_rdy_us; have_rdy = static_cast<std::int32_t>(rdy - acc) > 0; }
    } else if (e.accept && (s.req_us & 1) == 0) {
        cur_.flags |= kTrSuspectStamp;
    }
    std::int32_t acc_gap = 0, rdy_gap = 0;
    if (!have_acc && s.accept_at != accept0_ && s.accept_at - send_t0_ < 1000) {
        // No edge stamp and no capture marker for the Accept (inferred from the trace: it is read behind the GoodCRC of the same service
        // pass, which takes the only stamp). The sink's ms stamps still time it, to a service pass: Request = send(), PS_RDY = its own read.
        // Everything of the capture's own (microsecond) PS_RDY is dropped: it must not be mixed with a millisecond Accept.
        cur_.flags |= kTrSuspectStamp;
        req = send_t0_; acc = s.accept_at;
        have_acc = true; have_rdy = false;
        acc_gap = kMsFallbackAcceptGapUs;
        l.acc_us = (acc - req) * 1000;
        if (s.ps_rdy_at && static_cast<std::int32_t>(s.ps_rdy_at - s.accept_at) >= 0) {   // 0 while inferred or not come yet
            rdy = s.ps_rdy_at; have_rdy = true; rdy_gap = kMsFallbackRdyGapUs;
            l.rdy_us = (rdy - acc) * 1000;
        }
    }
    if (have_acc) {
        l.acc = true;
        if (!acc_gap) l.acc_us = acc - req;
        meas(Check::AcceptTime, {std::int32_t(l.acc_us), acc_gap, 0, std::int32_t(pd::spec::kAcceptSpecLimitMs * 1000), a, b});
    }
    if (have_acc && have_rdy) {
        l.rdy = true;
        if (!rdy_gap) l.rdy_us = rdy - acc;
        meas(rdy_c, {std::int32_t(l.rdy_us), rdy_gap, 0, rdy_lim_us, a, b});
    } else if (have_acc && !acc_gap) {   // (the ms fallback cannot tell "never" from an inferred PS_RDY: no value, not a failure)
        meas(rdy_c, {kNever, 0, 0, rdy_lim_us, a, b});   // the contract was taken without a PS_RDY
    }
    return l;
}

inline void Engine::measure_done(const Snapshot& s, std::uint32_t now) {
    Edges e = captured_ ? env_->capture_edges(from_us_) : Edges{};
    if (fam_ == Family::Pps) {
        const StepOut o = captured_ && e.accept ? env_->capture_step({e.accept_us, from_mv_, tgt_mv_}) : StepOut{};
        env_->capture_stop();
        judge_prog(o, e, s);
        // Not inside the new band by the end of the window (an unloaded fall can be slow): the next move starts once VBUS is there, and
        // its monotonic check has no level to compare with.
        if (!o.band.seen && !o.settled) { prev_valid_ = false; go(tgt_pos_, tgt_mv_, After::PpsNext, now); return; }
        prog_next(now);
        return;
    }
    const bool epr = (cur_.flags & kTrEpr) != 0;
    const Lat l = latencies(e, s, Check::PsRdyTime, std::int32_t((epr ? pd::spec::kSrcTransReqEprMs : pd::spec::kSrcTransReqSprMs) * 1000),
                            cur_.from, cur_.to);
    if (l.acc) cur_.accept_100us = static_cast<std::uint16_t>(l.acc_us / 100 > 0xFFFE ? 0xFFFE : l.acc_us / 100);
    if (l.rdy) cur_.psrdy_ms = static_cast<std::uint16_t>(l.rdy_us / 1000 > 0xFFFE ? 0xFFFE : l.rdy_us / 1000);
    TransitionOut o{};
    if (captured_ && e.accept) o = env_->capture_transition({e, from_mv_, pdo(cur_.to).max_mv});
    env_->capture_stop();
    judge_transition(o, e, now);
}

inline void Engine::judge_transition(const TransitionOut& o, const Edges& e, std::uint32_t now) {
    const std::int32_t to_mv = pdo(cur_.to).max_mv;
    const bool up = to_mv > from_mv_, epr = (cur_.flags & kTrEpr) != 0;
    const std::uint8_t a = cur_.from, b = cur_.to;
    cur_.gap_ms = static_cast<std::uint8_t>(o.max_gap_us / 1000 > 255 ? 255 : o.max_gap_us / 1000);
    if (o.start.seen) {
        meas(Check::SrcStart, {std::int32_t(o.start.us), std::int32_t(o.start.gap_us), std::int32_t(pd::spec::kSrcTransitionMinMs * 1000), 0, a, b});
        cur_.start_ms = static_cast<std::uint16_t>(o.start.us / 1000);
    }
    if (o.psrdy_known) {
        cur_.psrdy_mv = static_cast<std::uint16_t>(o.vbus_at_psrdy_mv);
        if (up) meas(Check::PsRdyLow, {o.vbus_at_psrdy_mv, 0, band_lo(to_mv), 0, a, b});
        else meas(Check::PsRdyEarly, {o.vbus_at_psrdy_mv, 0, 0, band_hi(to_mv), a, b});
    }
    if (e.accept && captured_) {
        cur_.peak_mv = static_cast<std::uint16_t>(o.peak_mv < 0 ? 0 : o.peak_mv);
        if (up) meas(Check::Overshoot, {o.peak_mv, 0, -kOpenLimit, band_hi(to_mv) + std::int32_t(pd::spec::kVSrcValidExtraMv), a, b});
        else meas(Check::Overshoot, {o.peak_mv, 0, band_lo(to_mv) - std::int32_t(pd::spec::kVSrcValidExtraMv), kOpenLimit, a, b});
    }
    const Check tc = up ? Check::RiseTime : Check::FallTime;
    const std::int32_t lim = std::int32_t((epr ? pd::spec::kInBandEprMs : pd::spec::kInBandSprMs) * 1000);
    if (o.band.seen) {
        meas(tc, {std::int32_t(o.band.us), std::int32_t(o.band.gap_us), 0, lim, a, b});
        cur_.band_ms = static_cast<std::uint16_t>(o.band.us / 1000);
        if (o.final_valid) {
            meas(Check::Accuracy, {o.final_mv, 0, band_lo(to_mv), band_hi(to_mv), a, b});
            cur_.final_mv = static_cast<std::uint16_t>(o.final_mv);
        }
        push_tr(cur_);
        next_item(now);
        return;
    }
    // Not in the band by the end of the capture (an unloaded fall can take seconds): follow it live, at poll resolution.
    live_t_ = now;
    live_n_ = 0;
    live_first_ = true;
    ph_ = Ph::LiveBand;
    phase_t_ = now;
}

inline void Engine::live_band(const Snapshot& s, std::uint32_t now) {
    const std::int32_t to_mv = pdo(cur_.to).max_mv;
    const bool up = to_mv > from_mv_, epr = (cur_.flags & kTrEpr) != 0;
    const Check tc = up ? Check::RiseTime : Check::FallTime;
    const std::int32_t lim = std::int32_t((epr ? pd::spec::kInBandEprMs : pd::spec::kInBandSprMs) * 1000);
    const std::uint32_t acc = s.accept_at;
    const bool first = live_first_;
    live_first_ = false;
    if (in_band(s.vbus_mv, to_mv)) {
        // Already in the band at the first look (no capture to show the entry): the entry was somewhere after the Accept, so the
        // value is only an upper end and the whole of it is the bracket (a pass is sure, a fail cannot happen).
        if (live_n_++ == 0) { live_t_ = now; live_unseen_ = first; }
        if (live_n_ >= kLiveBandPolls) {
            const std::uint32_t ms = live_t_ - acc;
            meas(tc, {std::int32_t(ms * 1000), std::int32_t((live_unseen_ ? ms : poll_gap_ + kVbusPeriodMs) * 1000), 0, lim, cur_.from, cur_.to});
            meas(Check::Accuracy, {s.vbus_mv, 0, band_lo(to_mv), band_hi(to_mv), cur_.from, cur_.to});
            cur_.band_ms = static_cast<std::uint16_t>(ms > 0xFFFE ? 0xFFFE : ms);
            cur_.final_mv = static_cast<std::uint16_t>(s.vbus_mv);
            cur_.flags |= kTrLiveBand;
            push_tr(cur_);
            next_item(now);
        }
        return;
    }
    live_n_ = 0;
    if (now - phase_t_ > kLiveBandMaxMs) {
        meas(tc, {kNever, 0, 0, lim, cur_.from, cur_.to});
        cur_.flags |= kTrLiveBand;
        push_tr(cur_);
        env_->on_note("level not reached");
        next_item(now);
    }
}

// ---- programmable sweep (PPS and SPR AVS) ----
// Per window (plan.hpp Prog) the classes 0..3 in order, each a Sweep from the window's minimum up and back to it. A measured move is
// captured and judged (judge_prog); a setup move (between the 20 mV windows) is a plain go() + settle. A PPS pass asks Get_PPS_Status at
// its top (before the first move down) and at its end.

inline const Prog& Engine::prog() const { return L_.prog[w_]; }

inline void Engine::pass_begin(std::uint32_t now) {
    const Prog& p = prog();
    sw_ = make_sweep(p, cls_, full20_);
    have_pend_ = false;
    dir_ = 0;
    asked_top_ = false;
    PassRec r;
    r.pos = p.pos;
    r.cls = static_cast<std::uint8_t>(cls_ | (p.kind == ProgKind::Avs ? kPassAvs : 0));
    r.lo_mv = static_cast<std::uint16_t>(p.lo);
    r.hi_mv = static_cast<std::uint16_t>(p.hi);
    r.min_delta_mv = 0x7FFF;
    r.over_mv = -0x7FFF;
    if (std::uint8_t* m = pass_raw()) ::new (m) PassRec(r);
    prog_next(now);
}

inline void Engine::prog_next(std::uint32_t now) {
    const bool pps = prog().kind == ProgKind::Pps;
    Move m;
    if (have_pend_) {
        m = pend_;
        have_pend_ = false;
    } else if (!sw_.next(m)) {
        if (pps && !asked_end_) { asked_end_ = true; status_begin(now); return; }
        asked_end_ = false;
        pass_end(now);
        return;
    }
    if (!m.measured) { go(prog().pos, static_cast<std::uint16_t>(m.mv), After::PpsNext, now); return; }
    const std::int8_t d = m.mv > cur_mv_ ? 1 : -1;
    if (pps && dir_ == 1 && d == -1 && !asked_top_) {   // the top of the pass: ask before the first move down
        asked_top_ = true;
        pend_ = m;
        have_pend_ = true;
        status_begin(now);
        return;
    }
    dir_ = d;
    const Snapshot s = env_->snapshot();
    from_us_ = s.now_us;
    from_mv_ = cur_mv_;
    tgt_pos_ = prog().pos;
    tgt_mv_ = static_cast<std::uint16_t>(m.mv);
    cur_mv_ = m.mv;
    const std::int32_t step = std::abs(tgt_mv_ - from_mv_);
    captured_ = env_->capture_start(large_step(step) ? kPostLargeUs : kPostStepUs);
    if (!captured_) for (const Check c : prog_checks_meter()) skip(c, Skip::NoCapture);
    allowed_mv_ = from_mv_ > tgt_mv_ ? from_mv_ : tgt_mv_;
    phase_t_ = now;
    sent_ = false;
    ph_ = Ph::Measure;
}

inline bool Engine::large_step(std::int32_t step_mv) const {
    return prog().kind == ProgKind::Avs ? step_mv > std::int32_t(pd::spec::kAvsSmallStepMv) : step_mv > std::int32_t(pd::spec::kPpsSmallStepMv);
}

inline void Engine::pass_end(std::uint32_t now) {
    if (pass_raw()) ++n_pass_;
    ++item_;
    if (item_ < items_) env_->on_item(fam_, item_, items_);   // the next pass's number; none after the last
    if (++cls_ < kClasses) { pass_begin(now); return; }   // the pass ended at the window's minimum: the next class starts there
    cls_ = 0;
    if (++w_ < L_.n_prog) { go(prog().pos, static_cast<std::uint16_t>(prog().lo), After::PpsSteps, now); return; }
    restore(now);
}

// The checks the meter feeds (a busy capture skips them).
inline std::array<Check, 6> Engine::prog_checks_meter() const {
    if (prog().kind == ProgKind::Avs) return {Check::AvsMonotonic, Check::AvsAccuracy, Check::AvsBand, Check::AvsOvershoot, Check::AvsTransient, Check::AvsTrack};
    return {Check::PpsMonotonic, Check::PpsAccuracy, cls_ == 0 ? Check::PpsSettle : Check::PpsBand, Check::PpsOvershoot, Check::PpsTransient, Check::PpsTrack};
}

inline void Engine::judge_prog(const StepOut& o, const Edges& e, const Snapshot& s) {
    const bool avs = prog().kind == ProgKind::Avs;
    const std::int32_t target = tgt_mv_, from = from_mv_;
    const bool up = target > from;
    const std::int32_t step = std::abs(target - from);
    const bool large = large_step(step);
    const std::int32_t lim_ms = std::int32_t(avs ? (large ? pd::spec::kAvsLargeStepMs : pd::spec::kAvsSmallStepMs)
                                                 : (large ? pd::spec::kPpsLargeStepMs : pd::spec::kPpsSmallStepMs));
    const auto a = static_cast<std::uint8_t>(tgt_pos_ | cls_ << 4 | (up ? 0 : 0x80));
    const auto b = static_cast<std::uint8_t>(target / 100);
    cur_ = {};
    PassRec scratch;
    PassRec* slot = pass_slot();
    PassRec& r = slot ? *slot : scratch;
    // PS_RDY is a received message: its limit carries the wire allowance (pd_spec_timing.hpp kPsRdyWireOverheadMs); the band does not.
    const std::int32_t rdy_ms = lim_ms + std::int32_t(pd::spec::kPsRdyWireOverheadMs);
    const Lat l = latencies(e, s, avs ? Check::AvsPsRdy : Check::PpsPsRdy, rdy_ms * 1000, a, b);
    if (l.acc) {
        const std::uint32_t t = l.rdy ? l.rdy_us / 100 : 0xFFFF;
        if (t > r.psrdy_100us) r.psrdy_100us = static_cast<std::uint16_t>(t > 0xFFFF ? 0xFFFF : t);
        if (!l.rdy || l.rdy_us > std::uint32_t(rdy_ms) * 1000) bump_sat(r.psrdy_fails);
    }
    if (!o.valid) return;
    ++r.steps;
    const auto sat = [](std::uint32_t us) { return static_cast<std::uint16_t>(us / 100 > 0xFFFE ? 0xFFFE : us / 100); };
    // In the band (classes 1..3) / on the final level (the 20 mV class: unverified, the meter's floor).
    if (!avs && cls_ == 0) {
        const std::int32_t settle = o.settled ? std::int32_t(o.settle.us) : kNever;
        meas(Check::PpsSettle, {settle, std::int32_t(o.settle.gap_us), 0, std::int32_t(pd::spec::kPpsSmallStepMs * 1000), a, b});
        const std::uint16_t t = o.settled ? sat(o.settle.us) : 0xFFFF;
        if (t > r.band_100us) r.band_100us = t;
        if (!pdspec::passes(Kind::Max, {settle, std::int32_t(o.settle.gap_us), 0, std::int32_t(pd::spec::kPpsSmallStepMs * 1000)})) bump_sat(r.band_fails);
    } else {
        const Meas m{o.band.seen ? std::int32_t(o.band.us) : kNever, std::int32_t(o.band.gap_us), 0, lim_ms * 1000, a, b};
        meas(avs ? Check::AvsBand : Check::PpsBand, m);
        const std::uint16_t t = o.band.seen ? sat(o.band.us) : 0xFFFF;
        if (t > r.band_100us) r.band_100us = t;
        if (!pdspec::passes(Kind::Max, m)) bump_sat(r.band_fails);
    }
    // Overshoot / undershoot beyond vPpsNew + vPpsValid (vAvsNew + vAvsValid), on the side the step moves to.
    const std::int32_t extra = avs ? pd::spec::kAvsValidExtraMv : pd::spec::kPpsValidExtraMv;
    const std::int32_t over = up ? o.peak_mv - band_hi(target) : band_lo(target) - o.peak_mv;
    if (up) meas(avs ? Check::AvsOvershoot : Check::PpsOvershoot, {o.peak_mv, 0, -kOpenLimit, band_hi(target) + extra, a, b});
    else meas(avs ? Check::AvsOvershoot : Check::PpsOvershoot, {o.peak_mv, 0, band_lo(target) - extra, kOpenLimit, a, b});
    if (over > r.over_mv) r.over_mv = static_cast<std::int16_t>(over > 0x7FFF ? 0x7FFF : over);
    if (over > extra) bump_sat(r.over_fails);
    // How long VBUS left the band again after entering it (tPpsTransient / tAvsTransient); one sample of bracket.
    if (o.band.seen) {
        const std::int32_t tr_ms = std::int32_t(avs ? pd::spec::kAvsTransientMs : pd::spec::kPpsTransientMs);
        const Meas mt{std::int32_t(o.out_us), o.out_us ? std::int32_t(o.band.gap_us) : 0, 0, tr_ms * 1000, a, b};
        meas(avs ? Check::AvsTransient : Check::PpsTransient, mt);
        if (sat(o.out_us) > r.out_100us) r.out_100us = sat(o.out_us);
        if (!pdspec::passes(Kind::Max, mt)) bump_sat(r.out_fails);
    }
    // The level is known when VBUS ended inside the band or sat still on its final level (an offset source); a level still moving at
    // the window's end (an unloaded fall) has none to judge.
    if (!o.band.seen && !o.settled) return;
    // The settled level inside vPpsNew / vAvsNew, and the move the right way (or not at all).
    const std::int32_t tol = target * std::int32_t(avs ? pd::spec::kAvsNewPermille : pd::spec::kPpsNewPermille) / 1000;
    const std::int32_t err = o.final_mv - target;
    meas(avs ? Check::AvsAccuracy : Check::PpsAccuracy, {o.final_mv, 0, target - tol, target + tol, a, b});
    if (std::abs(err) > tol) bump_sat(r.acc_fails);
    if (std::abs(err) > std::abs(r.worst_err_mv)) r.worst_err_mv = static_cast<std::int16_t>(err);
    if (prev_valid_) {
        const std::int32_t delta = up ? o.final_mv - prev_mv_ : prev_mv_ - o.final_mv;
        meas(avs ? Check::AvsMonotonic : Check::PpsMonotonic, {delta, 0, -kMonoAllowMv, 0, a, b});
        if (delta < -kMonoAllowMv) bump_sat(r.mono_fails);
        if (delta < r.min_delta_mv) r.min_delta_mv = static_cast<std::int16_t>(delta);
        // The finding of pd/prog_track.hpp on an upward step (the slams judge the two ends of a window, not a sweep): the first one of
        // a pass only - after a fold every later step sits on the plateau. The value is how far the output fell behind the request.
        if (up && cls_ < 3 && !r.track) {
            const std::int32_t behind = pd::track_behind(from, prev_mv_, target, o.final_mv), limit = pd::track_limit(step);
            meas(avs ? Check::AvsTrack : Check::PpsTrack, {behind, 0, 0, limit, a, b});
            if (behind > limit) r.track = 1;
        }
    }
    prev_mv_ = o.final_mv;
    prev_valid_ = true;
}

// Get_PPS_Status (PPS only, never in the keep-alive family): asked through the sink, the answer judged against the no-load
// expectations (PD CTS SRC3.7#2) and timed against tReceiverResponse.
inline void Engine::status_begin(std::uint32_t now) {
    st_sent_ = false;
    ph_ = Ph::Status;
    phase_t_ = now;
}

inline void Engine::status_poll(const Snapshot& s, std::uint32_t now) {
    if (!st_sent_) {
        const pd::Err e = env_->ask_pps_status();
        if (e == pd::Err::Ok) { st_sent_ = true; st_seq0_ = s.pps_seq; return; }
        if (now - phase_t_ > kBusyMs) { env_->on_note("PPS status not asked"); prog_next(now); }   // our own guards held it: no verdict
        return;
    }
    const bool done = s.pps_seq != st_seq0_ && s.pps_ask != pd::PpsAsk::Pending;
    if (!done) {
        if (now - phase_t_ > kStepMaxMs) finish(Stop::Stuck, now);
        return;
    }
    const std::int32_t target = cur_mv_;
    const auto a = static_cast<std::uint8_t>(tgt_pos_ | cls_ << 4 | (asked_end_ ? 0x80 : 0));
    const auto b = static_cast<std::uint8_t>(target / 100);
    const std::int32_t bits = pps_status_faults(s, target);
    meas(Check::PpsStatus, {bits, 0, 0, 0, a, b});
    if (PassRec* p = pass_slot()) p->status = static_cast<std::uint8_t>(p->status | bits);
    if (s.pps_ask != pd::PpsAsk::Timeout && (s.pps_ask_us & 1) && (s.pps_rx_us & 1) && s.pps_rx_us - s.pps_ask_us < 1000000)
        meas(Check::PpsStatusTime, {std::int32_t(s.pps_rx_us - s.pps_ask_us), 0, 0, std::int32_t(pd::spec::kPpsStatusLimitMs * 1000), a, b});
    prog_next(now);
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
inline std::int32_t Engine::pps_status_faults(const Snapshot& s, std::int32_t target_mv) {
    return pdspec::pps_status_faults(s.pps_ask, s.pps_mv_raw, s.pps_ma_raw, s.pps_flags, target_mv);
}

inline void Engine::next_item(std::uint32_t now) {
    if (fam_ != Family::Transitions) return;
    ++item_;
    if (item_ >= items_) { restore(now); return; }
    env_->on_item(fam_, item_, items_);
    measure_begin(now);
}

// ---- timers ----

inline void Engine::track_gap(std::uint32_t now) {
    if (last_poll_ && now - last_poll_ > poll_gap_) poll_gap_ = now - last_poll_;
    last_poll_ = now;
    if (poll_gap_ > tm_.poll_gap_ms) tm_.poll_gap_ms = static_cast<std::uint16_t>(poll_gap_ > 0xFFFF ? 0xFFFF : poll_gap_);
}

// When the source's PPS timer started: the GoodCRC of the last Request it accepted. The sink stamps the Accept in ms; with real edge
// stamps the Request went out (accept_us - req_us) before it (its GoodCRC follows the TX start by under 1 ms), else the Accept stamp.
inline std::uint32_t Engine::request_origin(const Snapshot& s) const {
    if (!s.accept_at) return 0;
    if ((s.req_us & 1) && (s.accept_us & 1) && s.accept_us - s.req_us < 1000000) return s.accept_at - (s.accept_us - s.req_us) / 1000;
    return s.accept_at;
}

inline void Engine::ka_wait(const Snapshot& s, std::uint32_t now) {
    const bool pps = fam_ == Family::PpsKeepAlive;
    const Check c = pps ? Check::PpsKeepAlive : Check::EprKeepAlive;
    const std::int32_t lo = std::int32_t(pps ? pd::spec::kPpsTimeoutMinMs : pd::spec::kSourceEprKeepAliveMinMs);
    const std::int32_t hi = std::int32_t(pps ? pd::spec::kPpsTimeoutMaxMs : pd::spec::kSourceEprKeepAliveMaxMs);
    // The origin follows the sink's own stamp should one more keep-alive have gone out as the silence began.
    const std::uint32_t origin = pps ? request_origin(s) : s.epr_ka_at;
    if (s.attached && origin && static_cast<std::int32_t>(origin - ka_t0_) > 0) { ka_t0_ = origin; ka_extra_ = 0; }
    // Without a stamp the last keep-alive went out up to one period before the silence: the value is the upper end, gap the bracket.
    const std::int32_t extra = std::int32_t(ka_extra_);
    const std::int32_t gap = std::int32_t(poll_gap_ + kVbusPeriodMs) + extra;
    if (s.source_hard_resets != hr0_ || (!s.attached && s.vbus_mv < pd::spec::kVSafe0VMaxMv)) {
        hr0_ = s.source_hard_resets;
        const std::int32_t ms = std::int32_t(now - ka_t0_) + extra;
        meas(c, {ms, gap, lo, hi, tgt_pos_, 0});
        if (pps) tm_.pps_ka_ms = std::uint32_t(ms); else tm_.epr_ka_ms = std::uint32_t(ms);
        env_->keepalive_silence(Silence::Off);
        silenced_ = false;
        after_ = After::Family;
        ph_ = Ph::Recover;
        phase_t_ = now;
        asked_ = false;
        return;
    }
    if (now - ka_t0_ > (pps ? kPpsKaWaitMs : kEprKaWaitMs)) {
        meas(c, {kNever, 0, lo, hi, tgt_pos_, 0});   // no reset at all: the source never timed out
        env_->keepalive_silence(Silence::Off);
        silenced_ = false;
        env_->on_note("no keep-alive timeout");
        restore(now);
    }
}

inline void Engine::reset_watch(const Snapshot& s, std::uint32_t now) {
    track_.feed(now, s.vbus_mv, s.caps_rx != caps0_);
    const ResetTimes& t = track_.times();
    const bool done = t.caps_seen || now - phase_t_ > kResetWatchMs;
    if (!done) return;
    const std::int32_t g = std::int32_t(t.gap_ms + kVbusPeriodMs);
    const std::int32_t el = std::int32_t(now - phase_t_);
    meas(Check::HrSafe0V, {t.off_seen ? std::int32_t(t.off_ms) : el, g, 0, std::int32_t(pd::spec::kPsHardResetMaxMs + pd::spec::kSafe0VMs), 0, 0});
    if (t.off_seen) {
        tm_.hr_off_ms = static_cast<std::uint16_t>(t.off_ms);
        meas(Check::HrRecover, {t.up_seen ? std::int32_t(t.dark_ms) : el, g,
                                std::int32_t(hr_epr_ ? pd::spec::kSrcRecoverEprMinMs : pd::spec::kSrcRecoverMinMs),
                                std::int32_t(hr_epr_ ? pd::spec::kSrcRecoverEprMaxMs : pd::spec::kSrcRecoverMaxMs), 0, 0});
    }
    if (t.up_seen) {
        tm_.hr_dark_ms = static_cast<std::uint16_t>(t.dark_ms);
        meas(Check::HrTurnOn, {t.on_seen ? std::int32_t(t.turn_on_ms) : el, g, 0, std::int32_t(pd::spec::kSrcTurnOnMs), 0, 0});
    }
    if (t.on_seen) {
        tm_.hr_on_ms = static_cast<std::uint16_t>(t.turn_on_ms);
        meas(Check::HrCaps, {t.caps_seen ? std::int32_t(t.caps_ms) : el, g, 0, std::int32_t(pd::spec::kFirstSourceCapMs), 0, 0});
    }
    if (t.caps_seen) tm_.hr_caps_ms = static_cast<std::uint16_t>(t.caps_ms);
    after_ = After::Family;
    ph_ = Ph::Recover;
    phase_t_ = now;
    asked_ = false;
}

inline void Engine::detach_gap(const Snapshot& s, std::uint32_t now) {
    track_.feed(now, s.vbus_mv, false);
    const ResetTimes& t = track_.times();
    if (t.off_seen && off_at_ == 0) off_at_ = now ? now : 1;
    const bool done = (t.off_seen && now - off_at_ >= kDetachHoldMs) || now - phase_t_ >= kDetachMaxMs;
    if (!done) return;
    meas(Check::DetachSafe0V, {t.off_seen ? std::int32_t(t.off_ms) : std::int32_t(now - phase_t_), std::int32_t(t.gap_ms + kVbusPeriodMs), 0,
                               std::int32_t(pd::spec::kVbusOffMs + pd::spec::kSrcDisconnectMaxMs), 0, 0});
    if (t.off_seen) tm_.det_off_ms = static_cast<std::uint16_t>(t.off_ms);
    env_->rd_off(false);
    rd_off_ = false;
    rd_back_ = now;
    track_.start(now);   // the way back: VBUS up to vSafe5V, then the first capabilities
    caps0_ = s.caps_rx;
    after_ = After::Family;
    ph_ = Ph::Recover;
    phase_t_ = now;
    asked_ = false;
}

// Rd back -> VBUS at vSafe5V within tCCDebounce + tBulkDischarge + tVBUSON, and the first Source_Capabilities within tFirstSourceCap of
// it (Type-C R2.5 Tables 4-32 / 4-34, PD R3.3 Table 7.10). At poll resolution; what never came is kNever.
inline void Engine::reattach_judge(std::uint32_t now) {
    const ResetTimes& t = track_.times();
    // VBUS never left after the detach (the source kept it up): there is no way back to time (detach_safe0v has the verdict).
    if (!t.off_seen) { skip(Check::Reattach, Skip::NoData); skip(Check::ReattachCaps, Skip::NoData); return; }
    const std::int32_t g = std::int32_t(t.gap_ms + kVbusPeriodMs);
    const std::uint32_t bulk = tm_.det_from_mv > pd::spec::kBulkDischargeHvAboveMv ? pd::spec::kBulkDischargeHvMs : pd::spec::kBulkDischargeMs;
    const std::int32_t lim = std::int32_t(pd::spec::kCcDebounceMaxMs + bulk + pd::spec::kVbusOnMs);
    const std::int32_t up = t.on_seen ? std::int32_t(t.off_ms + t.dark_ms + t.turn_on_ms) : kNever;
    meas(Check::Reattach, {up, t.on_seen ? g : 0, 0, lim, 0, 0});
    if (t.on_seen) meas(Check::ReattachCaps, {t.caps_seen ? std::int32_t(t.caps_ms) : kNever, t.caps_seen ? g : 0, 0, std::int32_t(pd::spec::kFirstSourceCapMs), 0, 0});
    (void)now;
}

inline void Engine::recover(const Snapshot& s, std::uint32_t now) {
    const bool back = s.attached && s.state == pd::SinkState::Ready && s.pos == 1 && s.vbus_mv >= pd::spec::kVSafe5VMinMv;
    if (fam_ == Family::Detach && rd_back_) track_.feed(now, s.vbus_mv, s.caps_rx != caps0_);
    if (back) {
        if (fam_ == Family::Detach && rd_back_) {
            reattach_judge(now);
            tm_.det_back_ms = static_cast<std::uint16_t>(now - rd_back_ > 0xFFFE ? 0xFFFE : now - rd_back_);
            rd_back_ = 0;
        }
        hr0_ = s.source_hard_resets;
        next_family();
        return;
    }
    // Back with a contract that is not 5 V (the sink re-asked an older level), or with capabilities and no contract after a while: ask
    // for 5 V (once it is queued).
    const bool other = s.attached && s.state == pd::SinkState::Ready && s.pos != 1;
    const bool idle = s.attached && s.state == pd::SinkState::Idle && now - phase_t_ > kRecoverAskMs;
    if (!asked_ && (other || idle)) {
        pd::Choice c;
        c.pos = 1;
        asked_ = env_->request(c) == pd::Err::Ok;
    }
    if (now - phase_t_ > kRecoverMs) {
        if (fam_ == Family::Detach && rd_back_) reattach_judge(now);
        finish(Stop::NoRecovery, now);
    }
}

// ---- the end ----

inline void Engine::finish(Stop why, std::uint32_t now) {
    if (finishing_) return;
    finishing_ = true;
    stop_ = why;
    if (silenced_) { env_->keepalive_silence(Silence::Off); silenced_ = false; }
    if (rd_off_) { env_->rd_off(false); rd_off_ = false; }
    env_->capture_stop();
    // Checks with no measurement and no reason yet: a family left out of the run was not selected, the family in flight was stopped,
    // the later ones were not reached, an earlier one that ran without a single value for it (no down-step got its band, say) had no
    // data.
    for (std::size_t f = 0; f < kFamilies; ++f) {
        const auto cur = static_cast<std::size_t>(fam_);
        const bool picked = (mask_ >> f & 1) != 0;
        skip_family(Family(f), !picked ? Skip::NotSelected : f < cur ? Skip::NoData : f == cur ? Skip::Stopped : Skip::NotReached);
    }
    meas(Check::SourceReset, {std::int32_t(src_resets_), 0, 0, 0, 0, 0});
    env_->release();
    ph_ = Ph::Finishing;
    phase_t_ = now;
}

inline bool Engine::guards(const Snapshot& s, std::uint32_t now) {
    if (cancel_) { finish(Stop::Cancelled, now); return false; }
    if (now - t0_ > kMaxRunMs) { finish(Stop::Timeout, now); return false; }
    if (s.inline_link) { finish(Stop::Inline, now); return false; }
    // Over the monitor ceiling + 5 % at once; over the step's own highest level + 10 % + 1 V for kOvPolls polls in a row (an overshoot
    // spike is a measurement, not a reason to stop).
    if (s.vbus_mv > std::int32_t(pd::overcap_abort_mv(env_->cap_mv()))) { finish(Stop::Overvoltage, now); return false; }
    const std::int32_t allowed = (ph_ == Ph::Settle || ph_ == Ph::Goto || ph_ == Ph::Measure || ph_ == Ph::Frozen || ph_ == Ph::LiveBand)
                                     ? allowed_mv_ : top_mv_;
    if (s.vbus_mv > allowed + allowed / 10 + 1000) {
        if (++ov_polls_ >= kOvPolls) { finish(Stop::Overvoltage, now); return false; }
    } else {
        ov_polls_ = 0;
    }
    if (std::abs(s.ibus_ma) > kLoadAbortMa) {
        if (++load_polls_ >= kLoadPolls) { finish(Stop::Load, now); return false; }
    } else {
        load_polls_ = 0;
    }
    // A Hard Reset from the source outside a keep-alive wait: none was due. Counted, the family ends, wait for 5 V.
    if (s.source_hard_resets != hr0_ && ph_ != Ph::KaWait) {
        hr0_ = s.source_hard_resets;
        ++src_resets_;
        if (ph_ != Ph::Recover) { lost(s, now); return false; }
    }
    return true;
}

inline void Engine::poll(std::uint32_t now) {
    if (!running_) return;
    const Snapshot s = env_->snapshot();
    if (ph_ == Ph::Finishing) {
        // Done at 5 V: a PDO 1 contract, or a source with no contract of ours (it holds vSafe5V). A dark line (a stop inside a reset
        // or the detach gap) must stay dark kDarkFinalMs: a source coming back starts at vSafe5V and is then confirmed as above.
        const bool low = s.vbus_mv <= kRestoreMaxMv;
        const bool at5 = s.attached && low && s.vbus_mv >= std::int32_t(pd::spec::kVSafe0VMaxMv) &&
                         ((s.state == pd::SinkState::Ready && s.pos == 1) || s.state == pd::SinkState::Idle);
        if (!s.attached && s.vbus_mv < pd::spec::kVSafe0VMaxMv) {
            if (!dark_since_) dark_since_ = now ? now : 1;
        } else {
            dark_since_ = 0;
        }
        const bool gone = dark_since_ && now - dark_since_ >= kDarkFinalMs;
        if (at5 || gone || now - phase_t_ > kReleaseMs) {
            if (!at5 && !gone) release_unconfirmed_ = true;
            running_ = false;
            end_ms_ = now;
            ph_ = Ph::Done;
            env_->on_finish(stop_);
        }
        return;
    }
    if (!guards(s, now)) return;
    track_gap(now);   // the run's longest poll gap: every live-timed value carries it as its uncertainty
    if (s.state != pd::SinkState::Ready) left_ready_ = true;
    const bool expect_dark = ph_ == Ph::KaWait || ph_ == Ph::ResetWatch || ph_ == Ph::DetachGap || ph_ == Ph::Recover;
    if (!s.attached && !expect_dark) { lost(s, now); return; }
    switch (ph_) {
        case Ph::FamStart: begin_family(now); return;
        case Ph::Goto: {
            if (!sent_) { (void)send(now); return; }
            const int o = outcome(s);
            if (o == 0) { if (now - send_t0_ > kStepMaxMs) finish(Stop::Stuck, now); return; }
            if (o == -1) { refused(now); return; }
            if (o == -2) { lost(s, now); return; }
            ph_ = Ph::Settle;
            phase_t_ = now;
            band_since_ = 0;
            ema_mv_ = s.vbus_mv;
            return;
        }
        case Ph::Settle:
            ema_mv_ += (s.vbus_mv - ema_mv_) / 4;
            if (settled(s)) {
                if (!band_since_) band_since_ = now ? now : 1;
                if (now - band_since_ >= kSettleQuietMs) after(s, now);
                return;
            }
            band_since_ = 0;
            if (now - phase_t_ > kSettleMaxMs) { env_->on_note("level not settled"); after(s, now); }
            return;
        case Ph::Measure: {
            if (!sent_) { (void)send(now); return; }
            const int o = outcome(s);
            if (o == 0) { if (now - send_t0_ > kStepMaxMs) finish(Stop::Stuck, now); return; }
            if (o == -1) { refused(now); return; }
            if (o == -2) { lost(s, now); return; }
            ph_ = Ph::Frozen;
            phase_t_ = now;
            if (!captured_) measure_done(s, now);
            return;
        }
        case Ph::Frozen:
            if (env_->capture_frozen() || now - phase_t_ > kFrozenMaxMs) measure_done(s, now);
            return;
        case Ph::LiveBand: live_band(s, now); return;
        case Ph::Status: status_poll(s, now); return;
        case Ph::KaWait: ka_wait(s, now); return;
        case Ph::ResetWatch: reset_watch(s, now); return;
        case Ph::DetachGap: detach_gap(s, now); return;
        case Ph::Recover: recover(s, now); return;
        case Ph::Restore:
        case Ph::Finishing:
        case Ph::Done: return;
    }
}

}  // namespace k2::pdspec
