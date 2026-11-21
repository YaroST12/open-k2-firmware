// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Fast-charge trigger engine (pure state machine, host-tested): drives one protocol at a time through
// an Env (clock, VBUS/D+/D- readings, D+/D- levels). It never blocks for more than one timed pulse
// list; every wait is a deadline checked in poll(), so BACK stays responsive.
//
// Safety rules enforced here, not by the callers:
//  - nothing starts without a VBUS ceiling (no power monitor => refused);
//  - a request above 5 V needs `confirmed == true`;
//  - requests are clamped to the ceiling (or refused when a discrete level exceeds it);
//  - every poll compares measured VBUS against the ceiling and target + 10 %, and aborts;
//  - abort and stop() send both data lines to 0 V (charger falls back to 5 V), then the idle state.
#pragma once
#include "k2/core/bss_zero.hpp"
#include <cstdint>
#include <span>

#include <algorithm>
#include <array>
#include "k2/fastcharge/accp.hpp"
#include "k2/fastcharge/fc_safety.hpp"
#include "k2/fastcharge/fc_seq.hpp"
#include "k2/pd/link_guard.hpp"
#include "k2/fastcharge/qc.hpp"
#include "k2/fastcharge/ufcs.hpp"
#include "k2/fastcharge/vivo.hpp"
#include "k2/fastcharge/vooc.hpp"

