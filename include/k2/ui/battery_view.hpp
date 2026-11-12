// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Battery place (Bench > Battery), pure half: the numbers as the screen writes them, the rows of each recorder state and
// the texts of the identity block and the footer. BatteryPage (k2/ui/ui_battery.hpp) paints them; the Bench row reads the
// same formatters. Host-tested in tests/test_ui_battery.cpp.
#pragma once
#include <cstdint>
#include <string_view>

#include "k2/battery/battery.hpp"
#include "k2/ui/kit.hpp"

namespace k2::battery::view {

// A short number with its unit, built in place.
struct Num {
    char b[14] = {};
    uint8_t n = 0;
    std::string_view view() const { return {b, n}; }
};
Num wh(uint64_t uwh);        // "9.999Wh" under 10 Wh, "99.99Wh" under 100, "999.9Wh" under 1000, "1234Wh" (cut, never rounded up)
Num mah(uint64_t uah);     // "2000mAh" up to 99999 mAh, then "123.4Ah"
Num hms(uint32_t s);         // "1h05m12s", "12m34s", "45s"
Num hm(uint32_t s);          // "1h05m", "12m", "0m"
Num pct(uint16_t permille);  // "80.0%"

// What the page shows: the run in RAM, or a stored one (Runs > a run). Built from the record form so both read the same.
struct Shown {
    bool any = false;            // a run (the recorder has run since boot, or a stored run is shown)
    RunState state = RunState::Done;
    Mode mode = Mode::Charge;
    Wiring wiring = Wiring::SamePort;
    int phases = 0;
    Config cfg{};
    Kind kind = Kind::Charge;    // the last phase's
    End end = End::Running;      // the last phase's
    bool lost = false;           // a phase was cut by a power loss
    uint16_t id = 0;
    uint16_t gaps = 0;
    uint32_t secs = 0, phase_secs = 0;
    uint32_t phase_uwh = 0;      // the last phase's energy in its own direction (in for a charge, out for a discharge)
    Sums sum{};
    uint16_t eff = kNoRatio;     // the round trip's ratio: so far while its discharge runs, final when done
    bool eff_final = false;
};
Shown shown(const Record &r, bool any);

enum class Act : uint8_t {
    In, Out, Time, Eff,                                   // facts (no verb)
    StartCharge, StartDischarge, StartRoundTrip,          // idle or done
    EndPhase, Next, Finish,                               // a run in progress
    Wiring, EndMa, EndS,                                  // the settings
    Runs, Share,                                          // the stored runs, the QR of the shown run
};
inline constexpr int kMaxRows = 12;
struct Rows {
    Act a[kMaxRows] = {};
    int n = 0;
    int facts = 0;               // the first `facts` rows are facts; the actions follow
};
// The main view (the run in RAM: facts when there is one, then what can be done now), or a stored run (`stored`: its facts
// and Share).
Rows rows(const Shown &s, bool stored);
bool is_fact(Act a);
const char *act_name(Act a, const Shown &s);   // the row text (font 16, <= 18 characters)
const char *act_verb(Act a);                   // the OK label of an action row ("" for a fact)

// The settings' steps: OK on the row moves to the next one (wrapping).
inline constexpr uint16_t kEndMaSteps[] = {20, 50, 100, 200, 500};
inline constexpr uint16_t kEndSSteps[] = {10, 30, 60, 120, 300, 600};
uint16_t next_step(const uint16_t *steps, int n, uint16_t cur);

// The Bench row's line (font 12, <= 28 characters): "Charging 1h05m, 12.34Wh", "Charged 12.34Wh, waiting",
// "Round trip 80.0%", "Discharged 9.87Wh", "Cut short, 12.34Wh". Nothing for no run (the row says "No run").
void bench_status(const Shown &s, StrBuf<29> &out);

// The identity block of the main view or a stored run.
void ident(const Shown &s, kit::Ident &out);
// The footer of the main view (a refusal or an armed confirm takes it first, BatteryPage).
FooterTone footer(const Shown &s, bool monitor, bool write_failed, bool unwritten, StrBuf<28> &out);

}  // namespace k2::battery::view
