// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// What latched the inline state (pd::Link::inline_cause()), kept apart from the Link class so pure logic that only
// needs the name or the topology question (the SuperVOOC guards, svooc_run.hpp) does not pull in the whole tracker.
#pragma once
#include <cstdint>

namespace k2::pd {

// What latched the inline state (Link::inline_cause()): the [TEST] preflight refusal names it.
enum class InlineCause : std::uint8_t { None, Load, Request, Rise, Vbus, FarRd };
constexpr const char* inline_cause_name(InlineCause c) {
    switch (c) {
        case InlineCause::None: return "none";
        case InlineCause::Load: return "load";
        case InlineCause::Request: return "sink request";
        case InlineCause::Rise: return "vbus rise";
        case InlineCause::Vbus: return "foreign vbus";
        case InlineCause::FarRd: return "far rd";
    }
    return "?";
}

// The two causes the topology explains on its own (pd_link.cpp): FarRd = a sink's Rd on CC with none of ours while a source feeds
// the link; Vbus = VBUS above 5.5 V with no contract of the K2's. Load (|I| over 50 mA), Request (a foreign sink's PD Request)
// and Rise (VBUS rose above 5.5 V after being low) are evidence of a device behind the K2, and a latch with no cause is not
// known to be topology: none of those is ever waived. The SuperVOOC USB-A waiver and the operator's VOOC override both skip
// only a latch for which this is true.
constexpr bool topology_only(InlineCause c) { return c == InlineCause::FarRd || c == InlineCause::Vbus; }

}  // namespace k2::pd
