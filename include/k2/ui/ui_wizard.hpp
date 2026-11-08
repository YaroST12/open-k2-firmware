// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure model behind the PD trigger step wizard (host-tested in tests/test_ui_wizard.cpp). No
// drawing, no hardware. One decision per screen: OFFER (which PDO), VOLTAGE (PPS / AVS only),
// CURRENT, CONFIRM, then LIVE (the contract after the request went out).
//
// Keys: [+] / [-] step the value by the hardware step, a long press steps coarsely (the button
// layer fires Long once, it never repeats, so a coarse step is what "hold" means). OK moves on,
// BACK moves one step back and leaves the wizard from OFFER.
#pragma once
#include <cstdint>

#include "k2/pd/pd.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2 {

enum class WizStep : uint8_t { Offer, Voltage, Current, Confirm, Live };

struct Wizard {
    enum class Ok : uint8_t { Stay, Next, Send };

    static constexpr uint16_t kPpsStepMv = 20, kAvsStepMv = 100, kCoarseMv = 1000;
    static constexpr uint16_t kStepMa = 50, kCoarseMa = 500;

    WizStep step = WizStep::Offer;
    uint8_t cursor = 0;         // 0-based index into the PDO list
    uint16_t mv = 0, ma = 0;    // chosen voltage (PPS / AVS) and current
    bool adj = false;           // the chosen offer has a voltage step (PPS / AVS)

    void enter(const pd::SourceCaps &caps, uint8_t held_pos);   // OFFER, cursor on the held contract (1-based), else PDO 1
    bool nudge(const pd::SourceCaps &caps, int dir, bool coarse, uint32_t cap_mv);   // true when the value changed
    Ok ok(const pd::SourceCaps &caps, uint32_t cap_mv);
    void sent() { step = WizStep::Live; }
    bool back();                // true = leave the wizard (BACK on OFFER)

    bool adjustable(const pd::SourceCaps &caps) const;   // cursor on a PPS / AVS PDO
    static bool offer_available(const pd::SourceCaps &caps, unsigned idx, uint32_t cap_mv);   // can this PDO be requested
    uint16_t volt_step_mv(const pd::SourceCaps &caps) const;
    int total(const pd::SourceCaps &caps) const;         // 4 for PPS / AVS, 3 for fixed
    int number(const pd::SourceCaps &caps) const;        // 1-based step number
    pd::Choice choice(const pd::SourceCaps &caps) const; // mv only for PPS / AVS, ma always
    void title(const pd::SourceCaps &caps, StrBuf<24> &out) const;   // "Step 2/4: VOLTAGE", "LIVE"
    void footer(const pd::SourceCaps &caps, uint32_t cap_mv, bool armed, StrBuf<32> &out) const;   // a fact about the step, never the keys

    void volt_range(const pd::SourceCaps &caps, uint32_t cap_mv, uint32_t &lo, uint32_t &hi) const;   // grid-aligned, below the cap
    uint16_t current_limit(const pd::SourceCaps &caps, uint32_t cap_mv) const;   // the offer's maximum for this voltage
};

// Range labels: "3.3V", "11V", "0.5A" (milli units, rounded to a tenth, no trailing ".0").
void put_short(StrBuf<8> &out, uint32_t milli, char unit);

// What the CONFIRM card shows, built from the plan that would be sent so the card and the request agree.
struct WizSummary {
    pd::Err err = pd::Err::NoCaps;
    bool needs_confirm = false;      // above 5 V: OK has to be pressed twice
    StrBuf<4> kind;                  // FIX / PPS / AVS
    StrBuf<8> volts, amps, watts;    // "9.00V" "1.50A" "13.5W"
    StrBuf<8> offer_w;               // what the source offers on this PDO: "55W"
    StrBuf<40> line;                 // "Request PPS 9.00V 1.50A = 13.5W"
    uint32_t req_mw = 0, offer_mw = 0;
};
bool wiz_summary(const pd::SourceCaps &caps, pd::Choice c, uint32_t cap_mv, WizSummary &out);   // false: no such PDO

// The contract as the LIVE card words it.
enum class LiveState : uint8_t { Requesting, Accepted, PsRdy, Rejected, HardReset, Failed, Dropping, Released };
LiveState live_state(const pd::SinkStatus &s);
const char *live_name(LiveState s);
bool live_good(LiveState s);
bool live_bad(LiveState s);

}  // namespace k2
