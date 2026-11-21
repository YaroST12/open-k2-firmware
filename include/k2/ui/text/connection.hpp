// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Who's attached copy: every fixed string the page shows, in one place, each table checked against the box it is drawn into.
// Layout and rules: docs/UI_SYSTEM.md 5.8 (verdict band), 6 (copy), 7.13 (decisions). An empty tab is a verdict band (a
// dash and the word in font 24 from x32) over ONE line of font 16 at x6; footers state a fact and never name a key.
#pragma once
#include "k2/ui/trunc.hpp"

namespace k2::text::conn {

// The verdict band's word: font 24 (12 px a cell) from x32 to x220.
inline constexpr int kBandWordPx = 188;
// The one line under the band: font 16 (8 px a cell) from x6 to x218, 26 cells.
inline constexpr int kLinePx = 212;

// ---- empty tabs: the band word, then the one line ------------------------------------------------------------------
// Pages with nothing held yet say which test or action fills them: offers come from a Light or PD test, the cable from the
// Cable Actions row (kReadEmarker is that row's label).
inline constexpr const char *kOffersNone = "No offers read",   *kOffersNoneL = "Tests: Light, PD";
inline constexpr const char *kReadEmarker = "Read e-marker";
inline constexpr const char *kCableNone = "Not read",          *kCableNoneL = "Actions: Read e-marker";
// Source tab
inline constexpr const char *kLineInUse = "LINE IN USE",       *kLineInUseL = "VBUS live, no source PD";
inline constexpr const char *kCrossed = "CC CROSSED",          *kCrossedL = "Flip one of the two cables";
inline constexpr const char *kFlip = "FLIP A CABLE?",          *kFlipL = "Charger waits, no answer";
inline constexpr const char *kAsleep = "SOURCE ASLEEP",        *kAsleepL = "Refresh or a test wakes it";
inline constexpr const char *kWaiting = "CHARGER WAITING",     *kWaitingL = kOffersNoneL;
inline constexpr const char *kDualRole = "DUAL-ROLE PORT",     *kDualRoleL = "Toggles Rp: a phone alone?";
inline constexpr const char *kNoCharger = "NO CHARGER",        *kNoChargerL = "Use either port";
// Sink tab
inline constexpr const char *kDevInUse = "DEVICE IN USE",      *kDevInUseL = "Press OK to interrogate";
inline constexpr const char *kDualRoleSinkL = "Ask to identify device";
inline constexpr const char *kNoDevice = "NO DEVICE";
inline constexpr const char *kNoDevDirectL = "Plug into the other port", *kNoDevViaL = "Charger only in this setup",
                            *kNoDevL = "Plug into either port";
// Cable tab
inline constexpr const char *kCableNotRead = "CABLE NOT READ";
inline constexpr const char *kCableInUseL = "A device may be in use", *kCableViaL = "Unplug the charger to read";
inline constexpr const char *kChipSilent = "CHIP, NO ANSWER",  *kChipSilentL = "Try with charger attached";
inline constexpr const char *kNoChip = "NO CHIP FOUND",        *kNoChipL = "Plain cable, no e-marker";
inline constexpr const char *kNoCable = "NO CABLE",            *kNoCableL = "Charger plugged in direct";
inline constexpr const char *kNoCableRead = "NO CABLE READ",   *kNoCableReadL = kCableNoneL;

inline constexpr const char *kBandWords[] = {kOffersNone, kCableNone, kLineInUse, kCrossed, kFlip, kAsleep, kWaiting, kDualRole, kNoCharger, kDevInUse,
                                             kNoDevice, kCableNotRead, kChipSilent, kNoChip, kNoCable, kNoCableRead};
static_assert(all_fit(kBandWords, 12, kBandWordPx), "a Who's attached band word is wider than the band");
inline constexpr const char *kLines[] = {kOffersNoneL, kCableNoneL, kLineInUseL, kCrossedL, kFlipL, kAsleepL, kWaitingL, kDualRoleL, kNoChargerL,
                                         kDevInUseL, kDualRoleSinkL, kNoDevDirectL, kNoDevViaL, kNoDevL, kCableInUseL, kCableViaL,
                                         kChipSilentL, kNoChipL, kNoCableL, kNoCableReadL};
static_assert(all_fit(kLines, 8, kLinePx), "a Who's attached line is wider than 26 cells");

// ---- no PD this attach: the band, one line, then the measured rows ---------------------------------------------------
inline constexpr const char *kNoPd = "NO PD";
inline constexpr const char *kNoPdNeverL = "Source never offered PD";      // Source tab, and a sink whose source never offered
inline constexpr const char *kNoPdUnansweredL = "PD offers went unanswered";  // Sink tab: the device took power, never talked
inline constexpr const char *kNoPdLines[] = {kNoPdNeverL, kNoPdUnansweredL};
static_assert(all_fit(kNoPdLines, 8, kLinePx), "a no-PD line is wider than 26 cells");

// ---- footers (at most 27 characters, facts, no key names) -----------------------------------------------------------
inline constexpr const char *kSourceUnread = "Source caps not read yet";   // a field of the open tab was never asked
inline constexpr const char *kCableUnread = "Cable not fully read yet";
inline constexpr const char *kSinkUnread = "Sink caps not read yet";
inline constexpr const char *kArmedBar = "Armed, replug to apply";
inline constexpr const char *kHoldActions = "Hold Back for Actions";
inline constexpr const char *kHoldInfo = "Hold Back for Info";
inline constexpr const char *kFooters[] = {kSourceUnread, kCableUnread, kSinkUnread, kArmedBar, kHoldActions, kHoldInfo};
static_assert(all_fit(kFooters, 8, 27 * 8), "a Who's attached footer is over 27 characters");

// ---- 3-slice Overview & Actions copy (width-checked against RowList / 208 px slice canvas) --------------------------
inline constexpr int kActionLabelMaxPx = 128;   // 16 cells of S16 beside a right-side tail/pill
inline constexpr int kActionNoteMaxPx = 174;    // kit::kNoteMaxPx (29 cells of font 12)
inline constexpr int kSliceLineMaxPx = 192;     // 32 cells of font 12 (x8..200 on 208 px canvas)

inline constexpr const char *kActResetLink = "Reset & re-read";
inline constexpr const char *kActSpecOffers = "Spec & offers";
inline constexpr const char *kActChipDetails = "Chip details";
inline constexpr const char *kActSinkDetails = "Sink details";
inline constexpr const char *kActionLabels[] = {kActResetLink, kActSpecOffers, kActChipDetails, kActSinkDetails, kReadEmarker};
static_assert(all_fit(kActionLabels, 8, kActionLabelMaxPx), "an Actions row label is wider than the row box");

inline constexpr const char *kNoteResetInline = "Force fresh PD negotiation";
inline constexpr const char *kNoteResetStand = "Wake / rescan source caps";
inline constexpr const char *kNoteResetArmed = "Hard Reset drops VBUS ~0.8s";
inline constexpr const char *kNoteSpecSource = "Open full offers & details";
inline constexpr const char *kNoteSpecCable = "Open full e-marker details";
inline constexpr const char *kNoteSpecSink = "Open full sink details";
inline constexpr const char *kActionNotes[] = {kNoteResetInline, kNoteResetStand, kNoteResetArmed,
                                               kNoteSpecSource, kNoteSpecCable, kNoteSpecSink};
static_assert(all_fit(kActionNotes, 6, kActionNoteMaxPx), "an Actions row note is wider than kit::kNoteMaxPx");

inline constexpr const char *kSliceTagCharger = "CHARGER";
inline constexpr const char *kSliceTagCable = "CABLE";
inline constexpr const char *kSliceTagSink = "SINK";
inline constexpr const char *kSliceBottleneck = "Bottleneck: caps link";
inline constexpr const char *kSliceReadyL1 = "Sensed on port \xC2\xB7 safe to probe";
inline constexpr const char *kSliceNoPdL1 = "VBUS live \xC2\xB7 no PD seen";
inline constexpr const char *kSliceNoVbusL1 = "No VBUS \xC2\xB7 ready for chip scan";
inline constexpr const char *kSliceLines[] = {kSliceBottleneck, kSliceReadyL1, kSliceNoPdL1, kSliceNoVbusL1};
static_assert(all_fit(kSliceLines, 6, kSliceLineMaxPx), "an Overview slice line is wider than 32 cells");

}  // namespace k2::text::conn
