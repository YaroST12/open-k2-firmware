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
inline constexpr const char *kNames[kSlots] = {K2_P("FC trigger"), K2_P("PD trigger"),   K2_P("Spoof"),        K2_P("Cable R"), K2_P("Charger ID"),
                                               K2_P("Cable read"), K2_P("Device ID"),    K2_P("Black Box"),    K2_P("PD diagnosis"), K2_P("Report cards"),
                                               "Diag",       K2_P("Device twins"), K2_P("Loop probe"),   K2_P("PD Stress")};
static_assert(all_fit_p(kNames, kSmallAdvPx, 130), "a Tools name is wider than its row");

// The focused row's note: what the tool needs, port-neutral (never says which port).
inline constexpr const char *kHints[kSlots] = {
    K2_P("charger on top or bottom"), K2_P("PD charger, either port"), K2_P("act as a saved cable"), K2_P("charger + cable + load"),
    K2_P("what the charger offers"),  K2_P("e-marker of a cable"),     K2_P("what a device asks for"), K2_P("passive PD recorder"),
    K2_P("passive PD verdict"),       K2_P("saved charger results"),
    K2_P("USB, PD and system state"), K2_P("will it charge my device"), K2_P("cable into own HID port"), K2_P("PD charger, no load")};
static_assert(all_fit_p(kHints, kLabelAdv, kit::kNoteMaxPx), "a Tools note is wider than its line");

// Tails of a row that cannot do its full job now (lower case, kDim: only the footer carries the tone).
inline constexpr const char *kTagReady = K2_P("ready"), *kTagLoad = "load", *kTagNoPd = K2_P("no PD"), *kTagNoPm = K2_P("no PM");
inline constexpr const char *kTags[] = {kTagReady, kTagLoad, kTagNoPd, kTagNoPm};
static_assert(all_fit_p(kTags, kSmallAdvPx, 48), "a Tools tail is wider than 6 characters");

// Footers (at most 27 characters): the blocker of the focused tool, or the hub's fact.
inline constexpr const char *kLoadAttached = K2_P("Load attached: unplug it");
inline constexpr const char *kLiveSink = K2_P("Live sink: unplug load");
inline constexpr const char *kNoPd = K2_P("No PD source on either port");
inline constexpr const char *kNoPm = K2_P("Power meter not found");
inline constexpr const char *kAllReady = K2_P("All tools ready");
inline constexpr const char *kSomeReady = " of ";    // "8 of 11 tools ready"
inline constexpr const char *kSomeReadyTail = K2_P(" tools ready");
inline constexpr const char *kFooters[] = {kLoadAttached, kLiveSink, kNoPd, kNoPm, kAllReady, K2_P("14 of 14 tools ready")};
static_assert(all_fit_p(kFooters, kSmallAdvPx, 27 * kSmallAdvPx), "a Tools footer is over 27 characters");

// PD limit: the last Tools row, its per-tech picker and their results. A note is at most 29 cells; a toast 30.
inline constexpr const char *kLimName = K2_P("PD limit"), *kLimOff = "Off", *kLimOn = "On", *kLimMax = "Max";
inline constexpr const char *kLimFixed = K2_P("Fixed"), *kLimPps = "PPS", *kLimAvs = K2_P("SPR AVS"), *kLimApply = K2_P("Apply");
inline constexpr const char *kLimRowMode = "Mode", *kLimRowMaxV = K2_P("Max V"), *kLimRowMaxA = K2_P("Max A");
inline constexpr const char *kLimNoteOff = K2_P("limits PD Fixed, PPS or AVS");   // the row with no limit set
inline constexpr const char *kLimNoteNone = K2_P("no limit, every offer shown");  // the picker's Off
inline constexpr const char *kLimNoteAbove = K2_P("limit active: ");              // then e.g. "9V3A" / "27W"
inline constexpr const char *kLimNotePps = K2_P("PPS voltage range and A");
inline constexpr const char *kLimNoteAvs = K2_P("SPR AVS 15/20V and A");
inline constexpr const char *kLimNoteApply = K2_P("arms selected PD limit");
inline constexpr const char *kLimNoteNoOffer = K2_P("not offered by charger");
inline constexpr const char *kLimNoteMode = K2_P("include in forged offers");
inline constexpr const char *kLimNoteStepV = K2_P("pick max voltage");
inline constexpr const char *kLimNoteStepA = K2_P("pick max current");
inline constexpr const char *kLimNoteAmpsAt = "at ", *kLimNoteAnyV = K2_P("any voltage");
inline constexpr const char *kLimNotes[] = {kLimNoteOff, kLimNoteNone, kLimNotePps, kLimNoteAvs, kLimNoteApply,
                                            kLimNoteNoOffer, kLimNoteMode, kLimNoteStepV, kLimNoteStepA};
