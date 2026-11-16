// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger-test sweep row's error hero (k2/ui/sweep_hero.hpp): the signed error of the live reading against the setpoint,
// as three characters of the 48 px font plus a sign, its unit, and its grade (green up to 100 mV, amber up to 300, red above:
// the sweep chip's thresholds, ctsweep::kErrOkMv / kErrCautionMv).
#include "test_util.h"
#include "k2/ui/sweep_hero.hpp"
#include "k2/chargertest/result_layout.hpp"
#include <string>

using namespace k2;
using sweep_hero::Grade;

static std::string digits(const sweep_hero::Err& e) { return std::string(e.digits, e.len); }

static void test_zero_has_no_sign_and_is_green() {
    const auto e = sweep_hero::format(0);
    CHECK_STR(digits(e).c_str(), "0");
    CHECK_EQ(int(e.sign), 0);
    CHECK_EQ(e.volts, false);
    CHECK_EQ(int(e.grade), int(Grade::Ok));
}

static void test_signed_millivolts() {
    auto e = sweep_hero::format(50);
    CHECK_STR(digits(e).c_str(), "50"); CHECK_EQ(e.sign, '+'); CHECK_EQ(e.volts, false);
    e = sweep_hero::format(-7);
    CHECK_STR(digits(e).c_str(), "7"); CHECK_EQ(e.sign, '-');
    e = sweep_hero::format(-999);
    CHECK_STR(digits(e).c_str(), "999"); CHECK_EQ(e.sign, '-'); CHECK_EQ(e.volts, false);
    e = sweep_hero::format(999);
    CHECK_STR(digits(e).c_str(), "999"); CHECK_EQ(e.sign, '+');
}

static void test_grade_thresholds_match_the_sweep_chip() {
    // green up to and including 100 mV (the chip's kSweepPassMv), amber up to and including 300, red above; the sign never matters
    static_assert(ctsweep::kErrOkMv == result_layout::kSweepPassMv, "the hero and the chip share the pass window");
    for (const int s : {1, -1}) {
        CHECK_EQ(int(sweep_hero::format(s * 100).grade), int(Grade::Ok));
        CHECK_EQ(int(sweep_hero::format(s * 101).grade), int(Grade::Caution));
        CHECK_EQ(int(sweep_hero::format(s * 300).grade), int(Grade::Caution));
        CHECK_EQ(int(sweep_hero::format(s * 301).grade), int(Grade::Error));
    }
}

static void test_a_volt_or_more_switches_to_volts_in_three_characters() {
    auto e = sweep_hero::format(1000);
    CHECK_STR(digits(e).c_str(), "1.0"); CHECK_EQ(e.sign, '+'); CHECK_EQ(e.volts, true); CHECK_EQ(int(e.grade), int(Grade::Error));
    e = sweep_hero::format(-1234);
    CHECK_STR(digits(e).c_str(), "1.2"); CHECK_EQ(e.sign, '-'); CHECK_EQ(e.volts, true);   // tenths of a volt, floored
    e = sweep_hero::format(9999);
    CHECK_STR(digits(e).c_str(), "9.9"); CHECK_EQ(e.volts, true);
    e = sweep_hero::format(-10000);
    CHECK_STR(digits(e).c_str(), "10"); CHECK_EQ(e.volts, true);   // whole volts from ten
    e = sweep_hero::format(16500);
    CHECK_STR(digits(e).c_str(), "16"); CHECK_EQ(e.sign, '+');
    e = sweep_hero::format(-100000);   // far beyond any reading: still three characters at most
    CHECK_EQ(e.len <= 3, true);
}

static void test_every_error_fits_three_characters() {
    for (int v = -40000; v <= 40000; v += 7) CHECK_EQ(sweep_hero::format(v).len >= 1 && sweep_hero::format(v).len <= 3, true);
}

static void test_the_sign_column_sits_against_the_first_digit() {
    // the sign cell is 12 px wide and ends where the first digit's 24 px cell starts: 3 digits -> the extra column, 1 digit -> 2 cells in
    const int right = 90, adv = 24, sw_ = 12;
    CHECK_EQ(sweep_hero::sign_x(right, adv, sw_, 3), right - 3 * adv - sw_);
    CHECK_EQ(sweep_hero::sign_x(right, adv, sw_, 2), right - 2 * adv - sw_);
    CHECK_EQ(sweep_hero::sign_x(right, adv, sw_, 1), right - 1 * adv - sw_);
}

int main() {
    test_zero_has_no_sign_and_is_green();
    test_signed_millivolts();
    test_grade_thresholds_match_the_sweep_chip();
    test_a_volt_or_more_switches_to_volts_in_three_characters();
    test_every_error_fits_three_characters();
    test_the_sign_column_sits_against_the_first_digit();
    return test_report("test_sweep_hero");
}
