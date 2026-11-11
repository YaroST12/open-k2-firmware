// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Device twin, live proof: negotiate with the charger AS the device (host-tested in tests/test_twin_live.cpp). The prediction
// (k2/twin/predict.hpp) says what the device would get; this runner makes the charger show it. Pure logic over an Input (the
// sink status, VBUS, the charger's list, the clock) and an Env (the few things it may DO), like charger_test::Runner.
//
// ONE TWIN, STEP BY STEP
//  1. Plan.     predict() against the list the charger shows now. Nothing usable, or a level above the monitor ceiling
//               (pd::fits_monitor), ends the twin as Skipped before anything is touched.
//  2. Attach.   Env::impersonate(twin) points the sink's Get_Sink_Cap answer at the twin's PDOs (pd::Sink::set_sink_cap_override),
//               then the K2 attaches afresh (Rd off until VBUS is dark, Rd on; a self-powered K2 does a Soft_Reset instead, its
//               pinned Rd never opens), so the charger reads the sink capabilities of the twin and not of the K2.
//  3. Caps.     Wait for the charger's list and the automatic 5 V contract, then predict again on THAT list (a charger may list
//               other levels for another sink; the second prediction is the one that is requested).
//  4. Request.  The Request predict chose, built by pd::plan_request with hunt = false: never more current than advertised,
//               never above the monitor ceiling. Accept and PS_RDY decide Refused / NoPsRdy / Reset.
//  5. Hold.     VBUS must sit in the level's band, continuously, for kHoldMs (a fixed level is judged on the running mean
//               kBandTauMs of charger_test, burst-mode ripple is no failure; a PPS level and 5 V on the raw sample).
//               kBandWaitMs without it: OutOfBand. A sample above pd::overcap_abort_mv, or the engine's own overvoltage
//               abort, ends the twin as Unsafe with the release already asked.
//  6. Release.  sink_release(): back to 5 V before the next twin.
//  After the last twin the K2's own Sink_Capabilities come back (impersonate(nullptr)) and the K2 attaches once more so the
//  charger's picture of its sink is the K2's again. cancel() skips that last attach: it asks for 5 V, clears the impersonation
//  and ends; the charger keeps the twin's words until the next attach of any kind.
//
// WHAT CANNOT BE IMPERSONATED (reverse-engineered limits, kept honest in docs/DEVICE_TWIN.md)
//  - the PD revision of every message (the K2 answers in the revision the charger announced; the twin's own is stored but
//    not spoken), the Sink_Capabilities_Extended block (the K2's own SKEDB, the twin keeps none), and the RDO: plan_request
//    builds it without Capability Mismatch, whatever the device would set;
//  - VDMs: no Discover Identity as the device (identity impersonation is out, see the docs for what it would take);
//  - a charger that decides from D+/D-, or from a vendor protocol the device speaks, sees none of that.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/chargertest/charger_test.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/twin/library.hpp"
#include "k2/twin/predict.hpp"
#include "k2/twin/twin.hpp"

