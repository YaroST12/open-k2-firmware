// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HID+Drive: the HID interface stays byte identical (interface 0, endpoints, report descriptor,
// device descriptor, strings). Only the configuration header changes (interface count, total
// length) and the MSC interface is appended after the HID interface.
#include "../src/drivers/usb_descriptors.cpp"
#include "usb_desc_check.hpp"

int main() {
    k2::usbcfg::current = {true, true};
    check_common();
    const std::uint8_t* c = tud_descriptor_configuration_cb(0);
    const std::size_t total = c[2] | (c[3] << 8);
    CHECK_EQ(total, golden::config.size() + golden::msc.size());     // 41 + 23 = 64
    // configuration header: only bNumInterfaces and wTotalLength differ from the HID-only one
    CHECK_EQ(c[0], golden::config[0]);
    CHECK_EQ(c[1], golden::config[1]);
    CHECK_EQ(c[4], 2);                                               // bNumInterfaces
    CHECK_EQ(c[5], golden::config[5]);                               // bConfigurationValue
    CHECK_EQ(c[6], golden::config[6]);
    CHECK_EQ(c[7], golden::config[7]);                               // bmAttributes
    CHECK_EQ(c[8], golden::config[8]);                               // bMaxPower
    // bytes 9..41 (the whole HID interface incl. endpoints) are identical to the HID-only build
    CHECK_EQ(std::memcmp(c + 9, golden::config.data() + 9, golden::config.size() - 9), 0);
    // the MSC interface follows
    CHECK_EQ(std::memcmp(c + golden::config.size(), golden::msc.data(), golden::msc.size()), 0);
    // HID stays interface 0, MSC is interface 1
    CHECK_EQ(c[9 + 2], 0);
    CHECK_EQ(c[41 + 2], 1);
    // the endpoints of the two interfaces do not collide
    CHECK_EQ(c[41 + 9 + 2] != c[9 + 9 + 9 + 2] && c[41 + 9 + 2] != c[9 + 9 + 9 + 7 + 2], 1);
    return test_report("test_usb_desc_msc1");
}
