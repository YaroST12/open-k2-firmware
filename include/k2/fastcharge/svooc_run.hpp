// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Guarded SuperVOOC trigger, part 3 of 3: the guards and the session, pure (tests/test_svooc_run.cpp). The firmware glue
// (src/fastcharge/svooc_trig.cpp) gathers the facts, feeds a Sample every main-loop pass and carries out the Next it
// gets back; every decision about starting, stopping and the exit check lives here.
//
// The trigger runs with NO LOAD only: the K2 stands in for a phone on D+/D- while nothing draws current, so the adapter
// may raise VBUS but no power flows. It never starts unless (refusal()):
//   - the remote confirmed it, and the host link is up (the dead-man lease needs the host);
//   - D+/D- are free (no trigger, scan, charger test or sniffer of another owner);
//   - the power monitor reads VBUS and current;
//   - no sink is on the bottom port (Rd on CC) and the link is not inline (a device behind the K2). The operator may waive
//     the far-sink check only (`--usb-a`, StartFacts::usb_a): a legacy A-to-C cable brings VBUS in through that port with no
//     pull-down on CC, which reads as a sink. It is refused (UsbAWithPd) while PD is active anywhere (Contract != None); the
//     50 mA limit, the VBUS window, the inline check and every in-run guard but the far-sink abort stay in force;
//   - no PD contract, except the K2's own: it holds it as the sink, a fixed PDO at or under 5.0 V (position 1, Ready,
//     not EPR, not inline, no far sink). A USB-C adapter such as the OnePlus SUPERVOOC port keeps VBUS at 0 V until it
//     sees Rd, so the K2 always has that contract, and the real phone does too before SuperVOOC takes over on D+/D-;
//   - |I| <= kCurrentMaxMa;
//   - VBUS is about 5 V (kFiveMinMv .. kFiveMaxMv);
//   - the mode RAM is free (the recorder of the session's edges; the USB drive borrows it).
// While it runs, every poll checks, in this order, and the first that fails ends it (abort, one Next::Release):
//   host gone (lease expiry and USB drop come in as stop()), monitor lost, VBUS > kVbusMaxMv, |I| > kCurrentMaxMa,
//   a sink or inline device appears, a PD contract appears (started without one) or changes to anything but the same
//   own 5 V one, or is lost (hard reset, detach: the D+/D- session is meaningless then), an edge storm on D+/D-, the
//   charger off-script for more than two frames in a row (the wire stopped), the run-time cap. The K2 never touches the
//   contract or the Rd pin itself: the main session releases them.
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
    PdContract,     // a PD contract is held that is not the K2's own 5 V fixed one
    Current,        // |I| > kCurrentMaxMa
    NotFiveVolts,   // VBUS outside kFiveMinMv .. kFiveMaxMv
    NoRam,          // the mode RAM is lent out (the USB drive is on)
    UsbAWithPd,     // --usb-a asked while PD is active anywhere (the waiver is for a PD-less legacy A port only)
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
    ContractLost,   // the own 5 V contract went away (hard reset, detach)
    ContractChanged,// the own 5 V contract became another one
};

// The PD contract as the guards see it.
enum class Contract : std::uint8_t {
    None = 0,   // no PD contract
    Own5V,      // the K2's own: it holds it as sink, a fixed PDO at or under 5.0 V
    Other,      // any other: above 5 V, PPS / AVS / EPR, held by a far sink or inline, passively decoded, mid-renegotiation
};

// The facts classify() reads (the glue fills them from the link probe and the sink status).
struct ContractFacts {
    bool pd_active = false;       // PD traffic or a decoded contract on CC
    bool held = false;            // the K2's own sink holds a contract (SinkStatus::held)
    bool ready = false;           // SinkState::Ready
    bool epr_mode = false;
    bool inline_mode = false;     // a device behind the K2
    bool far_sink = false;        // a far sink draws through the K2
    std::uint8_t pos = 0;         // object position of the contract (1 = the 5 V fixed PDO; 2+ may be PPS / AVS)
    std::uint16_t mv = 0;
};

constexpr Contract classify(const ContractFacts& c) {
    if (!c.pd_active && !c.held) return Contract::None;
    const bool own = c.held && c.ready && !c.epr_mode && !c.inline_mode && !c.far_sink && c.pos == 1 && c.mv <= 5000;
    return own ? Contract::Own5V : Contract::Other;
}

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
enum class Next : std::uint8_t {
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
    Contract contract = Contract::None;
    bool ram_free = false;
    bool usb_a = false;           // the operator's waiver of the far-sink refusal (a legacy A-to-C cable on the bottom port)
};

struct Sample {
    std::uint32_t now_ms = 0;
    bool host_ok = false;
    bool vbus_known = false, current_known = false;
    std::int32_t vbus_mv = 0;     // the latest sample (power::vbus_fast_mv), never the filtered display reading
    std::int32_t ibus_ma = 0;
    bool far_sink = false;
    bool inline_mode = false;
    Contract contract = Contract::None;
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
    Contract contract_start = Contract::None;   // the contract at the start
    Contract contract_end = Contract::None;     // the latest sample's contract (the end of the run, the exit watch included)
    bool usb_a = false;              // the run started with the far-sink check waived (USB-A)
};

// Log and report names (tools/k2_remote.py keeps the same lists, in the same order).
constexpr const char* name(Refusal r) {
    switch (r) {
    case Refusal::None: return "none";
    case Refusal::NotConfirmed: return "not confirmed";
    case Refusal::Busy: return "D+/D- busy";
    case Refusal::NoHost: return "no host link";
    case Refusal::NoMonitor: return "no power monitor";
    case Refusal::Sink: return "sink on the bottom port";
    case Refusal::Inline: return "inline mode";
    case Refusal::PdContract: return "PD contract not own 5 V";
    case Refusal::Current: return "current over 50 mA";
    case Refusal::NotFiveVolts: return "VBUS not 5 V";
    case Refusal::NoRam: return "no RAM: USB drive enabled";
    case Refusal::UsbAWithPd: return "USB-A waiver but PD is active";
    }
    return "?";
}

