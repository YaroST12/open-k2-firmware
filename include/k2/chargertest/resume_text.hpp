// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The words of the charger test's Resume stage (pure, host-tested in tests/test_resume_text.cpp).
//
// The stage shows why a run is waiting: a reboot (or a lost VBUS on a self-powered K2) interrupted it and it goes on by itself
// once VBUS is steady. "Self-powered" is a fact about the supply NOW, never about the run that was journaled: a run begun on a
// self-powered K2 that comes back on a host-powered one (the Baseus 70W report, 2026-11-07) is a plain reboot.
// Every string fits its field on the kit list (a 16 px row text beside the 12 px marker column leaves 23 cells, the focused
// 12 px note 31 cells, the footer 27 cells): status 20 cells, row 23 cells, note 31 cells, footer 27 cells.
#pragma once
#include <string_view>

namespace k2::resumetext {

inline constexpr unsigned kStatusMax = 20, kRowMax = 23, kNoteMax = 31, kFooterMax = 27;

struct In {
    bool self_now = false;       // the K2 is self-supplied now (pinned Rd, or bus-powered)
    bool gate_ready = false;     // VBUS has been steady (4.75 - 5.5 V, 0.5 s)
    bool rd_on = false;          // this resume presented the Rd (host-powered K2) and the charger wait goes on
    bool wait_over = false;      // that wait has ended (the charger is up, or it gave up)
    bool behind_cable = false;   // the journaled identity waits for a charger directly on the plug
    bool held = false;           // the identity or the Rd could not go on: OK asks again
};

constexpr std::string_view status(const In& in) { return in.self_now ? "Self-powered, paused" : "Rebooted mid-run"; }

constexpr std::string_view row(const In& in) {
    if (in.gate_ready) return "VBUS 5V stable: ready";
    if (in.held) return "Press OK to try again";
    if (in.behind_cable) return "Needs a direct plug";
    if (in.rd_on && !in.wait_over) return "Rd on, awaiting charger";
    return "Waiting for stable 5V";
}

constexpr std::string_view note(const In& in) {
    if (in.gate_ready) return "Resumes at 5V, never raises";
    return in.rd_on ? "Rd on; 5V steady for 0.5s" : "Waiting for 5V, steady 0.5s";
}

constexpr std::string_view footer(const In& in) {
    return in.held ? "Held. Results are kept" : "Auto-resumes; results kept";   // a fact, no key names; 27-cell footer
}

}  // namespace k2::resumetext
