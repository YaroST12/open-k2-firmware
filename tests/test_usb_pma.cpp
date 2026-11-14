// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The USB packet memory (512 B) must hold BTABLE + EP0 + every endpoint buffer of every link
// mode. The sum is recomputed here from the configuration descriptor the firmware really serves
// (endpoint sizes, aligned the way TinyUSB's fsdev driver allocates them) and compared with the
// budget in k2/drivers/usb_pma.hpp, so a descriptor change cannot silently outgrow the memory.
#include "../src/drivers/usb_descriptors.cpp"
#include "test_util.h"

namespace {

unsigned pma_used_by(const std::uint8_t* c) {
    const unsigned total = c[2] | (c[3] << 8);
    unsigned used = 64 /* BTABLE */ + 2 * CFG_TUD_ENDPOINT0_SIZE;
    std::array<bool, 256> seen{};   // an endpoint address is allocated once, whatever the alt settings
    for (unsigned i = 0; i < total; i += c[i]) {
        if (c[i + 1] != TUSB_DESC_ENDPOINT || seen[c[i + 2]]) continue;
        seen[c[i + 2]] = true;
        const unsigned size = c[i + 4] | (c[i + 5] << 8);
        used += size > 62 ? (size + 31) / 32 * 32 : (size + 1) / 2 * 2;
    }
    return used;
}

void check_mode(k2::usbcfg::Setting s, unsigned expect, const char*) {
    k2::usbcfg::current = s;
    const unsigned used = pma_used_by(tud_descriptor_configuration_cb(0));
    CHECK_EQ(used, expect);
    CHECK_EQ(used <= 512, 1);
    CHECK_EQ(used, k2::usbpma::budget(s));
}

}  // namespace

int main() {
    check_mode({true, false}, 320, "HID");
    check_mode({false, true}, 320, "Drive");
    check_mode({true, true}, 448, "HID+Drive");
    check_mode({true, false, true}, 464, "HID+Net");
    // The debug shell (CDC-ACM: 8 B notification + 2 x 16 B bulk = 40 B) rides on HID and HID+Drive.
    check_mode({true, false, false, true}, 360, "HID+Shell");
    check_mode({true, true, false, true}, 488, "HID+Drive+Shell");
    // WebUSB replaces HID as the remote channel: one vendor interface, bulk IN + OUT of 64 B (2 x 64 = 128 B).
    check_mode({false, false, false, false, true}, 320, "WebUSB");
    check_mode({false, false, false, true, true}, 360, "WebUSB+Shell");
    check_mode({false, true, false, false, true}, 448, "WebUSB+Drive");
    check_mode({false, true, false, true, true}, 488, "WebUSB+Drive+Shell");
    check_mode({true, false, false, true, true}, 488, "HID+WebUSB+Shell");
    // HID + WebUSB + Drive would need 576 B: normalize() drops HID, so it never reaches a descriptor.
    CHECK_EQ(k2::usbpma::budget({true, true, false, false, true}) > 512, 1);
    CHECK_EQ(k2::usbcfg::normalize({true, true, false, false, true}).hid, 0);
    // Net with Drive is not a mode: normalize() collapses it, so it never reaches a descriptor.
    CHECK_EQ(k2::usbcfg::any(k2::usbcfg::normalize({true, true, true})), 0);
    k2::usbcfg::current = {true, true};
    return test_report("test_usb_pma");
}