namespace k2::twin::live {

inline constexpr std::uint32_t kDarkMs = 10000, kDarkHoldMs = 300, kDetachQuickMs = 1500;   // the charger test's detach rule
inline constexpr std::uint32_t kCapsMs = 8000;      // attach -> the list and the 5 V contract
inline constexpr std::uint32_t kRequestMs = 5000;   // Request -> PS_RDY (the engine's own limits are shorter; this is the backstop)
inline constexpr std::uint32_t kHoldMs = 1000;      // in band for this long = granted
inline constexpr std::uint32_t kBandWaitMs = 4000;  // PS_RDY -> in band
inline constexpr std::uint32_t kReleaseMs = 8000;   // release -> back at 5 V

// Per twin: what became of it. None = not run (the run ended before it, or there was no run).
enum class Verdict : std::uint8_t {
    None, Granted, Refused, NoPsRdy, OutOfBand, NoCaps, Reset, Unsafe, Skipped, Cancelled
};
// Why a twin was Skipped: nothing was sent for it.
enum class Skip : std::uint8_t { None, NoPrediction, AboveCeiling, RequestRefused, NoTwin };
enum class Reason : std::uint8_t { Done, Cancelled, Busy };
// Why start() said no.
enum class Refusal : std::uint8_t { None, NeedConfirm, Busy, Inline, NoMonitor, NoSource, NoTwins };

constexpr const char* name(Verdict v) {
    switch (v) {
        case Verdict::None: return "not run";
        case Verdict::Granted: return "granted";
        case Verdict::Refused: return "refused";
        case Verdict::NoPsRdy: return "no PS_RDY";
        case Verdict::OutOfBand: return "out of band";
        case Verdict::NoCaps: return "no list";
        case Verdict::Reset: return "link reset";
        case Verdict::Unsafe: return "overvoltage";
        case Verdict::Skipped: return "skipped";
        case Verdict::Cancelled: return "cancelled";
    }
    return "?";
}
constexpr const char* name(Skip s) {
    switch (s) {
        case Skip::None: return "";
        case Skip::NoPrediction: return "nothing usable";
        case Skip::AboveCeiling: return "above the monitor ceiling";
        case Skip::RequestRefused: return "request not buildable";
        case Skip::NoTwin: return "twin unreadable";
    }
    return "?";
}
constexpr const char* name(Refusal r) {
    switch (r) {
        case Refusal::None: return "ok";
        case Refusal::NeedConfirm: return "confirm needed";
        case Refusal::Busy: return "busy";
        case Refusal::Inline: return "link inline";
        case Refusal::NoMonitor: return "no power monitor";
        case Refusal::NoSource: return "no charger";
        case Refusal::NoTwins: return "no twins";
    }
    return "?";
}
constexpr const char* name(Reason r) {
    switch (r) {
        case Reason::Done: return "done";
        case Reason::Cancelled: return "cancelled";
        case Reason::Busy: return "ports taken";
    }
    return "?";
}

// What one twin's run left (4 bytes; the table is indexed by the twin's number).
struct Slot {
    Verdict verdict = Verdict::None;
    Skip skip = Skip::None;
    std::uint16_t measured_mv = 0;   // the VBUS level the verdict rests on (0 = none measured)
};

struct Input {
    std::uint32_t now = 0;
    int vbus_mv = -1;                         // power::vbus_fast_mv(), -1 unknown
    const pd::SourceCaps* caps = nullptr;     // the charger's list as it is now
    std::uint32_t caps_seq = 0;               // fusb302::caps_count(): moves with every Source_Capabilities
    pd::SinkStatus sink{};
    Facts facts{};                            // the source's power claim and the cable in the path
    std::uint32_t cap_mv = 0;                 // the monitor ceiling (pd::request_cap_mv)
    bool self_powered = false;
    bool busy = false;                        // another tool owns the ports (charger test, FC release)
    bool inline_link = false;                 // a real sink sits behind the K2
};

class Env {
public:
    virtual bool load(std::uint8_t seq, Twin& out) = 0;
    // The sink answers Get_Sink_Cap with this twin's PDOs until the next call; nullptr = the K2's own. The Env keeps the words
    // (pd::Sink does not copy them).
    virtual void impersonate(const Twin* t) = 0;
    virtual void detach() = 0;                // Rd off, 5 V default, VBUS dark (charger test unit_detach)
    virtual void attach() = 0;                // Rd on as a fresh PD attach (unit_attach(false))
    virtual void hold() = 0;                  // a self-powered K2's boundary: Soft_Reset, Rd kept (unit_hold(false))
    virtual pd::Err request(pd::Choice c) = 0;
    virtual void release() = 0;               // back to 5 V
    virtual void record(std::uint8_t seq, const Slot& s) = 0;   // a twin ended
    virtual void finished(Reason) {}
protected:
    ~Env() = default;
};

class Runner {
public:
    // Starts a run over `seqs` (twin numbers, in this order, at most kCapacity). Needs the confirm: the run changes the contract.
    Refusal start(Env& env, std::span<const std::uint8_t> seqs, const Input& in, bool confirmed) {
        if (running()) return Refusal::Busy;
        if (seqs.empty()) return Refusal::NoTwins;
        if (in.busy) return Refusal::Busy;
        if (in.inline_link) return Refusal::Inline;
        if (in.vbus_mv < 0 || in.cap_mv < 5000) return Refusal::NoMonitor;
        if (in.vbus_mv < 1000 || in.caps == nullptr || !in.caps->valid) return Refusal::NoSource;
        if (!confirmed) return Refusal::NeedConfirm;
        env_ = &env;
        slots_ = {};
        n_ = static_cast<std::uint8_t>(std::min<std::size_t>(seqs.size(), order_.size()));
        for (std::size_t i = 0; i < n_; ++i) order_[i] = seqs[i];
        idx_ = 0;
        never_dark_ = false;
        detached_ = false;
        restoring_ = false;
        impersonating_ = false;
        next_twin(in);
        return Refusal::None;
    }

