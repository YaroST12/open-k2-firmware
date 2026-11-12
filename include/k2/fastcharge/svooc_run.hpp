// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Guarded SuperVOOC trigger, part 3 of 3: the guards and the session, pure (tests/test_svooc_run.cpp). The firmware glue
// (src/fastcharge/svooc.cpp) gathers the facts, feeds a Sample every main-loop pass and carries out the Step it gets
// back; every decision about starting, stopping and the exit check lives here.
//
// The trigger runs with NO LOAD only: the K2 stands in for a phone on D+/D- while nothing draws current, so the adapter
// may raise VBUS but no power flows. It never starts unless (refusal()):
//   - the remote confirmed it, and the host link is up (the dead-man lease needs the host);
//   - D+/D- are free (no trigger, scan, charger test or sniffer of another owner);
//   - the power monitor reads VBUS and current;
//   - no sink is on the bottom port (Rd on CC) and the link is not inline (a device behind the K2);
//   - no PD contract is held;
//   - |I| <= kCurrentMaxMa;
//   - VBUS is about 5 V (kFiveMinMv .. kFiveMaxMv);
//   - the mode RAM is free (the recorder of the session's edges; the USB drive borrows it).
// While it runs, every poll checks, in this order, and the first that fails ends it (abort, one Step::Release):
//   host gone (lease expiry and USB drop come in as stop()), monitor lost, VBUS > kVbusMaxMv, |I| > kCurrentMaxMa,
//   a sink or inline device appears, a PD contract appears, an edge storm on D+/D-, the charger off-script for more
//   than two frames in a row (the wire stopped), the run-time cap.
// Exit always means: stop answering, let D- go, both lines back to passive idle. Then the run watches VBUS for
// kExitWatchMs: back at or under kExitOkMv is exit_ok; still above when the watch ends is reported with the reading.
#pragma once
#include <algorithm>
#include <cstdint>

