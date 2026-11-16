// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger-test Running view of a deep-sweep row: the signed ERROR (reading - setpoint) is the hero. Pure, host-tested
// (tests/test_sweep_hero.cpp); the painting is ChargerTestPage::paint_sweep_hero.
//
//   Text:  at most three characters of the 48 px font, which has no '+': the sign is drawn separately in the 24 px font,
//          always immediately left of the first digit (sign_x). A zero is "0" with no sign.
//   Unit:  whole mV up to 999; from a volt on tenths of a volt ("1.2", floored), from ten volts whole volts ("12"). The
//          switch keeps the text three characters wide and is always red anyway (it is above kErrCautionMv).
//   Grade: green up to kErrOkMv, amber up to kErrCautionMv, red above (the sweep chip's window, ctsweep::kErrOkMv).
#pragma once
#include <cstdint>
#include "k2/chargertest/sweep.hpp"

namespace k2::sweep_hero {

enum class Grade : uint8_t { Ok, Caution, Error };

constexpr Grade grade(unsigned abs_mv) {
    return abs_mv <= ctsweep::kErrOkMv ? Grade::Ok : abs_mv <= ctsweep::kErrCautionMv ? Grade::Caution : Grade::Error;
}

struct Err {
    char sign = 0;          // '+', '-', or 0 (no sign: the error is zero)
    char digits[4] = {};    // NUL padded
    uint8_t len = 0;        // 1..3
    bool volts = false;     // the unit is V, not mV
    Grade grade = Grade::Ok;
};

constexpr Err format(int32_t err_mv) {
    Err e;
    const uint32_t a = err_mv < 0 ? 0u - uint32_t(err_mv) : uint32_t(err_mv);
    e.sign = err_mv == 0 ? char(0) : err_mv < 0 ? '-' : '+';
    e.grade = grade(a > 0xffffu ? 0xffffu : a);
    if (a < 1000u) {
        e.len = a >= 100u ? 3 : a >= 10u ? 2 : 1;
        uint32_t v = a;
        for (int i = e.len - 1; i >= 0; --i) { e.digits[i] = char('0' + v % 10); v /= 10; }
    } else if (a < 10000u) {
        e.volts = true; e.len = 3;
        e.digits[0] = char('0' + a / 1000u); e.digits[1] = '.'; e.digits[2] = char('0' + a / 100u % 10);
    } else {
        e.volts = true; e.len = 2;
        const uint32_t v = a / 1000u > 99u ? 99u : a / 1000u;
        e.digits[0] = char('0' + v / 10u); e.digits[1] = char('0' + v % 10u);
    }
    return e;
}

// Left edge of the sign cell: it ends `gap` before the first digit's cell starts (the digits are right-aligned in a three-cell box that
// ends at `digits_right`), so the sign hugs the number whatever its length. n = the digit count (1..3). The gap absorbs the landscape
// mapping: the digit Field's origin and the sign cell's origin are mapped separately (x * 240 / 224), and the cell lands up to two columns
// right of where the portrait arithmetic puts it.
constexpr int sign_x(int digits_right, int digit_adv, int sign_adv, int n, int gap = 0) { return digits_right - n * digit_adv - sign_adv - gap; }

}  // namespace k2::sweep_hero