    bool running() const { return phase_ != Phase::Idle && phase_ != Phase::Done; }
    const Slot& slot(std::uint8_t seq) const { return slots_[seq <= kCapacity ? seq : 0]; }
    std::uint8_t current() const { return running() && idx_ < n_ ? order_[idx_] : 0; }   // the twin being proven (0 = none)
    std::uint8_t count() const { return n_; }
    // Position in the run, for the screen: twins finished and in total.
    std::uint8_t done_count() const { return std::min<std::uint8_t>(idx_, n_); }
    Reason reason() const { return reason_; }
    bool restoring() const { return restoring_ && running(); }

    // Stops now: 5 V asked when a level above it may be held, Rd back when the K2 was detached, the K2's own caps restored.
    void cancel(Reason why = Reason::Cancelled) {
        if (!running()) return;
        if (idx_ < n_ && !restoring_ && slots_[order_[idx_]].verdict == Verdict::None) {   // a twin already judged keeps its verdict
            slots_[order_[idx_]].verdict = Verdict::Cancelled;
            env_->record(order_[idx_], slots_[order_[idx_]]);
        }
        if (phase_ == Phase::Request || phase_ == Phase::Hold) env_->release();
        else if (phase_ == Phase::Dark) env_->attach();
        end(why);
    }

    void poll(const Input& in) {
        if (!running()) return;
        if (in.busy || in.inline_link) { cancel(Reason::Busy); return; }
        switch (phase_) {
            case Phase::Dark: poll_dark(in); break;
            case Phase::Caps: poll_caps(in); break;
            case Phase::Request: poll_request(in); break;
            case Phase::Hold: poll_hold(in); break;
            case Phase::Release: poll_release(in); break;
            case Phase::Idle:
            case Phase::Done: break;
        }
    }

private:
    enum class Phase : std::uint8_t { Idle, Dark, Caps, Request, Hold, Release, Done };

    // The K2's own Sink_Capabilities back, only when a twin's were ever put in place (a run that skips every twin touches nothing).
    void clear_impersonation() {
        if (!impersonating_) return;
        impersonating_ = false;
        env_->impersonate(nullptr);
    }

    void end(Reason why) {
        clear_impersonation();
        phase_ = Phase::Done;
        reason_ = why;
        env_->finished(why);
    }

    // Judge a twin: record it, then release when a level above 5 V was requested, else on to the next.
    void finish_twin(const Input& in, Verdict v, std::uint16_t measured_mv, Skip skip = Skip::None) {
        Slot& s = slots_[order_[idx_]];
        s.verdict = v;
        s.skip = skip;
        s.measured_mv = measured_mv;
        env_->record(order_[idx_], s);
        if (requested_above_5v_) {
            env_->release();
            phase_ = Phase::Release;
            phase_at_ = in.now;
            return;
        }
        ++idx_;
        next_twin(in);
    }

    // The twin at idx_ (or the end): the plan on the list now, then impersonate and attach afresh.
    void next_twin(const Input& in) {
        requested_above_5v_ = false;
        while (idx_ < n_) {
            Twin tw;
            Slot& s = slots_[order_[idx_]];
            Outcome o;
            Skip why = Skip::None;
            if (!env_->load(order_[idx_], tw)) why = Skip::NoTwin;
            else if (!plan(tw, in, o, why)) {}
            if (why == Skip::None) {
                env_->impersonate(&tw);
                impersonating_ = true;
                begin_attach(in);
                return;
            }
            s.verdict = Verdict::Skipped;
            s.skip = why;
            env_->record(order_[idx_], s);
            ++idx_;
        }
        if (!restoring_ && detached_) {   // the K2's own caps back, and one more attach so the charger sees them
            restoring_ = true;
            clear_impersonation();
            begin_attach(in);
            return;
        }
        end(Reason::Done);
    }

    // predict() on the list in hand and the ceiling check. False with `why` set when the twin cannot run.
    bool plan(const Twin& tw, const Input& in, Outcome& o, Skip& why) const {
        why = Skip::None;
        if (in.caps == nullptr) { why = Skip::NoPrediction; return false; }
        o = predict(tw, *in.caps, in.facts);
        if (o.kind == Kind::None) { why = Skip::NoPrediction; return false; }
        if (!pd::fits_monitor(o.mv, in.cap_mv)) { why = Skip::AboveCeiling; return false; }
        return true;
    }