namespace k2::svooc {

inline constexpr std::int32_t kVbusMaxMv = 12000;     // above this: abort (12.0 V itself runs)
inline constexpr std::int32_t kCurrentMaxMa = 50;     // |I| above this: refuse / abort (the inline threshold)
inline constexpr std::int32_t kFiveMinMv = 4500, kFiveMaxMv = 5500;   // "about 5 V" at the start
inline constexpr std::int32_t kExitOkMv = 5500;       // VBUS back to 5 V after the exit
inline constexpr std::uint32_t kCapMs = 20000;        // the total run time, wake to release
inline constexpr std::uint32_t kExitWatchMs = 5000;   // how long the exit check waits for VBUS to come down
// The wake (fc_engine's vooc_wake burst, 24 ms): the adapter's first ident came ~540 ms after it on hardware.
inline constexpr int kWakeTries = 3;
inline constexpr std::uint32_t kWakeListenMs = 1500;  // listen this long after a wake for the first command
inline constexpr std::uint32_t kWakeRestMs = 3000;    // lines quiet this long before the next wake

enum class Refusal : std::uint8_t {
    None = 0,
    NotConfirmed,   // no --confirm
    Busy,           // D+/D- owned (a trigger, scan, charger test, the sniffer), or a run is on
    NoHost,         // the host link is down: no lease possible
    NoMonitor,      // the power monitor gives no VBUS or current
    Sink,           // a sink (Rd) on the bottom port
    Inline,         // a device behind the K2 (inline mode)
    PdContract,     // a PD contract is held
    Current,        // |I| > kCurrentMaxMa
    NotFiveVolts,   // VBUS outside kFiveMinMv .. kFiveMaxMv
    NoRam,          // the mode RAM is lent out (the USB drive is on)
};

enum class Abort : std::uint8_t {
    None = 0,
    Overvoltage,    // VBUS > kVbusMaxMv
    Current,        // |I| > kCurrentMaxMa
    OffScript,      // the charger left the script for more than two frames
    HostGone,       // USB or the host dropped
    NoMonitor,      // the power monitor stopped reading
    Sink,           // a sink or inline device appeared
    PdContract,     // a PD contract appeared
    Storm,          // the D+/D- edge-storm guard tripped
    NoAdapter,      // no command after kWakeTries wakes
    TimeCap,        // the run-time cap
    Stopped,        // stop(): the lease, the remote's stop, BACK
};

enum class Stage : std::uint8_t {
    Idle,     // never started, or refused
    Wake,     // started: the next poll wakes
    Listen,   // woken, waiting for the first command
    Rest,     // no command after a wake: quiet before the next one
    Answer,   // the adapter talks, the wire answers
    Exit,     // released: watching VBUS come back to 5 V
    Done,
};

// What the glue does now.
enum class Step : std::uint8_t {
    None,
    Wake,      // play the wake burst, then arm the wire (D+ listen, D- sniff)
    Rest,      // a wake went unanswered: keep the lines passive until the next one
    Release,   // stop answering, let D- go, lines to passive idle
};

struct StartFacts {
    bool confirmed = false;
    bool ports_busy = false;
    bool host_ok = false;
    bool vbus_known = false, current_known = false;
    std::int32_t vbus_mv = 0;
    std::int32_t ibus_ma = 0;     // signed: either direction counts
    bool far_sink = false;
    bool inline_mode = false;
    bool pd_contract = false;
    bool ram_free = false;
};

struct Sample {
    std::uint32_t now_ms = 0;
    bool host_ok = false;
    bool vbus_known = false, current_known = false;
    std::int32_t vbus_mv = 0;     // the latest sample (power::vbus_fast_mv), never the filtered display reading
    std::int32_t ibus_ma = 0;
    bool far_sink = false;
    bool inline_mode = false;
    bool pd_contract = false;
    bool storm = false;
    bool off_script = false;      // Wire::stopped(): the responder aborted
    std::uint32_t commands = 0;   // Log::commands so far
    bool accepted = false;        // Responder::accepted(): 88 > 04D was answered
};

struct Report {
    Refusal refusal = Refusal::None;
    Abort abort = Abort::None;
    std::uint8_t tries = 0;          // wakes played
    bool accepted = false;
    std::uint32_t commands = 0;
    std::uint32_t cap_ms = kCapMs;
    std::uint32_t elapsed_ms = 0;    // start to release
    std::int32_t vbus_start_mv = 0;
    std::int32_t vbus_max_mv = 0;    // the highest VBUS seen, the exit watch included
    std::int32_t ibus_max_ma = 0;    // the highest |I| seen
    bool exit_checked = false;
    bool exit_ok = false;            // VBUS came back to <= kExitOkMv
    std::int32_t vbus_exit_mv = 0;   // the reading that ended the exit watch
};

inline constexpr std::int32_t magnitude(std::int32_t v) { return v < 0 ? -v : v; }

constexpr Refusal refusal(const StartFacts& f) {
    if (!f.confirmed) return Refusal::NotConfirmed;
    if (f.ports_busy) return Refusal::Busy;
    if (!f.host_ok) return Refusal::NoHost;
    if (!f.vbus_known || !f.current_known) return Refusal::NoMonitor;
    if (f.far_sink) return Refusal::Sink;
    if (f.inline_mode) return Refusal::Inline;
    if (f.pd_contract) return Refusal::PdContract;
    if (magnitude(f.ibus_ma) > kCurrentMaxMa) return Refusal::Current;
    if (f.vbus_mv < kFiveMinMv || f.vbus_mv > kFiveMaxMv) return Refusal::NotFiveVolts;
    if (!f.ram_free) return Refusal::NoRam;
    return Refusal::None;
}

class Run {
public:
    // cap_ms: a shorter run than kCapMs if the caller asks; never a longer one.
    Refusal start(const StartFacts& f, std::uint32_t now_ms, std::uint32_t cap_ms = kCapMs) {
        if (running()) return Refusal::Busy;
        report_ = Report{};
        report_.refusal = refusal(f);
        if (report_.refusal != Refusal::None) { stage_ = Stage::Idle; return report_.refusal; }
        report_.cap_ms = (cap_ms == 0 || cap_ms > kCapMs) ? kCapMs : cap_ms;
        report_.vbus_start_mv = report_.vbus_max_mv = f.vbus_mv;
        report_.ibus_max_ma = magnitude(f.ibus_ma);
        start_t_ = now_ms;
        stage_ = Stage::Wake;
        return Refusal::None;
    }

