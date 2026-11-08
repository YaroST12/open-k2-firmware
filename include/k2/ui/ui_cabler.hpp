// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Cable R page v2, pure logic: what the two step rows, the result block and the footer say for the
// passive two-point meter and the Kelvin 8-wire mode. The page only paints a CrView.
#pragma once
#include <cstdint>
#include <optional>
#include "k2/pd/cable_r.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2 {

enum class CrTone : uint8_t { Ok, Caution, Dim, Error };

struct CrCard {
    const char *label = "";      // "IDLE", "LOAD", "LIVE", "SENSE"
    StrBuf<16> value;            // at most 14 characters (16 px font next to the step tile)
    CrTone tone = CrTone::Dim;
};

struct CrView {
    bool meter_missing = false;  // no power monitor: only the error card and footer are shown
    CrCard step[2];
    bool has_result = false;
    StrBuf<8> number;            // "110", "1.25", "12.3"
    const char *unit = "";       // "m\xCE\xA9" or "\xCE\xA9"
    cable_r::Grade grade = cable_r::Grade::Waiting;
    const char *grade_pill = ""; // at most 5 characters
    StrBuf<28> detail;           // "0.33V drop, 1.0W lost"
    StrBuf<24> foot;
    CrTone foot_tone = CrTone::Dim;
};

// Splits fmt_mohm into the number and the unit ("110", "m\xCE\xA9").
void cr_split_mohm(uint32_t mohm, StrBuf<8> &number, const char *&unit);

// Short grade names for the pill: "EXCEL", "GOOD", "FAIR", "POOR", "" (waiting).
const char *cr_grade_pill(cable_r::Grade g);

CrView cr_view(bool online, int32_t vbus_mv, int32_t ibus_ma, cable_r::Mode mode,
               const cable_r::Meter &meter, const std::optional<cable_r::Kelvin> &kelvin);

}  // namespace k2