    void begin_attach(const Input& in) {
        detached_ = true;
        phase_at_ = in.now;
        dark_since_ = 0;
        if (in.self_powered) {   // no detach for the charger to answer with a VBUS cut
            env_->hold();
            to_caps(in);
            return;
        }
        env_->detach();
        phase_ = Phase::Dark;
    }

    void to_caps(const Input& in) {
        caps_seq0_ = in.caps_seq;
        phase_ = Phase::Caps;
        phase_at_ = in.now;
    }

    void poll_dark(const Input& in) {
        const std::uint32_t waited = in.now - phase_at_;
        if (in.vbus_mv >= 0 && in.vbus_mv < pd::kVSafe0Mv) {
            if (dark_since_ == 0) dark_since_ = in.now ? in.now : 1;
        } else {
            dark_since_ = 0;
        }
        const bool dark = dark_since_ != 0 && in.now - dark_since_ >= kDarkHoldMs;
        const bool gave_up = waited >= (never_dark_ ? kDetachQuickMs : kDarkMs);
        if (!dark && !gave_up) return;
        if (!dark) never_dark_ = true;   // a source that never lets go (a USB-A port): later detaches do not wait long
        env_->attach();
        to_caps(in);
    }

    void poll_caps(const Input& in) {
        const bool listed = in.caps_seq != caps_seq0_ && in.caps != nullptr && in.caps->valid;
        if (listed && in.sink.state == pd::SinkState::Ready && in.sink.pos == 1) {
            if (restoring_) { end(Reason::Done); return; }
            on_caps(in);
            return;
        }
        if (in.now - phase_at_ < kCapsMs) return;
        if (restoring_) { end(Reason::Done); return; }   // the K2 is back on its own words; no contract is not the run's verdict
        finish_twin(in, Verdict::NoCaps, 0);
    }

    // The charger's list for this sink, at 5 V: plan again on it and send the Request.
    void on_caps(const Input& in) {
        Twin tw;
        Outcome o;
        Skip why = Skip::None;
        if (!env_->load(order_[idx_], tw)) why = Skip::NoTwin;
        else plan(tw, in, o, why);
        if (why != Skip::None) { finish_twin(in, Verdict::Skipped, 0, why); return; }
        want_ = to_choice(o);
        kind_ = o.kind;
        target_mv_ = o.mv;
        from_mv_ = in.vbus_mv;
        band_.clear();
        in_since_ = 0;
        if (o.kind == Kind::FiveV) {   // the 5 V contract is already up: the proof is the charger accepting this sink at 5 V
            ready_at_ = in.now;
            phase_ = Phase::Hold;
            return;
        }
        const pd::Err e = env_->request(want_);
        if (e != pd::Err::Ok) {
            const Skip s = (e == pd::Err::OverCap || e == pd::Err::BadVoltage) ? Skip::AboveCeiling
                         : (e == pd::Err::Inline || e == pd::Err::Busy) ? Skip::None : Skip::RequestRefused;
            if (e == pd::Err::Inline || e == pd::Err::Busy) cancel(Reason::Busy);
            else finish_twin(in, Verdict::Skipped, 0, s);
            return;
        }
        requested_above_5v_ = true;
        phase_ = Phase::Request;
        phase_at_ = in.now;
    }

    void poll_request(const Input& in) {
        const pd::SinkStatus& s = in.sink;
        if (s.state == pd::SinkState::Ready && s.pos == want_.pos) {
            ready_at_ = in.now;
            band_.clear();
            in_since_ = 0;
            last_t_ = in.now;
            phase_ = Phase::Hold;
            return;
        }
        if (s.result == pd::SinkResult::Rejected) return finish_twin(in, Verdict::Refused, 0);
        if (s.result == pd::SinkResult::OverCap) return finish_twin(in, Verdict::Unsafe, clamp_mv(in.vbus_mv));
        if (s.result == pd::SinkResult::HardReset || s.result == pd::SinkResult::SoftReset || s.result == pd::SinkResult::Aborted)
            return finish_twin(in, Verdict::Reset, 0);
        if (s.result == pd::SinkResult::WaitExhausted || s.result == pd::SinkResult::Timeout || s.result == pd::SinkResult::TxFailed)
            return finish_twin(in, Verdict::NoPsRdy, 0);
        if (in.now - phase_at_ >= kRequestMs) finish_twin(in, Verdict::NoPsRdy, 0);
    }

