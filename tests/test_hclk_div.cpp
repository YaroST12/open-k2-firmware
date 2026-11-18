// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The AHB (HCLK) divider table of the clock driver (k2/drivers/crm.hpp): which dividers exist, their register codes, the
// clocks derived from them. The switch itself runs on hardware.
#include "k2/drivers/crm.hpp"
#include "test_util.h"

using namespace k2::crm;

static void test_valid_dividers() {
    CHECK_EQ(hclk_div_valid(1), true);
    CHECK_EQ(hclk_div_valid(2), true);
    CHECK_EQ(hclk_div_valid(4), true);
    CHECK_EQ(hclk_div_valid(8), true);
    CHECK_EQ(hclk_div_valid(0), false);
    CHECK_EQ(hclk_div_valid(3), false);
    CHECK_EQ(hclk_div_valid(16), false);   // APB1 would fall under the 8 MHz the USB register interface needs
    CHECK_EQ(hclk_div_valid(256), false);
}

static void test_register_codes() {
    CHECK_EQ(ahbdiv_code(1), 0u);
    CHECK_EQ(ahbdiv_code(2), 8u);          // 1000b = /2
    CHECK_EQ(ahbdiv_code(4), 9u);
    CHECK_EQ(ahbdiv_code(8), 10u);
}

static void test_derived_clocks() {
    CHECK_EQ(hclk_hz_for(1), 240'000'000u);
    CHECK_EQ(hclk_hz_for(4), 60'000'000u);
    CHECK_EQ(apb1_hz_for(1), 120'000'000u);   // APB1 and APB2 stay at /2 of HCLK
    CHECK_EQ(apb1_hz_for(4), 30'000'000u);
    CHECK_EQ(apb1_hz_for(8), 15'000'000u);
    CHECK_EQ(timer_hz_for(1), 240'000'000u);  // a timer on a /2 bus runs at 2 x the bus = HCLK
    CHECK_EQ(timer_hz_for(8), 30'000'000u);
}

int main() {
    test_valid_dividers();
    test_register_codes();
    test_derived_clocks();
    return test_report("test_hclk_div");
}
