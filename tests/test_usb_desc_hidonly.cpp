// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB link = HID only, and Drive only: one interface, the same HID bytes as the HID-only golden
// configuration, so the host sees exactly the M1 logger.
#include "../src/drivers/usb_descriptors.cpp"
#include "usb_desc_check.hpp"

int main() {
    k2::usbcfg::current = {true, false};
    check_common();
    const std::uint8_t* c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(same(c, golden::config), 1);
    k2::usbcfg::current = {false, true};
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 9 + 23);   // drive only: header + MSC interface
    CHECK_EQ(c[4], 1);
    CHECK_EQ(c[9 + 2], 0);
    k2::usbcfg::current = {true, true};
    return test_report("test_usb_desc_hidonly");
}
