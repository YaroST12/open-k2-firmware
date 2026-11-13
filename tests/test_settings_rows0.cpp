// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Settings page's row constants in the default firmware build (K2_SCREENSHOT=0: no Screenshot row). The host tests
// build with K2_SCREENSHOT=1, so this one object is built with K2_SCREENSHOT=0 (the Makefile sets HOST_K2_SCREENSHOT=0 for
// it, as test_usb_desc_nonet does for K2_NET). ui_pages.cpp static_asserts the same constants against its SettingRow enum in
// whichever build compiles it; this pins the numbers the K2_SCREENSHOT=0 image really has.
#include "k2/core/build_config.h"
#include "k2/ui/ui_pages.hpp"
#include "test_util.h"

using namespace k2;

#if K2_SCREENSHOT
#error "test_settings_rows0 is the K2_SCREENSHOT=0 object"
#endif

static_assert(SettingsPage::kRows == 7, "USB Link, Autorotate, Rotate buttons, Smoothing, Wake, System state, Boot info");
static_assert(SettingsPage::kSystemRow == 5);
static_assert(SettingsPage::kBootRow == 6);
static_assert(SettingsPage::kBootRow == SettingsPage::kRows - 1);
static_assert(SettingsPage::kSystemRow == SettingsPage::kRows - 2);

int main() {
    // Runtime copies of the compile-time facts, so the run shows what was checked.
    constexpr int rows = SettingsPage::kRows;
    CHECK_EQ(rows, 7);
    return test_report("test_settings_rows0");
}
