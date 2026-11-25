// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// What latched the inline state (pd::Link::inline_cause()), kept apart from the Link class so pure logic that only
// needs the name or the topology question (the SuperVOOC guards, svooc_run.hpp) does not pull in the whole tracker.
#pragma once
#include <cstdint>
#include "k2/core/strpack.hpp"

namespace k2::pd {

// What latched the inline state (Link::inline_cause()): the [TEST] preflight refusal names it. Only Load and Request are
// ever set (user decision 2026-11-15: the inline latch keeps certain evidence only). Rise, Vbus and FarRd are retired: the
// values stay for the wire (remote summaries, journals) and the names of old records, and nothing sets them.
enum class InlineCause : std::uint8_t { None, Load, Request, Rise, Vbus, FarRd };
constexpr const char* inline_cause_name(InlineCause c) {
    switch (c) {
        case InlineCause::None: return "none";
        case InlineCause::Load: return "load";
        case InlineCause::Request: return K2_P("sink request");
        case InlineCause::Rise: return K2_P("vbus rise");
        case InlineCause::Vbus: return K2_P("foreign vbus");
        case InlineCause::FarRd: return K2_P("far rd");
    }
    return "?";
}

}  // namespace k2::pd}  // namespace k2::pd