    Step poll(const Sample& s) {
        if (stage_ == Stage::Idle || stage_ == Stage::Done) return Step::None;
        track(s);
        if (stage_ == Stage::Exit) { watch_exit(s); return Step::None; }
        report_.commands = s.commands;
        report_.accepted = report_.accepted || s.accepted;
        if (const Abort a = guard(s); a != Abort::None) return finish(a, s.now_ms);
        switch (stage_) {
        case Stage::Wake: return wake(s.now_ms);
        case Stage::Listen:
            if (s.commands > 0) { stage_ = Stage::Answer; return Step::None; }
            if (s.now_ms - stage_t_ < kWakeListenMs) return Step::None;
            if (report_.tries >= kWakeTries) return finish(Abort::NoAdapter, s.now_ms);
            stage_ = Stage::Rest;
            stage_t_ = s.now_ms;
            return Step::Rest;
        case Stage::Rest:
            if (s.commands > 0) { stage_ = Stage::Answer; return Step::None; }   // a late ident
            return s.now_ms - stage_t_ < kWakeRestMs ? Step::None : wake(s.now_ms);
        default: return Step::None;   // Answer: the wire does the work
        }
    }

    // An outside end (the lease, the remote's stop, BACK, USB gone): Release once; nothing if the lines are not held.
    Step stop(Abort why, std::uint32_t now_ms) {
        if (!lines_held()) return Step::None;
        return finish(why, now_ms);
    }

    Stage stage() const { return stage_; }
    bool lines_held() const {   // the session owns D+/D-
        return stage_ == Stage::Wake || stage_ == Stage::Listen || stage_ == Stage::Rest || stage_ == Stage::Answer;
    }
    bool running() const { return lines_held() || stage_ == Stage::Exit; }   // nothing else may start yet
    const Report& report() const { return report_; }

private:
    void track(const Sample& s) {
        if (s.vbus_known) report_.vbus_max_mv = std::max(report_.vbus_max_mv, s.vbus_mv);
        if (s.current_known) report_.ibus_max_ma = std::max(report_.ibus_max_ma, magnitude(s.ibus_ma));
    }

    Abort guard(const Sample& s) const {
        if (!s.host_ok) return Abort::HostGone;
        if (!s.vbus_known || !s.current_known) return Abort::NoMonitor;
        if (s.vbus_mv > kVbusMaxMv) return Abort::Overvoltage;
        if (magnitude(s.ibus_ma) > kCurrentMaxMa) return Abort::Current;
        if (s.far_sink || s.inline_mode) return Abort::Sink;
        if (s.pd_contract) return Abort::PdContract;
        if (s.storm) return Abort::Storm;
        if (s.off_script) return Abort::OffScript;
        if (s.now_ms - start_t_ >= report_.cap_ms) return Abort::TimeCap;
        return Abort::None;
    }

    Step wake(std::uint32_t now) {
        ++report_.tries;
        stage_ = Stage::Listen;
        stage_t_ = now;
        return Step::Wake;
    }

    Step finish(Abort why, std::uint32_t now) {
        report_.abort = why;
        report_.elapsed_ms = now - start_t_;
        stage_ = Stage::Exit;
        stage_t_ = now;
        return Step::Release;
    }

    void watch_exit(const Sample& s) {
        if (s.vbus_known) last_vbus_ = s.vbus_mv;
        const bool back = s.vbus_known && s.vbus_mv <= kExitOkMv;
        if (!back && s.now_ms - stage_t_ < kExitWatchMs) return;
        report_.exit_checked = true;
        report_.exit_ok = back;
        report_.vbus_exit_mv = s.vbus_known ? s.vbus_mv : last_vbus_;
        stage_ = Stage::Done;
    }

    Stage stage_ = Stage::Idle;
    Report report_;
    std::uint32_t start_t_ = 0, stage_t_ = 0;
    std::int32_t last_vbus_ = 0;
};

}  // namespace k2::svooc
