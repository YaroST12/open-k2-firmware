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
inline constexpr const char *kOffersNone = K2_P("No offers read"),   *kOffersNoneL = K2_P("Tests: Light, PD");
inline constexpr const char *kReadEmarker = K2_P("Read e-marker");
inline constexpr const char *kCableNone = K2_P("Not read"),          *kCableNoneL = K2_P("Actions: Read e-marker");
// Source tab
inline constexpr const char *kLineInUse = K2_P("LINE IN USE"),       *kLineInUseL = K2_P("VBUS live, no source PD");
inline constexpr const char *kCrossed = K2_P("CC CROSSED"),          *kCrossedL = K2_P("Flip one of the two cables");
inline constexpr const char *kFlip = K2_P("FLIP A CABLE?"),          *kFlipL = K2_P("Charger waits, no answer");
inline constexpr const char *kAsleep = K2_P("SOURCE ASLEEP"),        *kAsleepL = K2_P("Refresh or a test wakes it");
inline constexpr const char *kWaiting = K2_P("CHARGER WAITING"),     *kWaitingL = kOffersNoneL;
inline constexpr const char *kDualRole = K2_P("DUAL-ROLE PORT"),     *kDualRoleL = K2_P("Toggles Rp: a phone alone?");
inline constexpr const char *kNoCharger = K2_P("NO CHARGER"),        *kNoChargerL = K2_P("Use either port");
// Sink tab
inline constexpr const char *kDevInUse = K2_P("DEVICE IN USE"),      *kDevInUseL = K2_P("Press OK to interrogate");
inline constexpr const char *kDualRoleSinkL = K2_P("Ask to identify device");
inline constexpr const char *kNoDevice = K2_P("NO DEVICE");
inline constexpr const char *kNoDevDirectL = K2_P("Plug into the other port"), *kNoDevViaL = K2_P("Charger only in this setup"),
                            *kNoDevL = K2_P("Plug into either port");
// Cable tab
inline constexpr const char *kCableNotRead = K2_P("CABLE NOT READ");
inline constexpr const char *kCableInUseL = K2_P("A device may be in use"), *kCableViaL = K2_P("Unplug the charger to read");
inline constexpr const char *kChipSilent = K2_P("CHIP, NO ANSWER"),  *kChipSilentL = K2_P("Try with charger attached");
inline constexpr const char *kNoChip = K2_P("NO CHIP FOUND"),        *kNoChipL = K2_P("Plain cable, no e-marker");
inline constexpr const char *kNoCable = K2_P("NO CABLE"),            *kNoCableL = K2_P("Charger plugged in direct");
inline constexpr const char *kNoCableRead = K2_P("NO CABLE READ"),   *kNoCableReadL = kCableNoneL;

inline constexpr const char *kBandWords[] = {kOffersNone, kCableNone, kLineInUse, kCrossed, kFlip, kAsleep, kWaiting, kDualRole, kNoCharger, kDevInUse,
                                             kNoDevice, kCableNotRead, kChipSilent, kNoChip, kNoCable, kNoCableRead};
static_assert(all_fit_p(kBandWords, 12, kBandWordPx), "a Who's attached band word is wider than the band");
inline constexpr const char *kLines[] = {kOffersNoneL, kCableNoneL, kLineInUseL, kCrossedL, kFlipL, kAsleepL, kWaitingL, kDualRoleL, kNoChargerL,
                                         kDevInUseL, kDualRoleSinkL, kNoDevDirectL, kNoDevViaL, kNoDevL, kCableInUseL, kCableViaL,
                                         kChipSilentL, kNoChipL, kNoCableL, kNoCableReadL};
static_assert(all_fit_p(kLines, 8, kLinePx), "a Who's attached line is wider than 26 cells");

// ---- no PD this attach: the band, one line, then the measured rows ---------------------------------------------------
inline constexpr const char *kNoPd = K2_P("NO PD");
inline constexpr const char *kNoPdNeverL = K2_P("Source never offered PD");      // Source tab, and a sink whose source never offered
inline constexpr const char *kNoPdUnansweredL = K2_P("PD offers went unanswered");  // Sink tab: the device took power, never talked
inline constexpr const char *kNoPdLines[] = {kNoPdNeverL, kNoPdUnansweredL};
static_assert(all_fit_p(kNoPdLines, 8, kLinePx), "a no-PD line is wider than 26 cells");

