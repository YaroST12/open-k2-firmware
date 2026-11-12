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
// Source tab
inline constexpr const char *kLineInUse = "LINE IN USE",       *kLineInUseL = "VBUS live, no source PD";
inline constexpr const char *kCrossed = "CC CROSSED",          *kCrossedL = "Flip one of the two cables";
inline constexpr const char *kFlip = "FLIP A CABLE?",          *kFlipL = "Charger waits, no answer";
inline constexpr const char *kAsleep = "SOURCE ASLEEP",        *kAsleepL = "Refresh or a test wakes it";
inline constexpr const char *kWaiting = "CHARGER WAITING",     *kWaitingL = "Start a charger test";
inline constexpr const char *kDualRole = "DUAL-ROLE PORT",     *kDualRoleL = "Toggles Rp: a phone alone?";
inline constexpr const char *kNoCharger = "NO CHARGER",        *kNoChargerL = "Use either port";
// Sink tab
inline constexpr const char *kDevInUse = "DEVICE IN USE",      *kDevInUseL = "Line behind K2: not asked";
inline constexpr const char *kNoDevice = "NO DEVICE";
inline constexpr const char *kNoDevDirectL = "Plug into the other port", *kNoDevViaL = "Charger only in this setup",
                            *kNoDevL = "Plug into either port";
// Cable tab
inline constexpr const char *kCableNotRead = "CABLE NOT READ";
inline constexpr const char *kCableInUseL = "A device may be in use", *kCableViaL = "Unplug the charger to read";
inline constexpr const char *kChipSilent = "CHIP, NO ANSWER",  *kChipSilentL = "Try with charger attached";
inline constexpr const char *kNoChip = "NO CHIP FOUND",        *kNoChipL = "Plain cable, no e-marker";
inline constexpr const char *kNoCable = "NO CABLE",            *kNoCableL = "Charger plugged in direct";
inline constexpr const char *kNoCableRead = "NO CABLE READ",   *kNoCableReadL = "Plug a cable into a port";

inline constexpr const char *kBandWords[] = {kLineInUse, kCrossed, kFlip, kAsleep, kWaiting, kDualRole, kNoCharger, kDevInUse,
                                             kNoDevice, kCableNotRead, kChipSilent, kNoChip, kNoCable, kNoCableRead};
static_assert(all_fit(kBandWords, 12, kBandWordPx), "a Who's attached band word is wider than the band");
inline constexpr const char *kLines[] = {kLineInUseL, kCrossedL, kFlipL, kAsleepL, kWaitingL, kDualRoleL, kNoChargerL,
                                         kDevInUseL, kNoDevDirectL, kNoDevViaL, kNoDevL, kCableInUseL, kCableViaL,
                                         kChipSilentL, kNoChipL, kNoCableL, kNoCableReadL};
static_assert(all_fit(kLines, 8, kLinePx), "a Who's attached line is wider than 26 cells");

// ---- no PD this attach: the band, one line, then the measured rows ---------------------------------------------------
inline constexpr const char *kNoPd = "NO PD";
inline constexpr const char *kNoPdNeverL = "Source never offered PD";      // Source tab, and a sink whose source never offered
inline constexpr const char *kNoPdUnansweredL = "PD offers went unanswered";  // Sink tab: the device took power, never talked
inline constexpr const char *kNoPdLines[] = {kNoPdNeverL, kNoPdUnansweredL};
static_assert(all_fit(kNoPdLines, 8, kLinePx), "a no-PD line is wider than 26 cells");

// ---- the single tools (K2_WHOS_ATTACHED=0): one tab each, its title in the TopBar (font 16 from x4, before the readout)
inline constexpr const char *kToolTitles[] = {"Charger ID", "Cable read", "Device ID"};   // by conn::Tab: Source, Cable, Sink
static_assert(all_fit(kToolTitles, 8, 96), "a single tool's title is wider than the TopBar's title box");

// ---- footers (at most 27 characters, facts, no key names) -----------------------------------------------------------
inline constexpr const char *kSourceUnread = "Source caps not read yet";   // a field of the open tab was never asked
inline constexpr const char *kCableUnread = "Cable not fully read yet";
inline constexpr const char *kSinkUnread = "Sink caps not read yet";
inline constexpr const char *kArmedBar = "Armed, replug to apply";
inline constexpr const char *kFooters[] = {kSourceUnread, kCableUnread, kSinkUnread, kArmedBar};
static_assert(all_fit(kFooters, 8, 27 * 8), "a Who's attached footer is over 27 characters");

}  // namespace k2::text::conn