constexpr const char* name(Abort a) {
    switch (a) {
    case Abort::None: return "none";
    case Abort::Overvoltage: return "VBUS over 12 V";
    case Abort::Current: return "current over 50 mA";
    case Abort::OffScript: return "charger off-script";
    case Abort::HostGone: return "host gone";
    case Abort::NoMonitor: return "power monitor lost";
    case Abort::Sink: return "sink appeared";
    case Abort::PdContract: return "PD contract appeared";
    case Abort::Storm: return "D+/D- edge storm";
    case Abort::NoAdapter: return "no adapter answer";
    case Abort::TimeCap: return "time cap";
    case Abort::Stopped: return "stopped";
    case Abort::ContractLost: return "PD contract lost";
    case Abort::ContractChanged: return "PD contract changed";
    }
    return "?";
}

constexpr const char* name(Contract c) {
    switch (c) {
    case Contract::None: return "none";
    case Contract::Own5V: return "own 5 V";
    case Contract::Other: return "other";
    }
    return "?";
}

inline constexpr std::int32_t magnitude(std::int32_t v) { return v < 0 ? -v : v; }

constexpr Refusal refusal(const StartFacts& f) {
    if (!f.confirmed) return Refusal::NotConfirmed;
    if (f.ports_busy) return Refusal::Busy;
    if (!f.host_ok) return Refusal::NoHost;
    if (!f.vbus_known || !f.current_known) return Refusal::NoMonitor;
    // The waiver is for a PD-less legacy A port: any PD (a contract of ours or another, traffic on CC) refuses the flag itself.
    if (f.usb_a && f.contract != Contract::None) return Refusal::UsbAWithPd;
    if (f.far_sink && !f.usb_a) return Refusal::Sink;
    if (f.inline_mode) return Refusal::Inline;
    if (f.contract == Contract::Other) return Refusal::PdContract;
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
        report_.contract_start = report_.contract_end = start_contract_ = f.contract;
        report_.usb_a = usb_a_ = f.usb_a;
        start_t_ = now_ms;
        stage_ = Stage::Wake;
        return Refusal::None;
    }

    Next poll(const Sample& s) {
        if (stage_ == Stage::Idle || stage_ == Stage::Done) return Next::None;
        track(s);
        if (stage_ == Stage::Exit) { watch_exit(s); return Next::None; }
        report_.commands = s.commands;
        report_.accepted = report_.accepted || s.accepted;
        if (const Abort a = guard(s); a != Abort::None) return finish(a, s.now_ms);
        switch (stage_) {
        case Stage::Wake: return wake(s.now_ms);
        case Stage::Listen:
            if (s.commands > 0) { stage_ = Stage::Answer; return Next::None; }
            if (s.now_ms - stage_t_ < kWakeListenMs) return Next::None;
            if (report_.tries >= kWakeTries) return finish(Abort::NoAdapter, s.now_ms);
            stage_ = Stage::Rest;
            stage_t_ = s.now_ms;
            return Next::Rest;
        case Stage::Rest:
            if (s.commands > 0) { stage_ = Stage::Answer; return Next::None; }   // a late ident
            return s.now_ms - stage_t_ < kWakeRestMs ? Next::None : wake(s.now_ms);
        default: return Next::None;   // Answer: the wire does the work
        }
    }

    // An outside end (the lease, the remote's stop, BACK, USB gone): Release once; nothing if the lines are not held.
    Next stop(Abort why, std::uint32_t now_ms) {
        if (!lines_held()) return Next::None;
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
        report_.contract_end = s.contract;
        if (s.vbus_known) report_.vbus_max_mv = std::max(report_.vbus_max_mv, s.vbus_mv);
        if (s.current_known) report_.ibus_max_ma = std::max(report_.ibus_max_ma, magnitude(s.ibus_ma));
    }

    Abort guard(const Sample& s) const {
        if (!s.host_ok) return Abort::HostGone;
        if (!s.vbus_known || !s.current_known) return Abort::NoMonitor;
        if (s.vbus_mv > kVbusMaxMv) return Abort::Overvoltage;
        if (magnitude(s.ibus_ma) > kCurrentMaxMa) return Abort::Current;
        if ((s.far_sink && !usb_a_) || s.inline_mode) return Abort::Sink;
        if (s.contract != start_contract_) {
            if (start_contract_ != Contract::Own5V) return Abort::PdContract;   // none at the start: one appeared
            return s.contract == Contract::None ? Abort::ContractLost : Abort::ContractChanged;
        }
        if (s.contract == Contract::Other) return Abort::PdContract;   // (a run never starts with one)
        if (s.storm) return Abort::Storm;
        if (s.off_script) return Abort::OffScript;
        if (s.now_ms - start_t_ >= report_.cap_ms) return Abort::TimeCap;
        return Abort::None;
    }

    Next wake(std::uint32_t now) {
        ++report_.tries;
        stage_ = Stage::Listen;
        stage_t_ = now;
        return Next::Wake;
    }

    Next finish(Abort why, std::uint32_t now) {
        report_.abort = why;
        report_.elapsed_ms = now - start_t_;
        stage_ = Stage::Exit;
        stage_t_ = now;
        return Next::Release;
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
    Contract start_contract_ = Contract::None;
    bool usb_a_ = false;
    Report report_;
    std::uint32_t start_t_ = 0, stage_t_ = 0;
    std::int32_t last_vbus_ = 0;
};

}  // namespace k2::svooc
