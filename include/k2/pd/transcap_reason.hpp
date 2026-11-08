// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
// Which transition-capture trigger a failed charger-test row maps to. Pure: the page stores a capture for every row
// that maps to something other than None. A failure reason missing here silently loses the capture: the C135 3.3 V
// row ended "no PD after re-attach" (NoReattach) with the capture frozen in RAM and nothing in flash (hardware 2026-11-03).
#include "k2/chargertest/charger_test.hpp"
#include "k2/pd/transcap_calc.hpp"

namespace k2::charger_test {

constexpr transcap::Reason capture_reason(Reason r) {
    switch (r) {
    case Reason::ShutOff:
    case Reason::CutRecovered: return transcap::Reason::ShutOff;   // the source cut VBUS (it may have come back by itself)
    case Reason::Timeout: return transcap::Reason::Timeout;
    case Reason::HardReset: return transcap::Reason::HardReset;
    case Reason::Request: return transcap::Reason::NoPsRdy;
    case Reason::Lost: return transcap::Reason::Detach;
    case Reason::Voltage: return transcap::Reason::Voltage;
    case Reason::NoReattach:                                       // no PD after the re-attach: the cut's trace is the evidence
    case Reason::BlockedByPd: return transcap::Reason::Other;      // a Legacy attach that never fell back: the PD chatter is the evidence
    case Reason::NoQc3Plus:                                        // a skip, not a failure: no capture
    case Reason::ClassAOnly:                                       // a skip, not a failure: no capture
    case Reason::NotUnlocked:                                      // a skip, not a failure: no capture
    default: return transcap::Reason::None;
    }
}

}  // namespace k2::charger_test
