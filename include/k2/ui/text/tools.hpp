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

inline constexpr int kSlots = 14;   // kToolSlots (ui_tools.hpp); Loop probe and PD Stress are the last two slots

// Row names. A row is the 12 px icon, the name at x28, and the tail (at most 6 characters, 48 px) at the right: 130 px.
inline constexpr const char *kNames[kSlots] = {"FC trigger", "PD trigger",   "Spoof",        "Cable R", "Charger ID",
                                               "Cable read", "Device ID",    "Black Box",    "PD diagnosis", "Report cards",
                                               "Diag",       "Device twins", "Loop probe",   "PD Stress"};
static_assert(all_fit(kNames, kSmallAdvPx, 130), "a Tools name is wider than its row");

// The focused row's note: what the tool needs, port-neutral (never says which port).
inline constexpr const char *kHints[kSlots] = {
    "charger on top or bottom", "PD charger, either port", "act as a saved cable", "charger + cable + load",
    "what the charger offers",  "e-marker of a cable",     "what a device asks for", "passive PD recorder",
    "passive PD verdict",       "saved charger results",
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
inline constexpr const char *kFooters[] = {kLoadAttached, kLiveSink, kNoPd, kNoPm, kAllReady, "14 of 14 tools ready"};
static_assert(all_fit(kFooters, kSmallAdvPx, 27 * kSmallAdvPx), "a Tools footer is over 27 characters");

// PD limit: the last Tools row, its per-tech picker and their results. A note is at most 29 cells; a toast 30.
inline constexpr const char *kLimName = "PD limit", *kLimOff = "Off", *kLimOn = "On", *kLimMax = "Max";
inline constexpr const char *kLimFixed = "Fixed", *kLimPps = "PPS", *kLimAvs = "SPR AVS", *kLimApply = "Apply";
inline constexpr const char *kLimRowMode = "Mode", *kLimRowMaxV = "Max V", *kLimRowMaxA = "Max A";
inline constexpr const char *kLimNoteOff = "limits PD Fixed, PPS or AVS";   // the row with no limit set
inline constexpr const char *kLimNoteNone = "no limit, every offer shown";  // the picker's Off
inline constexpr const char *kLimNoteAbove = "limit active: ";              // then e.g. "9V3A" / "27W"
inline constexpr const char *kLimNoteFixed = "fixed 5-20V offers";
inline constexpr const char *kLimNotePps = "PPS voltage range and A";
inline constexpr const char *kLimNoteAvs = "SPR AVS 15/20V and A";
inline constexpr const char *kLimNoteApply = "arms selected PD limit";
inline constexpr const char *kLimNoteNoOffer = "not offered by charger";
inline constexpr const char *kLimNoteMode = "include in forged offers";
inline constexpr const char *kLimNoteStepV = "pick max voltage";
inline constexpr const char *kLimNoteStepA = "pick max current";
inline constexpr const char *kLimNoteAmpsAt = "at ", *kLimNoteAnyV = "any voltage";
inline constexpr const char *kLimNotes[] = {kLimNoteOff, kLimNoteNone, kLimNoteFixed, kLimNotePps, kLimNoteAvs, kLimNoteApply,
                                            kLimNoteNoOffer, kLimNoteMode, kLimNoteStepV, kLimNoteStepA};
static_assert(all_fit(kLimNotes, kLabelAdv, kit::kNoteMaxPx), "a PD limit note is wider than its line");
static_assert(fits_px(kLabelAdv, kLimNoteAbove, kit::kNoteMaxPx - 5 * static_cast<int>(kLabelAdv)), "the PD limit note plus label is over its line");
inline constexpr const char *kLimToastNoOffers = "No charger offers yet";
inline constexpr const char *kLimToastNoLimit = "Set a limit first";
inline constexpr const char *kLimToastKeepOne = "Keep one PD mode On";
inline constexpr const char *kLimToastBadLimit = "Limit out of range";
inline constexpr const char *kLimToastBusy = "Port in use: stop it first";
inline constexpr const char *kLimToastRate = "Limit off: too many retries";
inline constexpr const char *kLimToastChargerResets = "Limit off: charger resets";
inline constexpr const char *kLimToastSinkResets = "Limit off: sink resets";
inline constexpr const char *kLimToastSinkIgnores = "Limit off: sink ignores it";
inline constexpr const char *kLimToastEpr = "Limit off: EPR contract";
// A live contract is reset before a new limit (or Off): the same two-press window as Reset & re-read on LINK.
inline constexpr const char *kLimToastConfirm = "OK again: resets link";
inline constexpr const char *kLimToastResetting = "Resetting link...";
inline constexpr const char *kLimToastRescan = "Rescanning...";
inline constexpr const char *kLimToastNoReset = "No reset: replug to apply";
inline constexpr const char *kLimToastSelfPowered = "Power K2 via USB port";
inline constexpr const char *kLimToastRetry = "Busy, retry";
inline constexpr const char *kLimToastNoLink = "No active link";
inline constexpr const char *kLimToastNoFit = "Fixed Off: not on charger";   // a fixed object before the APDOs
inline constexpr const char *kLimToastChargerRefuses = "Limit off: charger refuses";   // it will not take a re-offered Request
inline constexpr const char *kLimRowReset = "Reset link";                       // the picker's row when no offers are known
inline constexpr const char *kLimNoteReset = "reads the charger's offers";
inline constexpr const char *kLimToasts[] = {kLimToastNoOffers, kLimToastNoLimit, kLimToastKeepOne, kLimToastBadLimit, kLimToastBusy, kLimToastRate,
                                             kLimToastChargerResets, kLimToastSinkResets, kLimToastSinkIgnores, kLimToastEpr,
                                             kLimToastConfirm, kLimToastResetting, kLimToastRescan, kLimToastNoReset,
                                             kLimToastSelfPowered, kLimToastRetry, kLimToastNoLink, kLimToastNoFit,
                                             kLimToastChargerRefuses};
static_assert(fits_px(kLabelAdv, kLimNoteReset, kit::kNoteMaxPx), "the reset row note is wider than its line");
static_assert(all_fit(kLimToasts, kLabelAdv, kToastTextPx), "a PD limit result is wider than the toast");

}  // namespace k2::text::tools