static_assert(all_fit_p(kLimNotes, kLabelAdv, kit::kNoteMaxPx), "a PD limit note is wider than its line");
static_assert(fits_px_p(kLabelAdv, kLimNoteAbove, kit::kNoteMaxPx - 5 * static_cast<int>(kLabelAdv)), "the PD limit note plus label is over its line");
inline constexpr const char *kLimToastNoOffers = K2_P("No charger offers yet");
inline constexpr const char *kLimToastNoLimit = K2_P("Set a limit first");
inline constexpr const char *kLimToastKeepOne = K2_P("Keep one PD mode On");
inline constexpr const char *kLimToastBadLimit = K2_P("Limit out of range");
inline constexpr const char *kLimToastBusy = K2_P("Port in use: stop it first");
inline constexpr const char *kLimToastRate = K2_P("Limit off: too many retries");
inline constexpr const char *kLimToastChargerResets = K2_P("Limit off: charger resets");
inline constexpr const char *kLimToastSinkResets = K2_P("Limit off: sink resets");
inline constexpr const char *kLimToastSinkIgnores = K2_P("Limit off: sink ignores it");
inline constexpr const char *kLimToastEpr = K2_P("Limit off: EPR contract");
// A live contract is reset before a new limit (or Off): the same two-press window as Reset & re-read on LINK.
inline constexpr const char *kLimToastConfirm = "OK again: resets link";
inline constexpr const char *kLimToastResetting = K2_P("Resetting link...");
inline constexpr const char *kLimToastRescan = K2_P("Rescanning...");
inline constexpr const char *kLimToastNoReset = K2_P("No reset: replug to apply");
inline constexpr const char *kLimToastSelfPowered = K2_P("Power K2 via USB port");
inline constexpr const char *kLimToastRetry = K2_P("Busy, retry");
inline constexpr const char *kLimToastNoLink = K2_P("No active link");
inline constexpr const char *kLimToastNoFit = K2_P("Fixed Off: not on charger");   // a fixed object before the APDOs
inline constexpr const char *kLimToastChargerRefuses = K2_P("Limit off: charger refuses");   // it will not take a re-offered Request
inline constexpr const char *kLimRowReset = K2_P("Reset link");                       // the picker's row when no offers are known
inline constexpr const char *kLimNoteReset = K2_P("reads the charger's offers");
inline constexpr const char *kLimToasts[] = {kLimToastNoOffers, kLimToastNoLimit, kLimToastKeepOne, kLimToastBadLimit, kLimToastBusy, kLimToastRate,
                                             kLimToastChargerResets, kLimToastSinkResets, kLimToastSinkIgnores, kLimToastEpr,
                                             kLimToastConfirm, kLimToastResetting, kLimToastRescan, kLimToastNoReset,
                                             kLimToastSelfPowered, kLimToastRetry, kLimToastNoLink, kLimToastNoFit,
                                             kLimToastChargerRefuses};
static_assert(fits_px_p(kLabelAdv, kLimNoteReset, kit::kNoteMaxPx), "the reset row note is wider than its line");
static_assert(all_fit_p(kLimToasts, kLabelAdv, kToastTextPx), "a PD limit result is wider than the toast");

}  // namespace k2::text::tools