namespace k2::fc {

inline constexpr std::uint32_t kQc2TransitionMs = 12000;
// SCP early end: every setpoint write acked, yet VBUS has not moved toward a target at least kScpNoChangeStepMv away by
// kScpNoChangeMs after the last write. A passing 4.5 V row moved 0.5 V within 150 ms of the Vset ack (2026-11 capture), so 2 s is
// over ten times the slowest observed answer; the 100 mV is above the monitor's noise and below any slew that counts as following.
inline constexpr std::uint32_t kScpNoChangeMs = 2000;
// A kept SCP session (Engine::keep) nobody retargets within this long is released.
inline constexpr std::uint32_t kScpHoldMs = 4000;
inline constexpr int kScpNoChangeMv = 100, kScpNoChangeStepMv = 300;

enum class Proto : std::uint8_t { Qc2, Qc3, Qc3Plus, Afc, Fcp, Scp, Vooc, SuperVooc, SuperVooc65, Ufcs, Vivo, Count };
enum class State : std::uint8_t { Idle, Negotiating, Active, Failed };
enum class Fail : std::uint8_t {
    None, NeedConfirm, OverLimit, NoMonitor, Busy, Unsupported,
    NoHandshake, NoResponse, NoBand, Overvoltage, SourceLost, Aborted,
    Inline,     // a PD contract or a load is attached behind the K2 and nothing overrode the guard
    NoAnswer,   // VOOC: no frame heard after either wake (an all-ones frame counts as none). Wire value 13, once "needs load"
    NoVoltage,  // SVOOC "handshake ok": no longer produced (the K2 answers no VOOC command); kept for stored results
    Detected,   // SCP: every identifying read answered; VOOC family: the adapter clocked its ident and nothing was answered.
                // Not a failure: the row passes as "detected"
    NoChange,   // SCP: every setpoint write acked, VBUS did not move toward the target within kScpNoChangeMs
};

// VOOC wake lists (the fake chargers count wakes by these lengths): the phone's burst of the 2026-11-11 capture, D+ 40 x
// (3.3 V 300 us, 0 V 300 us); and the 13-period train of the 2026-11-02 work.
inline constexpr std::size_t kVoocBurstSteps = 80, kVoocTrainSteps = 13 * 4 + 3;

// The VOOC family's detection session (one session answers VOOC, SVOOC and SVOOC65): wake, listen, and when nothing was
// heard rest and wake again with the other wake list (burst, then train). The adapter clocked its first ident 539 ms
// after a wake in both captures (2026-11-11 phone burst and no-phone train), so a listen of 1.6 s has the ident in with
// over a second to spare; two tries cover both wake lists. A silent adapter ends the session after
// 2 x 1.6 s listen + 2.0 s rest = 5.2 s (plus the two wake plays, 24 ms and 48 ms, played before each listen), so the
// whole session still fits one clean window of a port that cuts VBUS at 5 V (charger test kCutStepBudgetMs, 5.5 s): a
// 2.0 s listen would not.
inline constexpr std::uint32_t kVoocWakeListenMs = 1600;   // no ident this long after a wake: rest and try again
inline constexpr std::uint32_t kVoocRestMs = 2000;         // lines idle between wakes
inline constexpr int kVoocTries = 2;                       // wakes before the adapter is called silent ("no answer")
inline constexpr std::uint32_t kVoocDetectMaxMs = kVoocTries * kVoocWakeListenMs + (kVoocTries - 1) * kVoocRestMs;   // 5200

// The HID-cable loop 5 V lock (Env::hv_locked): the highest target, and the highest VBUS a running session may show.
inline constexpr int kLockMv = 5000, kLockVbusMv = 5500;

const char* name(Proto p);
const char* name(State s);
const char* name(Fail f);
// The [FC] state line: a session that ends as Fail::Detected is a pass (the row reads "detected"), so its line says
// `VOOC detected vbus=...` and carries no reason; a real failure stays `failed ... reason=<why>`.
constexpr bool log_is_failure(State s, Fail f) { return s == State::Failed && f != Fail::Detected; }
const char* log_name(State s, Fail f);

// Everything the engine touches. The firmware glue (k2::fctrig) and the host fakes implement it;
// the defaults do nothing so a test only overrides what it needs.
struct Env {
    virtual std::uint32_t now_ms() { return 0; }
    virtual int vbus_mv() { return -1; }       // measured VBUS, < 0 when unknown
    virtual int dp_mv() { return -1; }
    virtual int dm_mv() { return -1; }
    virtual int ceiling_mv() { return 0; }     // power::max_vbus_mv(); 0 = nothing may be triggered
    virtual link::Reason inline_reason() { return link::Reason::None; }   // k2::link::inline_now()
    // Only the source's own unanswered caps behind the K2 (k2::link::silent_caps_only_now()): the one inline case a VOOC-family
    // start may override, and only when its caller allowed inline.
    virtual bool silent_caps_only() { return false; }
    // The HID-cable loop 5 V lock (k2::loopguard::locked()): no target above kLockMv starts or is retargeted to (OverLimit),
    // and a running session above it, or with VBUS above kLockVbusMv, is aborted with OverLimit (both lines to 0 V).
    virtual bool hv_locked() { return false; }
    virtual void set_dp(Drive) {}
    virtual void set_dm(Drive) {}
    virtual void play(std::span<const Step>) {}   // blocking timed list
    // One ACCP/AFC wire exchange: ping, tx bytes, ping, rx bytes (rx may be empty). False when the
    // charger did not answer the ping or the reply failed its checks.
    virtual bool xfer(std::span<const std::uint8_t>, std::span<std::uint8_t>) { return false; }
    // The same exchange tried exactly once (the hardware xfer() repeats it up to three times back to back). AFC uses this one: a
    // missed slave ping is retried by the engine after the lines have sat low, not in a tight loop.
    virtual bool xfer_once(std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx) { return xfer(tx, rx); }
    // One line per AFC exchange: n counts the session's exchanges from 1, tx is the command byte, rx the byte heard (-1 when the
    // exchange failed), vbus_mv the VBUS sample right after it.
    virtual void log_afc(int /*n*/, std::uint8_t /*tx*/, int /*rx*/, int /*vbus_mv*/) {}
    // The AFC settle ended: the highest D- level read while D+ sat at 0.6 V (0 = no BC1.2 short seen), D- now, ms waited.
    virtual void log_afc_settle(int /*short_mv*/, int /*dm_mv*/, int /*waited_ms*/) {}
    virtual void log_reg(std::uint8_t /*reg*/, std::uint8_t /*value*/) {}   // informational register reads
    virtual void log_qc3(int /*vbus_mv*/, int /*pending_mv*/, int /*step_mv*/, int /*n*/) {}   // one line per QC3 pulse batch
    virtual void log_qc3_coarse(int /*step_mv*/) {}   // a QC3+ session whose pulses move VBUS by QC3-sized steps (ends as no response)
    virtual void log_write(std::uint8_t /*reg*/, std::uint8_t /*value*/, bool /*acked*/) {}   // each register write and whether it was ACKed
    // VOOC: one adapter command frame (9 bits: 8 command bits + a trailing 0, D+ clocked) or -1 when none
    // arrived within the timeout. There is no transmit: the engine answers nothing (operator rule 2026-11-11).
    virtual int vooc_rx(std::uint32_t /*timeout_us*/) { return -1; }
    // UFCS over USART2: ufcs_open() runs the D+/D- handshake and claims the UART (false when the
    // handshake got no answer); ufcs_send() writes one frame; ufcs_recv() moves the bytes received so
    // far into the buffer without blocking; ufcs_close() releases the UART. reset_bus() also closes it.
    virtual bool ufcs_open() { return false; }
    virtual bool ufcs_send(std::span<const std::uint8_t>) { return false; }
    virtual std::size_t ufcs_recv(std::span<std::uint8_t>) { return 0; }
    virtual void ufcs_close() {}
    // The runtime frame/timing tuner (k2/fastcharge/ufcs_tune.hpp); the default is the published standard format.
    virtual const ufcs::Tuner& ufcs_tuner() { return ufcs::kDefaultTuner; }
    // UFCS wire diagnostics (log only, they never change what the engine decides): bytes the receiver
    // dropped (`held` is the whole accumulator, `used` how many of it went, `why` the parse verdict),
    // each decoded frame, and each retransmit (`what` names the phase, `pending` is the partial
    // frame still waiting in the accumulator).
    virtual void log_ufcs_drop(std::span<const std::uint8_t> /*held*/, std::size_t /*used*/, ufcs::Why /*why*/) {}
    virtual void log_ufcs_frame(const ufcs::Frame&) {}
    virtual void log_ufcs_echo() {}   // the session ends: only our own frames came back (D+/D- shorted)
    virtual void log_ufcs_retry(const char* /*what*/, int /*try_no*/, std::span<const std::uint8_t> /*pending*/) {}
    virtual void log_ufcs_exit(bool /*acked*/, std::uint32_t /*ms*/, int /*vbus_mv*/) {}   // the Exit_UFCS_Mode wait ended
    virtual void log_ufcs_busy_retry(std::uint32_t /*ms*/) {}   // a Request refused as busy goes out again after this wait
    // VIVO uses the same UART calls; vivo_open() only claims the UART with D+/D- high-Z (there
    // is no D+/D- handshake for it). False when the UART could not be claimed.
    virtual bool vivo_open() { return false; }
    virtual void reset_bus() {}                   // both lines to 0 V now, passive idle after 500 ms
    virtual bool reset_busy() { return false; }

protected:
    ~Env() = default;
};

// What the last sessions learned about the charger, for the planner that sizes later rows (the charger test). The SCP limits
// are re-read by every SCP session and the UFCS Output_Capabilities by every UFCS session; a list is empty / `valid` false until
// the session that reads it got its answer, and it stays after the session ends.
struct Caps {
    accp::ScpCaps scp;
    std::array<ufcs::Cap, ufcs::kMaxCaps> ufcs{};
    std::uint8_t ufcs_n = 0;
};

class Engine {
public:
    explicit Engine(Env& env) : env_(env) {}

