// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home / status-bar readout formatting: 3 total digits, never mA / mW, and the combined
// "data / power" negotiated row.
#include "test_util.h"
#include "k2/ui/readout_fmt.hpp"
#include <string>

using namespace k2::readout;

static std::string f3(long long milli, char unit) {
    char b[16];
    fmt3(b, milli, unit);
    return b;
}

static void test_three_digits() {
    CHECK_STR(f3(15000, 'V').c_str(), "15.0V");
    CHECK_STR(f3(5080, 'V').c_str(), "5.08V");
    CHECK_STR(f3(2040, 'A').c_str(), "2.04A");
    CHECK_STR(f3(120, 'A').c_str(), "0.12A");
    CHECK_STR(f3(30600, 'W').c_str(), "30.6W");
    CHECK_STR(f3(100000, 'W').c_str(), "100W");
    CHECK_STR(f3(620, 'W').c_str(), "0.62W");
    CHECK_STR(f3(0, 'W').c_str(), "0.00W");
    CHECK_STR(f3(20100, 'V').c_str(), "20.1V");
    CHECK_STR(f3(48000, 'V').c_str(), "48.0V");
    CHECK_STR(f3(240000, 'W').c_str(), "240W");
}

static void test_rounding_carries() {
    CHECK_STR(f3(9994, 'V').c_str(), "9.99V");
    CHECK_STR(f3(9995, 'V').c_str(), "10.0V");     // 3 digits again after the carry
    CHECK_STR(f3(99949, 'W').c_str(), "99.9W");
    CHECK_STR(f3(99950, 'W').c_str(), "100W");
    CHECK_STR(f3(99500, 'W').c_str(), "99.5W");
    CHECK_STR(f3(999499, 'W').c_str(), "999W");
}

static void test_magnitude_only() {
    CHECK_STR(f3(-2040, 'A').c_str(), "2.04A");     // direction is never a sign
}

static void test_no_unit_and_truncation() {
    CHECK_STR(f3(15000, 0).c_str(), "15.0");
    char tiny[4];
    CHECK_EQ(fmt3(tiny, 15000, 'V'), 3);
    CHECK_STR(tiny, "15.");                          // truncated, still terminated
}

static void test_formula() {
    char b[24];
    CHECK_EQ(formula(b, 19100, 2040, 38964), 18);   // 17 cells, 18 bytes (the multiply sign is 2)
    CHECK_STR(b, "19.1V\xC3\x97" "2.04A=39.0W");
    formula(b, 5000, 90, 450);
    CHECK_STR(b, "5.00V\xC3\x97" "0.09A=0.45W");
    formula(b, 20000, 5000, 100000);
    CHECK_STR(b, "20.0V\xC3\x97" "5.00A=100W");
}

static void test_rated_watts() {
    char b[8];
    rated_watts(b, 100000);  CHECK_STR(b, "100W");
    rated_watts(b, 45000);   CHECK_STR(b, "45W");
    rated_watts(b, 15000);   CHECK_STR(b, "15W");
    rated_watts(b, 27500);   CHECK_STR(b, "28W");   // nearest watt
}

static void test_negotiated_row() {
    char b[40];
    negotiated_row(b, "USB3 10G +DP Alt", "PD 100W");
    CHECK_STR(b, "USB3 10G +DP Alt / PD 100W");
    negotiated_row(b, "", "QC3 18W");
    CHECK_STR(b, "? / QC3 18W");
    negotiated_row(b, "USB 2.0", "");
    CHECK_STR(b, "USB 2.0 / ?");
    negotiated_row(b, "", "");
    CHECK_STR(b, "? / ?");
    negotiated_row(b, "--", "5V 15W");               // usblink "--" = undetermined
    CHECK_STR(b, "? / 5V 15W");
    negotiated_row(b, "Unkn", "5V 15W");             // Home keeps "?" where Detail and the PD tabs say "Unkn"
    CHECK_STR(b, "? / 5V 15W");
}

static void test_power_half() {
    char b[16];
    // PD contract 20 V x 5 A
    power_half(b, PowerKind::Pd, 20000, 5000, "");
    CHECK_STR(b, "PD 100W");
    // legacy protocol name + measured volts
    power_half(b, PowerKind::Legacy, 9000, 0, "QC3");
    CHECK_STR(b, "QC3 9V");
    // plain 5 V with a Type-C Rp current
    power_half(b, PowerKind::Plain, 5000, 3000, "");
    CHECK_STR(b, "5V 15W");
    power_half(b, PowerKind::Plain, 5000, 0, "");
    CHECK_STR(b, "5V");
    power_half(b, PowerKind::None, 0, 0, "");
    CHECK_STR(b, "");
}

int main() {
    test_three_digits();
    test_rounding_carries();
    test_magnitude_only();
    test_no_unit_and_truncation();
    test_formula();
    test_rated_watts();
    test_negotiated_row();
    test_power_half();
    return test_report("test_readout_fmt");
}
