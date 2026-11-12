// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools hub copy: every string the hub shows, in one place, each table checked against the box it is drawn into.
// Layout and rules: docs/UI_SYSTEM.md 6 (copy) and 7.4 / 7.13.3 (blocked rows: a dim lower-case tail, the footer alone red).
#pragma once
#include "k2/ui/font_aa.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui_labels.hpp"

namespace k2::text::tools {

inline constexpr int kSlots = 12;   // kToolSlots (ui_tools.hpp); PD Stress is the last slot

// Row names. A row is the 12 px icon, the name at x28, and the tail (at most 6 characters, 48 px) at the right: 130 px.
inline constexpr const char *kNames[kSlots] = {"FC trigger",   "PD trigger", "Spoof", "Cable R",      "Who's attached", "Black Box",
                                               "PD diagnosis", "Report cards", "Diag", "Device twins", "Loop probe",     "PD Stress"};
static_assert(all_fit(kNames, kSmallAdvPx, 130), "a Tools name is wider than its row");

// The focused row's note: what the tool needs, port-neutral (never says which port).
inline constexpr const char *kHints[kSlots] = {
    "charger on top or bottom", "PD charger, either port", "act as a saved cable", "charger + cable + load",
    "source, cable and device", "passive PD recorder",     "passive PD verdict",   "saved charger results",
    "USB, PD and system state", "will it charge my device", "cable into own HID port", "PD charger, no load"};
static_assert(all_fit(kHints, kLabelAdv, kit::kNoteMaxPx), "a Tools note is wider than its line");

// Tails of a row that cannot do its full job now (lower case, kDim: only the footer carries the tone).
inline constexpr const char *kTagReady = "ready", *kTagLoad = "load", *kTagNoPd = "no PD", *kTagNoPm = "no PM";
inline constexpr const char *kTags[] = {kTagReady, kTagLoad, kTagNoPd, kTagNoPm};
static_assert(all_fit(kTags, kSmallAdvPx, 48), "a Tools tail is wider than 6 characters");

// Footers (at most 27 characters): the blocker of the focused tool, or the hub's fact.
inline constexpr const char *kLoadAttached = "Load attached: unplug it";
inline constexpr const char *kLiveSink = "Live sink: unplug load";
inline constexpr const char *kNoPd = "No PD source on either port";
inline constexpr const char *kNoPm = "Power meter not found";
inline constexpr const char *kAllReady = "All tools ready";
inline constexpr const char *kSomeReady = " of ";    // "8 of 11 tools ready"
inline constexpr const char *kSomeReadyTail = " tools ready";
inline constexpr const char *kFooters[] = {kLoadAttached, kLiveSink, kNoPd, kNoPm, kAllReady, "12 of 12 tools ready"};
static_assert(all_fit(kFooters, kSmallAdvPx, 27 * kSmallAdvPx), "a Tools footer is over 27 characters");

}  // namespace k2::text::tools