    // Begin a trigger; Fail::None when accepted. Refusals leave the pins untouched. `allow_inline`
    // overrides the guard against a PD contract or a load behind the K2; it never replaces `confirmed`.
    // `scp_ma` is the SCP current limit written before the target voltage; 0 = detect only (the adapter registers
    // are read, nothing is written, the session ends as Fail::Detected). Other protocols ignore it.
    // `ufcs_ma` is the UFCS Request current (0 = ufcs::kNoCableMa, the no-cable-info default), held to the mode's range.
    Fail start(Proto p, int target_mv, bool confirmed, bool allow_inline = false, int scp_ma = accp::kScpInitMa,
               int ufcs_ma = 0);
    // Change the target of a running trigger (QC2 level, QC3 voltage, a new UFCS Request at `ufcs_ma`).
    // `scp_ma` >= 0 sets the SCP current limit of the new level (an SCP session the first row left detect-only gets its init then).
    Fail set_target(int target_mv, bool confirmed, int ufcs_ma = 0, int scp_ma = -1);
    // SCP: with keep(true) a session that ends Detected or NoChange is not released: the lines stay as they are, the keep-alive reads
    // go on and set_target() takes the next level on the live session (held()), so the next rows skip the BC1.2 re-detection and the
    // SCP detection. The state reads Failed with that fail until then; stop(), start() or kScpHoldMs release it. start() clears the flag.
    void keep(bool on) { keep_ = on; }
    bool held() const { return held_; }
    // Leave the session and go Idle. A running UFCS session first sends Exit_UFCS_Mode and keeps the UART (D- idle high)
    // until the source's ACK or kUfcsExitMs, which poll() serves (exiting()); only then the bus reset takes both lines to 0 V:
    // D- low for 2 ms or more while the source is still in UFCS mode is a hardware reset, which cuts VBUS. Every other
    // protocol and a failed session reset at once; a repeated stop() keeps the wait (stop_now() and start() end it).
    void stop();
    // Return to 5 V now (reset helper), no Exit_UFCS_Mode wait: the guards' stop (inline, overvoltage, no monitor).
    void stop_now();
    // An Exit_UFCS_Mode wait is running: the state is Idle, but the UART still holds D+/D-.
    bool exiting() const { return phase_ == Phase::UfcsExit; }
    // End an Exit_UFCS_Mode wait now (lines released, the reset stamped); a no-op otherwise. start() calls it.
    void finish_exit();
    void poll();
    // Only the UFCS message exchange (ACKs, retransmissions, timers) of poll(): safe to call from inside a long blocking
    // draw, so a source's tACKReceive window does not depend on how long the main loop spends elsewhere. No-op outside UFCS.
    void service_ufcs();
    // The QC2 / QC3 / QC3+ HVDCP handshake steps only (discharge, float, bias, the wait for the charger's drop), from the same LCD
    // idle hook: a draw delays a step by at most one hook round (the hook runs once 1 ms has passed, lcd::kServiceUs) instead of by the
    // whole draw. Never logs, blocks or aborts; a step that went wrong is left to poll(), which aborts. A no-op inside poll() / inside
    // itself (guarded), for FCP (its drop starts the register script, which only poll() runs) and outside the handshake.
    void service_hs();