// ---- footers (at most 27 characters, facts, no key names; the Source hint names an action, not a key) -----------------------------------------------------------
inline constexpr const char *kSourceUnread = K2_P("Not asked: Reset & re-read");   // the attach's auto read did not run (a tool, inline or passive link)
inline constexpr const char *kCableUnread = K2_P("Cable not fully read yet");
inline constexpr const char *kSinkUnread = K2_P("Sink caps not read yet");
inline constexpr const char *kArmedBar = K2_P("Armed, replug to apply");
inline constexpr const char *kHoldActions = K2_P("Hold Back for Actions");
inline constexpr const char *kHoldInfo = K2_P("Hold Back for Info");
inline constexpr const char *kReadingCharger = K2_P("Reading charger ");   // + "n/m" while a survey runs (the survey only asks the charger)
inline constexpr const char *kFooters[] = {kSourceUnread, kCableUnread, kSinkUnread, kArmedBar, kHoldActions, kHoldInfo};
static_assert(all_fit_p(kFooters, 8, 27 * 8), "a Who's attached footer is over 27 characters");
static_assert(strpack::cells_packed(kReadingCharger) + 5 <= 27, "\"Reading charger nn/nn\" fits the status line");
inline constexpr const char *kWakingCharger = K2_P("Waking charger");   // the Charger overview woke a sleeping source by itself (toast, 24 cells)
static_assert(strpack::cells_packed(kWakingCharger) <= 24, "the wake toast fits the op buffer");

// ---- 3-slice Overview & Actions copy (width-checked against RowList / 208 px slice canvas) --------------------------
inline constexpr int kActionLabelMaxPx = 128;   // 16 cells of S16 beside a right-side tail/pill
inline constexpr int kActionNoteMaxPx = 174;    // kit::kNoteMaxPx (29 cells of font 12)
inline constexpr int kSliceLineMaxPx = 192;     // 32 cells of font 12 (x8..200 on 208 px canvas)

inline constexpr const char *kActResetLink = K2_P("Reset & re-read");
inline constexpr const char *kActSpecOffers = K2_P("Spec & offers");
inline constexpr const char *kActSinkDetails = K2_P("Sink details");
inline constexpr const char *kActionLabels[] = {kActResetLink, kActSpecOffers, kActSinkDetails, kReadEmarker};
static_assert(all_fit_p(kActionLabels, 8, kActionLabelMaxPx), "an Actions row label is wider than the row box");

inline constexpr std::string_view kNoteResetInline = K2_PSV("Force fresh PD negotiation");
inline constexpr std::string_view kNoteResetStand = K2_PSV("Wake / rescan source caps");
inline constexpr std::string_view kNoteResetArmed = K2_PSV("Hard Reset drops VBUS ~0.8s");
inline constexpr std::string_view kNoteSpecSource = K2_PSV("Open full offers & details");
inline constexpr std::string_view kNoteSpecSink = K2_PSV("Open full sink details");
inline constexpr std::string_view kActionNotes[] = {kNoteResetInline, kNoteResetStand, kNoteResetArmed,
                                                    kNoteSpecSource, kNoteSpecSink};
static_assert(all_fit_p(kActionNotes, 6, kActionNoteMaxPx), "an Actions row note is wider than kit::kNoteMaxPx");

inline constexpr const char *kSliceTagCharger = K2_P("CHARGER");
inline constexpr const char *kSliceTagCable = K2_P("CABLE");
inline constexpr const char *kSliceTagSink = "SINK";
inline constexpr const char *kSliceBottleneck = K2_P("Bottleneck: caps link");
inline constexpr const char *kSliceReadyL1 = "Sensed on port \xC2\xB7 safe to probe";
inline constexpr const char *kSliceNoPdL1 = "VBUS live \xC2\xB7 no PD seen";
inline constexpr const char *kSliceNoVbusL1 = "No VBUS \xC2\xB7 ready for chip scan";
inline constexpr const char *kSliceLines[] = {kSliceReadyL1, kSliceNoPdL1, kSliceNoVbusL1};
static_assert(fits_px_p(6, kSliceBottleneck, kSliceLineMaxPx) && all_fit(kSliceLines, 6, kSliceLineMaxPx),
              "an Overview slice line is wider than 32 cells");

}  // namespace k2::text::conn