    // The band of the level: 5 V = vSafe5V; a fixed level +-5 % (100 mV at least) on the running mean; a PPS level the same
    // margin below and, for a level reached from below, above; one asked from above gets the unloaded ceiling.
    bool in_band(int mv) const {
        if (kind_ == Kind::FiveV) return pd::is_vsafe5(mv);
        const int level = int(target_mv_);
        const int margin = std::max(100, level / 20);
        if (kind_ == Kind::Pps) {
            const bool down = from_mv_ > level;
            return mv >= level - margin && mv <= (down || from_mv_ <= 0 ? pd::programmable_ceiling_mv(level) : level + margin);
        }
        return std::abs(mv - level) <= margin;
    }

    void poll_hold(const Input& in) {
        const pd::SinkStatus& s = in.sink;
        const int v = in.vbus_mv;
        if (v >= 0 && std::uint32_t(v) > pd::overcap_abort_mv(in.cap_mv)) {
            env_->release();
            slots_[order_[idx_]].measured_mv = clamp_mv(v);
            return finish_twin_released(in, Verdict::Unsafe, clamp_mv(v));
        }
        if (kind_ != Kind::FiveV && (s.state != pd::SinkState::Ready || s.pos != want_.pos))
            return finish_twin(in, s.result == pd::SinkResult::OverCap ? Verdict::Unsafe : Verdict::Reset, clamp_mv(v));
        if (kind_ == Kind::FiveV && (s.state != pd::SinkState::Ready || s.pos != 1)) return finish_twin(in, Verdict::Reset, clamp_mv(v));
        // The judged level: a fixed level above 5 V on the running mean (burst-mode ripple), anything else on the sample.
        const std::uint32_t gap = in.now - last_t_;
        last_t_ = in.now;
        int level = v;
        if (kind_ == Kind::Fixed) {
            if (gap > 250) band_.clear();   // missing samples cannot be averaged across
            if (v < 0 || std::abs(v - int(target_mv_)) * 100 > int(target_mv_) * int(charger_test::kBandGrossPct)) {
                band_.clear();
                level = -1;
            } else {
                band_.add(v, gap);
                level = band_.mean();
            }
        }
        if (level >= 0 && in_band(level)) {
            if (in_since_ == 0) in_since_ = in.now ? in.now : 1;
            if (in.now - in_since_ >= kHoldMs) return finish_twin(in, Verdict::Granted, clamp_mv(level));
        } else {
            in_since_ = 0;
        }
        if (in.now - ready_at_ >= kBandWaitMs + kHoldMs) finish_twin(in, Verdict::OutOfBand, clamp_mv(v));
    }

    // The Unsafe path asked for 5 V itself: record, then wait for the release like any other.
    void finish_twin_released(const Input& in, Verdict v, std::uint16_t mv) {
        Slot& s = slots_[order_[idx_]];
        s.verdict = v;
        s.measured_mv = mv;
        env_->record(order_[idx_], s);
        phase_ = Phase::Release;
        phase_at_ = in.now;
    }

    void poll_release(const Input& in) {
        const bool back = in.sink.state == pd::SinkState::Ready && in.sink.pos == 1 && in.vbus_mv >= 0 && pd::is_vsafe5(in.vbus_mv);
        if (!back && in.now - phase_at_ < kReleaseMs) return;
        ++idx_;
        next_twin(in);
    }

    static std::uint16_t clamp_mv(int mv) { return static_cast<std::uint16_t>(std::clamp(mv, 0, 65535)); }

    Env* env_ = nullptr;
    std::array<Slot, kCapacity + 1> slots_{};
    std::array<std::uint8_t, kCapacity> order_{};
    std::uint8_t n_ = 0, idx_ = 0;
    Phase phase_ = Phase::Idle;
    Reason reason_ = Reason::Done;
    bool never_dark_ = false, detached_ = false, restoring_ = false, requested_above_5v_ = false, impersonating_ = false;
    std::uint32_t phase_at_ = 0, dark_since_ = 0, ready_at_ = 0, in_since_ = 0, last_t_ = 0, caps_seq0_ = 0;
    pd::Choice want_{};
    Kind kind_ = Kind::None;
    std::uint16_t target_mv_ = 0;
    int from_mv_ = 0;
    charger_test::VbusMean band_;
};

}  // namespace k2::twin::live