    State state() const { return state_; }
    // Whether a session of `p` ever opens a window paint_critical() reports; the UI asks it before the engine starts (to say why the
    // screen will sit still). The QC2 / QC3 / QC3+ handshake is served by service_hs() and AFC has only tolerant phases.
    static constexpr bool blocks_paint(Proto p) {
        return p != Proto::Qc2 && p != Proto::Qc3 && p != Proto::Qc3Plus && p != Proto::Afc && p != Proto::Count;
    }
    // The engine is inside a window it times itself from poll() (nothing else serves it): the FCP / SCP handshake pulses (Hs* of
    // FCP, Hsb*: ms-timed D+/D- steps and a wake pulse), the VOOC receive slices (15 ms: a frame between two of them is lost), the
    // UFCS handshake (UfcsOpen; the Ping..Keep exchange is served by the LCD idle hook) and the vivo UART frames. A redraw there
    // (even a 5 ms one) can cost the handshake, so the charger-test page paints nothing while this is true. The settle and loop
    // phases (QC2 / QC3 / AFC / SCP keep-alive), the QC2 / QC3 / QC3+ handshake (service_hs()), the SCP reset (a plain wait: a
    // late end of it is harmless) and the hook-served UFCS exchange are tolerant: they are not listed.
    bool paint_critical() const {
        if (state_ != State::Negotiating && state_ != State::Active) return false;
        switch (phase_) {
        case Phase::HsDischarge: case Phase::HsFloatDp: case Phase::HsBias: case Phase::HsSample: case Phase::HsWaitDrop:
            return proto_ == Proto::Fcp;
        case Phase::HsbBias: case Phase::HsbWake: case Phase::HsbWaitLow: case Phase::HsbFinish:
        case Phase::VoocListen:
        case Phase::UfcsOpen:
        case Phase::VivoOpen: case Phase::VivoSeq: case Phase::VivoRun: return true;
        default: return false;
        }
    }
    Proto proto() const { return proto_; }
    Fail fail() const { return fail_; }
    int vooc_tries() const { return vooc_tries_; }   // VOOC/SVOOC wakes sent (1..3); 0 for other protocols
    // The opener the VOOC-family adapter clocked (vooc::is_detect_opener) when the session ended Detected; 0 otherwise. One ident proves the family, never the variant.
    std::uint8_t vooc_ident() const { return vooc_ident_; }
    int target_mv() const { return target_mv_; }
    int reached_mv() const { return reached_mv_; }   // VBUS at the last poll while running
    int adapter_mv() const { return adapter_mv_; }   // SCP: voltage the adapter reports (0 = unknown)
    int adapter_ma() const { return adapter_ma_; }
    const Caps& caps() const { return caps_; }

private:
    enum class Phase : std::uint8_t {
        Off,
        RedetectHold,                                             // lines at 0 V until the charger's 500 ms re-detection hold is over
        HsDischarge, HsFloatDp, HsBias, HsSample, HsWaitDrop,     // HVDCP handshake
        Qc2Settle,
        Qc3Enter, Qc3Loop,
        AfcSettle, AfcSend, Band,
        HsbReset, HsbBias, HsbWake, HsbWaitLow, HsbFinish,       // strict handshake
        Script, ScpKeep,
        VoocListen, VoocRest,
        UfcsOpen, UfcsPing, UfcsCaps, UfcsRequest, UfcsPower, UfcsKeep,
        VivoOpen, VivoSeq, VivoRun,
        UfcsExit,                                                 // Exit_UFCS_Mode sent, the UART kept until its ACK
    };
    // UFCS message exchange (reverse-engineered): every message that is not an ACK / NCK waits for its ACK.
    enum class UKind : std::uint8_t { Main, Keep, Refuse, Soft };   // Main: Ping opener, Get_Output_Capabilities, Request
    enum class UPend : std::uint8_t { None, Ping, Caps, Request };       // a main message queued until tMsgTransDelay has passed

