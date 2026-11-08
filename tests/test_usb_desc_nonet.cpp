// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// K2_NET=0 (the default firmware build; this test is compiled with -DK2_NET=0): the descriptors carry no
// CDC-NCM function and no network strings, whatever the setting says. A setting that still has `net` set (the
// stored request is normalized away by usbcfg::init(), but this proves the descriptors do not depend on it)
// gets the HID-only bytes of the golden configuration, class 0 in the device descriptor, and strings 4 and 5
// stall like any unknown string.
#include "../src/drivers/usb_descriptors.cpp"
#include "usb_desc_check.hpp"

static_assert(!k2::usbcfg::kNetBuilt, "this test is the K2_NET=0 build");
static_assert(CFG_TUD_NCM == 0, "the NCM class is not compiled in");

int main() {
    k2::usbcfg::current = {true, false, true};   // a raw net request that did not go through normalize()
    const std::uint8_t* d = tud_descriptor_device_cb();
    CHECK_EQ(same(d, golden::device), 1);        // no IAD class triple: class 0
    const std::uint8_t* c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(same(c, golden::config), 1);        // HID only, nothing after it
    CHECK_EQ(c[4], 1);                           // one interface
    CHECK_EQ(tud_descriptor_string_cb(4, 0x409) == nullptr, 1);
    CHECK_EQ(tud_descriptor_string_cb(5, 0x409) == nullptr, 1);
    CHECK_EQ(same(tud_hid_descriptor_report_cb(0), golden::report), 1);

    k2::usbcfg::current = {true, true};          // HID + Drive is unchanged
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 41 + 23);
    CHECK_EQ(c[4], 2);

    k2::usbcfg::current = {true, false, false, true};   // HID + Shell is unchanged
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[4], 3);

    k2::usbcfg::current = {true, true};
    return test_report("test_usb_desc_nonet");
}
