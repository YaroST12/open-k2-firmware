// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger test's SuperVOOC ceiling stage, the pure part: what the stage reads of the guarded trigger, when it ends,
// and how its result reads on the screen and in the remote readout.
//
// NO LOAD. The stage runs the guarded trigger (k2/fastcharge/svooc_trig.hpp) with the Probe profile: the K2 answers the
// adapter as a phone would and asks for 1.5, 4.0, 6.3 and 10.9 A in turn (0x86 words 0x00F, 0x028, 0x03F, 0x06D, 100 mA
// per count). Nothing is drawn: the 0x86 reply is the K2's own word, so the "ceiling" is the highest ask under which the
// adapter went on talking (kAcceptPolls on-script 82 polls), the current the adapter accepted to be asked for. It is read
// from the protocol, never tested by load, and it is not a measured current. The word carries no voltage.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/fastcharge/svooc_run.hpp"
#include "k2/fastcharge/svooc_script.hpp"

namespace k2::svooc_ceiling {

// Opt-in per run (never in a preset): Off = no stage; On = the trigger's guards all stay active; UsbA = the operator
// states a legacy USB-A port with the top port empty, so the far-sink refusal is waived (svooc_run.hpp Params usb_a).
enum class Mode : std::uint8_t { Off, On, UsbA };
inline constexpr std::uint8_t kModeCount = 3;

// How the stage ended. None = not run (or still running). Refused = the trigger's start guards said no (End detail =
// svooc::Refusal); NoAdapter = the adapter never spoke to the K2's wake (a plain VOOC charger, or a cable that does not
// carry D+/D-); Aborted = a guard or the adapter ended the session early (detail = svooc::Abort), the ceiling stays what
// had been accepted so far; Cap = the stage's time cap ended it; Judged = every rung of the ladder was asked and judged;
// Cancelled = the run was stopped; Stopped = the adapter talked, then went quiet with no guard tripped (every no-load session
// so far: ~3.3 s after the wake on the OnePlus 120 W brick): a normal outcome, read as "stopped at <time>" with what had been
// claimed until then and the highest VBUS seen. Appended last: the remote and tools/k2_remote.py keep the numbers.
enum class End : std::uint8_t { None, Refused, NoAdapter, Aborted, Cap, Judged, Cancelled, Stopped };
inline constexpr std::uint8_t kEndCount = 8;

inline constexpr std::uint32_t kCapS = 12;                     // the trigger's own run cap for the stage
inline constexpr std::uint32_t kCapMs = kCapS * 1000;
inline constexpr std::uint32_t kExitGraceMs = 6000;            // after the stop: how long the exit watch may still hold the lines
inline constexpr std::size_t kLadder = 4;                      // rungs of the Probe ladder
// An adapter that spoke and then sent no edge for this long has ended the session (it polls 0x82 every ~44 ms while it talks).
inline constexpr std::uint32_t kQuietMs = 500;
// VBUS under vSafe5V this long in the release after the stage: the adapter dropped its output (hardware 2026-11-15, OnePlus
// 120 W: 7.98 V -> 0.74 V in 60 ms, 80 ms after the release) and the run brings it back with an Rd cycle.
inline constexpr std::uint32_t kDropMs = 200;

constexpr const char* name(Mode m) { return m == Mode::On ? "on" : m == Mode::UsbA ? "usb-a" : "off"; }
constexpr const char* name(End e) {
    switch (e) {
    case End::None: return "none";
    case End::Refused: return "refused";
    case End::NoAdapter: return "no adapter";
    case End::Aborted: return "aborted";
    case End::Cap: return "cap";
    case End::Judged: return "judged";
    case End::Cancelled: return "cancelled";
    case End::Stopped: return "stopped";
    }
    return "?";
}

// The mA a 0x86 word stands for (100 mA per count), clamped to the 9-bit word's range.
constexpr std::uint32_t ceiling_ma(std::uint16_t word) { return std::uint32_t(word > 0x1FF ? 0x1FF : word) * 100u; }

// Every rung of the ladder was asked and the last one stood for kAcceptPolls polls: nothing left to learn from the session.
constexpr bool ladder_judged(std::span<const svooc::Responder::Ask> asks) {
    if (asks.size() < kLadder) return false;
    return asks[asks.size() - 1].after >= svooc::kAcceptPolls;
}

// What the stage knows of the trigger at one poll.
struct Live {
    bool running = false;          // svooc_trig::running()
    bool judged = false;           // ladder_judged() of the asks
    std::uint8_t abort = 0;        // svooc::Abort of the report
    std::uint8_t commands = 0;     // saturating: did the adapter say anything at all
    std::uint32_t quiet_ms = 0;    // ms since the adapter's last D+/D- edge of this wake (0 before it spoke)
};

// The stage's ending for one poll: End::None while it goes on. `elapsed_ms` since the start. A running session that is
// judged or past the cap is asked to stop by the caller (the trigger's exit watch then ends it); once it is not running the
// ending is read from why the trigger stopped.
constexpr End end_of(const Live& l, std::uint32_t elapsed_ms) {
    if (l.running) {
        if (l.judged) return End::Judged;
        if (l.commands > 0 && l.quiet_ms >= kQuietMs) return End::Stopped;   // the adapter ended the session: stop the trigger
        if (elapsed_ms >= kCapMs + kExitGraceMs) return End::Cap;
        return End::None;
    }
    if (l.judged) return End::Judged;
    const auto a = static_cast<svooc::Abort>(l.abort);
    if (a == svooc::Abort::NoAdapter || (a == svooc::Abort::None && l.commands == 0)) return End::NoAdapter;
    if (a == svooc::Abort::TimeCap) return End::Cap;
    if (a == svooc::Abort::Stopped) return End::Cancelled;
    return End::Aborted;
}

// What the stage recorded (charger_test::Evidence): the highest ask the adapter went on under, how it ended, the highest VBUS
// the trigger saw (mV), when the adapter's last edge came (ms after the wake) and, for an abort, its cause (svooc::Abort).
struct Outcome {
    std::uint16_t word = 0;
    End end = End::None;
    std::uint16_t vmax_mv = 0;
    std::uint16_t stop_ms = 0;
    std::uint8_t why = 0;
};

// An abort's cause in at most 10 characters for the Results row (svooc::name(Abort) is the log's longer form).
constexpr const char* abort_word(std::uint8_t why) {
    switch (static_cast<svooc::Abort>(why)) {
    case svooc::Abort::None: return "";
    case svooc::Abort::Overvoltage: return "over 12 V";
    case svooc::Abort::Current: return "current";
    case svooc::Abort::OffScript: return "off-script";
    case svooc::Abort::HostGone: return "host gone";
    case svooc::Abort::NoMonitor: return "no monitor";
    case svooc::Abort::Sink: return "sink";
    case svooc::Abort::PdContract: return "PD";
    case svooc::Abort::Storm: return "edge storm";
    case svooc::Abort::NoAdapter: return "no adapter";
    case svooc::Abort::TimeCap: return "time cap";
    case svooc::Abort::Stopped: return "stopped";
    case svooc::Abort::ContractLost: return "PD lost";
    case svooc::Abort::ContractChanged: return "PD changed";
    }
    return "?";
}

// The Results row's words. The ask is what the K2 claimed in the protocol (100 mA per count), never a drawn current:
//   Stopped: "stopped at 3.3 s, 4.0 A claimed, 9.1 V max" (the adapter's last edge, the ask it went on under, the top VBUS);
//   Aborted: "aborted: sink, none claimed, 8.0 V max" (the cause's short word; "aborted, ..." with none recorded);
//   any other run: "10.9 A claimed, 11.1 V max" ("none claimed" without an accepted ask; no VBUS part when none was read);
//   "refused" / "no adapter" when nothing ran, "-" while it runs. Tenths are rounded. No stdio on the device: digits by hand.
// Returns the length; a short buffer cuts the text (always terminated).
inline std::size_t format_cap(const Outcome& c, std::span<char> out) {
    if (out.empty()) return 0;
    char tmp[48] = {};
    std::size_t n = 0;
    const auto put = [&](const char* s) { while (*s && n + 1 < sizeof tmp) tmp[n++] = *s++; };
    const auto tenths = [&](std::uint32_t t) {   // "12.5"
        char d[8];
        std::size_t k = 0;
        std::uint32_t whole = t / 10u;
        do { d[k++] = char('0' + whole % 10u); whole /= 10u; } while (whole && k < sizeof d);
        while (k && n + 1 < sizeof tmp) tmp[n++] = d[--k];
        if (n + 3 < sizeof tmp) { tmp[n++] = '.'; tmp[n++] = char('0' + t % 10u); }
    };
    if (c.end == End::Refused) put("refused");
    else if (c.end == End::NoAdapter) put("no adapter");
    else if (c.end == End::None) put("-");
    else {
        if (c.end == End::Stopped) { put("stopped at "); tenths((std::uint32_t(c.stop_ms) + 50u) / 100u); put(" s, "); }
        if (c.end == End::Aborted) {
            const char* w = abort_word(c.why);
            if (*w) { put("aborted: "); put(w); put(", "); } else put("aborted, ");
        }
        if (c.word) { tenths(ceiling_ma(c.word) / 100u); put(" A claimed"); }
        else put("none claimed");
        if (c.vmax_mv) { put(", "); tenths((std::uint32_t(c.vmax_mv) + 50u) / 100u); put(" V max"); }
    }
    n = n < out.size() - 1 ? n : out.size() - 1;
    for (std::size_t i = 0; i < n; ++i) out[i] = tmp[i];
    out[n] = '\0';
    return n;
}

}  // namespace k2::svooc_ceiling
