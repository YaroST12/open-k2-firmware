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
// Cancelled = the run was stopped.
enum class End : std::uint8_t { None, Refused, NoAdapter, Aborted, Cap, Judged, Cancelled };
inline constexpr std::uint8_t kEndCount = 7;

inline constexpr std::uint32_t kCapS = 12;                     // the trigger's own run cap for the stage
inline constexpr std::uint32_t kCapMs = kCapS * 1000;
inline constexpr std::uint32_t kExitGraceMs = 6000;            // after the stop: how long the exit watch may still hold the lines
inline constexpr std::size_t kLadder = 4;                      // rungs of the Probe ladder

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
};

// The stage's ending for one poll: End::None while it goes on. `elapsed_ms` since the start. A running session that is
// judged or past the cap is asked to stop by the caller (the trigger's exit watch then ends it); once it is not running the
// ending is read from why the trigger stopped.
constexpr End end_of(const Live& l, std::uint32_t elapsed_ms) {
    if (l.running) {
        if (l.judged) return End::Judged;
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

// The Results row's words: "6.3 A asked" (the ceiling as the adapter's accepted ask), "none" when no ask was accepted,
// "refused" / "no adapter" when nothing ran. No stdio on the device: digits are written by hand. Returns the length.
inline std::size_t format_cap(std::uint16_t word, End end, std::span<char> out) {
    if (out.empty()) return 0;
    char tmp[16] = {};
    std::size_t n = 0;
    const auto put = [&](const char* s) { while (*s && n + 1 < sizeof tmp) tmp[n++] = *s++; };
    if (word) {
        const std::uint32_t tenths = ceiling_ma(word) / 100u;   // 0.1 A units
        const std::uint32_t whole = tenths / 10u;
        if (whole >= 10) tmp[n++] = char('0' + whole / 10);
        tmp[n++] = char('0' + whole % 10);
        tmp[n++] = '.';
        tmp[n++] = char('0' + tenths % 10);
        put(" A asked");
    } else if (end == End::Refused) put("refused");
    else if (end == End::NoAdapter) put("no adapter");
    else if (end == End::None) put("-");
    else put("none");
    n = n < out.size() - 1 ? n : out.size() - 1;
    for (std::size_t i = 0; i < n; ++i) out[i] = tmp[i];
    out[n] = '\0';
    return n;
}

}  // namespace k2::svooc_ceiling
