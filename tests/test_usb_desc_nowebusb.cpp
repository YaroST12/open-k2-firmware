// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// K2_WEBUSB=0 (the default firmware build; this test is compiled with -DK2_WEBUSB=0): the descriptors carry no vendor
// function and no BOS descriptor, whatever the setting says. A setting that still has `webusb` set (the stored request is
// normalized away by usbcfg::init(), but this proves the descriptors do not depend on it) gets the HID or Drive bytes of the
// golden configuration, bcdUSB 2.00, and the BOS request stalls.
#include "../src/drivers/usb_descriptors.cpp"
#include "usb_desc_check.hpp"

static_assert(!k2::usbcfg::kWebusbBuilt, "this test is the K2_WEBUSB=0 build");
static_assert(CFG_TUD_VENDOR == 0, "the vendor class is not compiled in");

int main() {
    k2::usbcfg::current = {true, false, false, false, true};   // HID + a raw WebUSB request that did not go through normalize()
    CHECK_EQ(same(tud_descriptor_device_cb(), golden::device), 1);
    CHECK_EQ(same(tud_descriptor_configuration_cb(0), golden::config), 1);
    check_common();

    k2::usbcfg::current = {false, false, false, false, true};   // WebUSB alone: the HID/Drive modes cannot be picked either, but nothing vendor appears
    const std::uint8_t* c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[4] <= 2, 1);

    k2::usbcfg::current = {true, true, false, false, true};
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 41 + 23);
    CHECK_EQ(c[4], 2);

    k2::usbcfg::current = {true, true};
    return test_report("test_usb_desc_nowebusb");
}
