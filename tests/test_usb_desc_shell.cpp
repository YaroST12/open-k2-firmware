// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HID+Shell and HID+Drive+Shell: the debug shell is a CDC-ACM function appended after the HID (and
// MSC) interfaces. The HID bytes stay identical, the device descriptor gets the IAD class triple
// (the CDC function is an interface association), and the shell's endpoints (notification EP4 IN,
// data EP5 OUT/IN) collide with nothing. Modes without the shell are untouched (the other
// descriptor tests keep comparing them against the golden bytes).
#include "../src/drivers/usb_descriptors.cpp"
#include "usb_desc_check.hpp"

namespace {

// Counts descriptors of one type; endpoints are collected into `eps`.
struct Walk {
    int interfaces = 0, iads = 0;
    std::array<std::uint8_t, 16> eps{};
    int n_eps = 0;
};

Walk walk(const std::uint8_t* c) {
    Walk w;
    const unsigned total = c[2] | (c[3] << 8);
    for (unsigned i = 0; i < total; i += c[i]) {
        if (c[i + 1] == TUSB_DESC_INTERFACE) w.interfaces++;
        if (c[i + 1] == TUSB_DESC_INTERFACE_ASSOCIATION) w.iads++;
        if (c[i + 1] == TUSB_DESC_ENDPOINT) w.eps[w.n_eps++] = c[i + 2];
    }
    return w;
}

bool unique_eps(const Walk& w) {
    for (int i = 0; i < w.n_eps; i++)
        for (int j = i + 1; j < w.n_eps; j++)
            if (w.eps[i] == w.eps[j]) return false;
    return true;
}

bool has_ep(const Walk& w, std::uint8_t ep) {
    for (int i = 0; i < w.n_eps; i++) if (w.eps[i] == ep) return true;
    return false;
}

}  // namespace

int main() {
    // HID + Shell: interfaces HID 0, CDC control 1, CDC data 2
    k2::usbcfg::current = {true, false, false, true};
    const std::uint8_t* c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 41 + TUD_CDC_DESC_LEN);
    CHECK_EQ(c[4], 3);                                               // bNumInterfaces
    CHECK_EQ(std::memcmp(c + 9, golden::config.data() + 9, golden::config.size() - 9), 0);   // HID bytes
    CHECK_EQ(c[41], 8);                                              // IAD follows the HID interface
    CHECK_EQ(c[41 + 1], TUSB_DESC_INTERFACE_ASSOCIATION);
    CHECK_EQ(c[41 + 2], 1);                                          // first interface of the function
    CHECK_EQ(c[41 + 4], TUSB_CLASS_CDC);
    {
        const Walk w = walk(c);
        CHECK_EQ(w.interfaces, 3);
        CHECK_EQ(w.iads, 1);
        CHECK_EQ(unique_eps(w), 1);
        CHECK_EQ(has_ep(w, 0x84) && has_ep(w, 0x05) && has_ep(w, 0x85), 1);
        CHECK_EQ(has_ep(w, 0x01) && has_ep(w, 0x81), 1);
    }
    const std::uint8_t* d = tud_descriptor_device_cb();
    CHECK_EQ(d[4], TUSB_CLASS_MISC);                                 // IAD class triple EF/02/01
    CHECK_EQ(d[5], MISC_SUBCLASS_COMMON);
    CHECK_EQ(d[6], MISC_PROTOCOL_IAD);
    CHECK_EQ(same(tud_hid_descriptor_report_cb(0), golden::report), 1);

    // HID + Drive + Shell: HID 0, MSC 1, CDC control 2, CDC data 3
    k2::usbcfg::current = {true, true, false, true};
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 41 + 23 + TUD_CDC_DESC_LEN);
    CHECK_EQ(c[4], 4);
    CHECK_EQ(std::memcmp(c + 9, golden::config.data() + 9, golden::config.size() - 9), 0);
    CHECK_EQ(std::memcmp(c + 41, golden::msc.data(), golden::msc.size()), 0);   // MSC bytes unchanged
    CHECK_EQ(c[41 + 23 + 1], TUSB_DESC_INTERFACE_ASSOCIATION);
    CHECK_EQ(c[41 + 23 + 2], 2);
    {
        const Walk w = walk(c);
        CHECK_EQ(w.interfaces, 4);
        CHECK_EQ(unique_eps(w), 1);
        CHECK_EQ(has_ep(w, 0x82) && has_ep(w, 0x02), 1);             // MSC endpoints stay on EP2
        CHECK_EQ(has_ep(w, 0x84) && has_ep(w, 0x05) && has_ep(w, 0x85), 1);
    }

    // The shell never joins the network mode or a drive-only link, whatever the flag says.
    k2::usbcfg::current = {true, false, true, true};
    CHECK_EQ(tud_descriptor_configuration_cb(0) == desc_config_net, 1);   // HID + NCM, no CDC-ACM
    k2::usbcfg::current = {false, true, false, true};
    CHECK_EQ(walk(tud_descriptor_configuration_cb(0)).interfaces, 1);   // drive only

    k2::usbcfg::current = {true, true};
    return test_report("test_usb_desc_shell");
}
