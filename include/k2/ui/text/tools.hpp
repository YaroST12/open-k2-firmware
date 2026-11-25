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
inline constexpr std::string_view kLimName = K2_PSV("PD limit");
inline constexpr const char *kLimOff = "Off", *kLimOn = "On", *kLimMax = "Max";
inline constexpr std::string_view kLimFixed = K2_PSV("Fixed"), kLimPps = "PPS", kLimAvs = K2_PSV("SPR AVS"), kLimApply = K2_PSV("Apply");
inline constexpr std::string_view kLimRowMode = "Mode", kLimRowMaxV = K2_PSV("Max V"), kLimRowMaxA = K2_PSV("Max A");
inline constexpr std::string_view kLimNoteOff = K2_PSV("limits PD Fixed, PPS or AVS");   // the row with no limit set
inline constexpr std::string_view kLimNoteNone = K2_PSV("no limit, every offer shown");  // the picker's Off
inline constexpr const char *kLimNoteAbove = K2_P("limit active: ");              // then e.g. "9V3A" / "27W"
inline constexpr std::string_view kLimNoteFixed = K2_PSV("fixed 5-20V offers");
inline constexpr std::string_view kLimNotePps = K2_PSV("PPS voltage range and A");
inline constexpr std::string_view kLimNoteAvs = K2_PSV("SPR AVS 15/20V and A");
inline constexpr std::string_view kLimNoteApply = K2_PSV("arms selected PD limit");
inline constexpr std::string_view kLimNoteNoOffer = K2_PSV("not offered by charger");
inline constexpr std::string_view kLimNoteMode = K2_PSV("include in forged offers");
inline constexpr std::string_view kLimNoteStepV = K2_PSV("pick max voltage");
inline constexpr std::string_view kLimNoteStepA = K2_PSV("pick max current");
inline constexpr const char *kLimNoteAmpsAt = "at ", *kLimNoteAnyV = K2_P("any voltage");
inline constexpr std::string_view kLimNotes[] = {kLimNoteOff, kLimNoteNone, kLimNoteFixed, kLimNotePps, kLimNoteAvs, kLimNoteApply,
                                                 kLimNoteNoOffer, kLimNoteMode, kLimNoteStepV, kLimNoteStepA};
static_assert(all_fit_p(kLimNotes, kLabelAdv, kit::kNoteMaxPx), "a PD limit note is wider than its line");
static_assert(fits_px_p(kLabelAdv, kLimNoteAbove, kit::kNoteMaxPx - 5 * static_cast<int>(kLabelAdv)), "the PD limit note plus label is over its line");
inline constexpr std::string_view kLimToastNoOffers = K2_PSV("No charger offers yet");
inline constexpr std::string_view kLimToastNoLimit = K2_PSV("Set a limit first");
inline constexpr std::string_view kLimToastKeepOne = K2_PSV("Keep one PD mode On");
inline constexpr std::string_view kLimToastBadLimit = K2_PSV("Limit out of range");
inline constexpr std::string_view kLimToastBusy = K2_PSV("Port in use: stop it first");
inline constexpr std::string_view kLimToastRate = K2_PSV("Limit off: too many retries");
inline constexpr std::string_view kLimToastChargerResets = K2_PSV("Limit off: charger resets");
inline constexpr std::string_view kLimToastSinkResets = K2_PSV("Limit off: sink resets");
inline constexpr std::string_view kLimToastSinkIgnores = K2_PSV("Limit off: sink ignores it");
inline constexpr std::string_view kLimToastEpr = K2_PSV("Limit off: EPR contract");
// A live contract is reset before a new limit (or Off): the same two-press window as Reset & re-read on LINK.
inline constexpr std::string_view kLimToastConfirm = "OK again: resets link";
inline constexpr std::string_view kLimToastResetting = K2_PSV("Resetting link...");
inline constexpr std::string_view kLimToastRescan = K2_PSV("Rescanning...");
inline constexpr std::string_view kLimToastNoReset = K2_PSV("No reset: replug to apply");
inline constexpr std::string_view kLimToastSelfPowered = K2_PSV("Power K2 via USB port");
inline constexpr std::string_view kLimToastRetry = K2_PSV("Busy, retry");
inline constexpr std::string_view kLimToastNoLink = K2_PSV("No active link");
inline constexpr std::string_view kLimToastNoFit = K2_PSV("Fixed Off: not on charger");   // a fixed object before the APDOs
inline constexpr std::string_view kLimToastChargerRefuses = K2_PSV("Limit off: charger refuses");   // it will not take a re-offered Request
inline constexpr std::string_view kLimRowReset = K2_PSV("Reset link");                       // the picker's row when no offers are known
inline constexpr std::string_view kLimNoteReset = K2_PSV("reads the charger's offers");
inline constexpr std::string_view kLimToasts[] = {kLimToastNoOffers, kLimToastNoLimit, kLimToastKeepOne, kLimToastBadLimit, kLimToastBusy, kLimToastRate,
                                                  kLimToastChargerResets, kLimToastSinkResets, kLimToastSinkIgnores, kLimToastEpr,
                                                  kLimToastConfirm, kLimToastResetting, kLimToastRescan, kLimToastNoReset,
                                                  kLimToastSelfPowered, kLimToastRetry, kLimToastNoLink, kLimToastNoFit,
                                                  kLimToastChargerRefuses};
static_assert(fits_px_p(kLabelAdv, kLimNoteReset, kit::kNoteMaxPx), "the reset row note is wider than its line");
static_assert(all_fit_p(kLimToasts, kLabelAdv, kToastTextPx), "a PD limit result is wider than the toast");

}  // namespace k2::text::tools