    struct Action {
        enum class Op : std::uint8_t { Read, Write } op;
        enum class Chk : std::uint8_t { None, NonZero, Bit4Set, Bit4Clear, Log, Soft } chk;   // Soft: a write the adapter may refuse (FCP current)
        std::uint8_t reg, val;
        std::uint16_t wait_ms;
    };
    void poll_step();        // poll() inside its re-entrancy guard
    Fail hs_step(std::uint32_t now);   // one HVDCP handshake step (HsDischarge .. HsWaitDrop), shared by poll() and service_hs()
    void begin_protocol();   // the AFC / VOOC / UFCS / vivo openings, after the re-detection hold
    void build_script();
    void run_script();
    void keep_alive();
    bool vooc_inline_blocked();   // VOOC family: any inline reason, but the caller-allowed silent-caps window
    bool scp_unmoved(int vbus) const;
    void ufcs_poll(std::uint32_t now);
    void ufcs_track(std::size_t n, UKind kind, std::uint32_t now);
    void ufcs_send_ctl(std::uint8_t cmd, UKind kind);
    void ufcs_send_request(int pos, int mv, int ma);
    void ufcs_send_queued(std::uint32_t now);
    void ufcs_on_ack(const ufcs::Frame& f, std::uint32_t now);
    void ufcs_on_msg(const ufcs::Frame& f, std::uint32_t now);
    void ufcs_implicit_ack();
    void ufcs_source_reset(std::uint32_t now);
    void ufcs_resend(std::uint32_t now);
    void ufcs_escalate(std::uint32_t now);
    void ufcs_clear();
    const char* ufcs_what() const;
    bool ufcs_take(ufcs::Frame& f);
    void vivo_poll(std::uint32_t now);
    bool vivo_take(vivo::Reply& r);
    void vivo_send(const vivo::Canned& c);
    void vivo_send_volt(int mv);
    void vivo_advance(std::uint32_t now);
    void vivo_release();
    bool reg_read(std::uint8_t reg, std::uint8_t& out);
    bool reg_write(std::uint8_t reg, std::uint8_t val);
    void begin_script();
    void abort(Fail why);
    void release_bus();                   // UART closed, both lines to 0 V, the release stamped
    void ufcs_exit_poll(std::uint32_t now);
    void go(Phase p, std::uint32_t wait_ms);
    void apply_target_levels();
    Fail validate(Proto p, int& target_mv, bool confirmed) const;
    int guard_mv() const;

