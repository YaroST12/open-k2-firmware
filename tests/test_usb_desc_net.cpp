// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HID+Net: the HID interface stays byte identical (interface 0), the CDC-NCM function follows
// (its own IAD, control interface 1, data interface 2) and the device descriptor gains the IAD
// class triple. Strings 4 (network name) and 5 (MAC) exist only in this mode.
#include "../src/drivers/usb_descriptors.cpp"
#include "usb_desc_check.hpp"

int main() {
    k2::usbcfg::current = {true, false, true};
    // device: golden bytes except class / subclass / protocol = EF/02/01
    const std::uint8_t* d = tud_descriptor_device_cb();
    auto g = golden::device;
    g[4] = 0xEF; g[5] = 0x02; g[6] = 0x01;
    CHECK_EQ(same(d, g), 1);
    CHECK_EQ(d[17], 1);                                                  // one configuration

    const std::uint8_t* c = tud_descriptor_configuration_cb(0);
    const std::size_t total = c[2] | (c[3] << 8);
    CHECK_EQ(total, golden::config.size() + TUD_CDC_NCM_DESC_LEN);
    CHECK_EQ(c[4], 3);                                                   // bNumInterfaces
    CHECK_EQ(c[5], 1);                                                   // bConfigurationValue
    CHECK_EQ(std::memcmp(c + 9, golden::config.data() + 9, golden::config.size() - 9), 0);   // HID bytes
    const std::uint8_t* n = c + golden::config.size();
    CHECK_EQ(n[1], 0x0B);                                                // IAD first
    CHECK_EQ(n[2], 1); CHECK_EQ(n[3], 2);                                // interfaces 1..2
    CHECK_EQ(n[4], 0x02); CHECK_EQ(n[5], 0x0D);                          // CDC / NCM subclass
    CHECK_EQ(n[8 + 2], 1);                                               // control interface 1

    // endpoints of the NCM function, found by walking the descriptor
    int notif = 0, out = 0, in = 0;
    for (std::size_t i = 0; i < total; i += c[i]) {
        if (c[i + 1] != 5) continue;
        const std::uint8_t a = c[i + 2];
        const unsigned size = c[i + 4] | (c[i + 5] << 8);
        if (a == 0x82 && c[i + 3] == 3 && size == 16) notif++;
        if (a == 0x03 && c[i + 3] == 2 && size == 64) out++;
        if (a == 0x83 && c[i + 3] == 2 && size == 64) in++;
    }
    CHECK_EQ(notif, 1); CHECK_EQ(out, 1); CHECK_EQ(in, 1);

    CHECK_EQ(same(tud_hid_descriptor_report_cb(0), golden::report), 1);
    const std::uint16_t* s4 = tud_descriptor_string_cb(4, 0x409);
    CHECK_EQ(s4 != nullptr && (s4[0] & 0xFF) == 2 * 15 + 2 && s4[1] == 'O', 1);
    const std::uint16_t* s5 = tud_descriptor_string_cb(5, 0x409);
    CHECK_EQ(s5 != nullptr && (s5[0] & 0xFF) == 2 * 12 + 2, 1);
    const char* mac = "024B32000000";
    bool ok = s5 != nullptr;
    for (int i = 0; ok && i < 12; i++) ok = s5[1 + i] == mac[i];
    CHECK_EQ(ok, 1);

    k2::usbcfg::current = {true, true};
    return test_report("test_usb_desc_net");
}
