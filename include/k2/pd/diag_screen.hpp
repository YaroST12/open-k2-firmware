// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The K2 screen's compact form of a Black Box diagnosis (k2/pd/diag.hpp): the full lines are up to 79 characters, the
// panel's rows are 16 (Home row 1), 23 and 31. Every text here fits its row by construction (no cut, no wrap; the
// host tests feed worst-case numbers): a fixed label per finding, its evidence in a 12 px note, a short verdict.
// Units follow the screen's copy rules (no space: "20V3.25A"). Pure, no I/O.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/diag.hpp"

namespace k2::pd::diag::screen {

inline constexpr std::size_t kLabelChars = 16;     // a RowList label (font 16 from x28, beside a 7-char tail) and Home row 1
inline constexpr std::size_t kTailChars = 7;       // the tail: the finding's time, m:ss or h:mm:ss
inline constexpr std::size_t kNoteChars = 31;      // the focused row's note (font 12 from x28)
inline constexpr std::size_t kVerdictChars = 27;   // a footer or a full-width 16 px line

// One row of the screen list: the findings in rank order, then the healthy lines, then the data lanes.
struct Row {
    Sev sev = Sev::Ok;                              // Ok for a healthy line and the lanes
    bool lanes = false;                             // the data-lane row (no verdict about it: they are not observed)
    std::array<char, 24> label{};                   // <= kLabelChars
    std::array<char, 12> tail{};                    // <= kTailChars, empty for no time
    std::array<char, 40> note{};                    // <= kNoteChars
};
unsigned row_count(const Report& r);                // 0 before any session
bool row(const Report& r, unsigned idx, Row& out);

// "Link OK, power short" (<= kVerdictChars); "No PD session yet" for an empty report.
void verdict(const Report& r, std::span<char> out);
// Home row 1: the top finding's label when it is a Fail or a Warn, else the state ("Healthy", "Contract unseen", ...);
// "No PD session" for an empty report. <= kLabelChars. `sev` gets the colour's severity (Ok = dim, no alarm).
void word(const Report& r, std::span<char> out, Sev& sev);
// The worst severity of the findings (Ok when none).
Sev worst(const Report& r);

}  // namespace k2::pd::diag::screen