    Env& env_;
    bool busy_ K2_BSS_INIT(false);           // poll() or service_hs() is running: the other one (reached through a draw) does nothing
    State state_ K2_BSS_INIT(State::Idle);
    Proto proto_ K2_BSS_INIT(Proto::Qc2);
    Fail fail_ K2_BSS_INIT(Fail::None);
    Phase phase_ K2_BSS_INIT(Phase::Off);
    std::array<Action, 24> script_ K2_BSS_ZERO;
    std::size_t script_n_ K2_BSS_INIT(0), script_i_ K2_BSS_INIT(0);
    int afc_n_ K2_BSS_INIT(0);               // AFC exchanges of this session (the log's n)
    bool afc_retried_ K2_BSS_INIT(false);    // the one retry of an unanswered first AFC exchange was used
    bool afc_short_seen_ K2_BSS_INIT(false); // the settle has judged D- once (qc::afc_ping_due)
    int sends_ K2_BSS_INIT(0), ok_sends_ K2_BSS_INIT(0), wake_i_ K2_BSS_INIT(0), keep_fail_ K2_BSS_INIT(0), adapter_mv_ K2_BSS_INIT(0), adapter_ma_ K2_BSS_INIT(0);
    // AFC / FCP / SCP: the charger answered the protocol itself (a CRC-valid ACCP frame or an AFC byte exchange). Until it has,
    // a failure of the session says "this charger does not speak it" (see abort()), whatever VBUS did: QC-only banks move VBUS
    // on the D+/D- levels alone.
    bool acked_ K2_BSS_INIT(false);
    bool unacked_handshake() const { return !acked_ && (proto_ == Proto::Afc || proto_ == Proto::Fcp || proto_ == Proto::Scp); }
    int scp_ma_ = accp::kScpInitMa;
    int ufcs_ma_ K2_BSS_INIT(0);   // UFCS Request current, 0 = ufcs::kNoCableMa
    int keep_i_ K2_BSS_INIT(0), vooc_tries_ K2_BSS_INIT(0), vooc_frames_ K2_BSS_INIT(0);
    std::uint8_t vooc_ident_ K2_BSS_INIT(0);   // VOOC: wakes sent, frames heard (not all ones)
    std::array<std::uint8_t, ufcs::kFrameMax> ux_ K2_BSS_ZERO;          // UFCS receive accumulator
    std::uint8_t ux_n_ K2_BSS_INIT(0), ux_drop_ K2_BSS_INIT(0);        // bytes held / bytes to drop (both below ufcs::kFrameMax)
    Caps caps_ K2_BSS_ZERO;                                // what the adapter / source said about itself (see caps())
    std::array<std::uint8_t, 8> scp_lim_ K2_BSS_ZERO;      // SCP limit registers 0x90..0x97 as read in this session
    void patch_scp_current();
    bool inline_ok_ K2_BSS_INIT(false);   // start() was allowed to run inline (the charger test's own PD-silent start)
    bool keep_ K2_BSS_INIT(false), held_ K2_BSS_INIT(false);   // keep(): hold an SCP session at its Detected / NoChange end; held_: it is held now
    bool scp_read_ K2_BSS_INIT(false), scp_on_ K2_BSS_INIT(false);   // this session read the adapter's registers / wrote the enable and init
    int scp_iset_ma_ K2_BSS_INIT(0);                                  // the current limit written last
    std::uint32_t hold_ms_ K2_BSS_INIT(0);                            // when the hold began
    void hold_poll(std::uint32_t now);
    std::uint8_t uid_ K2_BSS_INIT(0);                       // message number of our next message (0..15); the source's ACK moves it on
    std::array<std::uint8_t, 14> utx_ K2_BSS_ZERO;         // the message sent last that is still waiting for its ACK (kept for the retransmit; a Request is the longest, 14 bytes)
    std::uint8_t utx_n_ K2_BSS_INIT(0);                     // its length, 0 = nothing outstanding
    UKind utx_kind_ K2_BSS_INIT(UKind::Main);
    std::uint8_t utx_id_ K2_BSS_INIT(0);
    std::uint32_t utx_ms_ K2_BSS_INIT(0);                   // when it went out (last time)
    std::uint8_t uretx_ K2_BSS_INIT(0);                     // retransmits of it so far
    bool uwait_ K2_BSS_INIT(false);                         // the main message was ACKed: its answer is due at due_ms_
    bool usoft_ K2_BSS_INIT(false);                         // a Soft_Reset was already spent on this negotiation
    bool usrst_ K2_BSS_INIT(false);                         // the source's Soft_Reset was already answered with a new opening since the last Power_Ready
    UPend upend_ K2_BSS_INIT(UPend::None);
    std::uint32_t upend_ms_ K2_BSS_INIT(0);
    ufcs::Pick upk_ K2_BSS_ZERO;                           // the Request (re)sent by the engine
    bool uref_ K2_BSS_INIT(false);                          // a Refuse is owed to the source (unsupported request)
    std::uint8_t uref_id_ K2_BSS_INIT(0), uref_type_ K2_BSS_INIT(0), uref_cmd_ K2_BSS_INIT(0), uref_reason_ K2_BSS_INIT(0);
    std::uint32_t uref_ms_ K2_BSS_INIT(0);
    bool usrc_valid_ K2_BSS_INIT(false), usrc_data_ K2_BSS_INIT(false);   // the last source message handled, to tell a retransmission from a new message
    std::uint8_t usrc_id_ K2_BSS_INIT(0), usrc_cmd_ K2_BSS_INIT(0);
    std::size_t vi_ K2_BSS_INIT(0);                       // VIVO: next sequence frame
    int vreq_ K2_BSS_INIT(0);                               // VIVO: voltage in the last command-3 frame
    bool vheard_ K2_BSS_INIT(false);
    int utries_ K2_BSS_INIT(0);
    bool ubusy_retried_ K2_BSS_INIT(false);                 // this Request was already refused as busy once and sent again
    bool uart_up_ K2_BSS_INIT(false);
    bool uecho_ K2_BSS_INIT(false), uheard_ K2_BSS_INIT(false);        // UFCS: our own last frame came back verbatim / a frame addressed to the sink arrived
    std::uint32_t ulast_rx_ K2_BSS_INIT(0);
    void rx_shift(std::size_t n);
    void hsb_poll(std::uint32_t now);
    void afc_poll(std::uint32_t now);
    void vooc_poll(std::uint32_t now);
    void qc3_send_pulses(qc::Mode m, int total, int cap, int vbus);
    bool qc3_poll(std::uint32_t now, int vbus, bool& rests_at_5v);
    void vooc_wake();
    std::array<std::uint8_t, 4> keep_raw_ K2_BSS_ZERO;
    int target_mv_ K2_BSS_INIT(0), applied_mv_ = 5000, prev_mv_ = 5000, entry_mv_ K2_BSS_INIT(0), reached_mv_ K2_BSS_INIT(0), dm0_mv_ K2_BSS_INIT(0), qc3_pending_mv_ K2_BSS_INIT(0), qc3_seen_mv_ K2_BSS_INIT(0), qc3_step_mv_ K2_BSS_INIT(0), qc3_base_mv_ K2_BSS_INIT(0), qc3_cmd_n_ K2_BSS_INIT(0);
    bool qc3_learned_ K2_BSS_INIT(false);
    qc::Settle qc3_settle_;                      // is VBUS stable since the last batch?
    int qc3_nomove_ K2_BSS_INIT(0);                         // pulses commanded in a row without VBUS moving (QC3+ "no response")
    int qc3_dead_ K2_BSS_INIT(0);                           // settled up batches in a row that left VBUS in the 12 V band unmoved (a Class A top)
    std::uint32_t qc3_sent_ms_ K2_BSS_INIT(0);
    std::uint32_t reset_ms_ K2_BSS_INIT(0);                 // when the last session released the lines (both at 0 V)
    bool reset_seen_ K2_BSS_INIT(false);
    std::uint32_t due_ms_ K2_BSS_INIT(0), phase_ms_ K2_BSS_INIT(0), change_ms_ K2_BSS_INIT(0), band_ms_ K2_BSS_INIT(0), lost_ms_ K2_BSS_INIT(0);
};

}  // namespace k2::fc
