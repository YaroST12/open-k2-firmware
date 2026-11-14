// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The WebUSB link modes: a vendor-class interface (class 0xFF, bulk OUT 0x03 / IN 0x83, 64 B) carries the remote protocol
// instead of the HID interface. The device descriptor claims bcdUSB 2.10 (browsers need it for the BOS request) and the
// BOS descriptor carries the WebUSB and Microsoft OS 2.0 capabilities. The modes that keep HID or the Drive keep their
// bytes (the other descriptor tests compare them against the golden descriptors). Compiled with K2_WEBUSB=1.
#include "../src/drivers/usb_descriptors.cpp"
#include "k2/drivers/usb_webusb.hpp"
#include "usb_desc_check.hpp"

static_assert(k2::usbcfg::kWebusbBuilt, "this test is the K2_WEBUSB=1 build");
static_assert(CFG_TUD_VENDOR == 1, "the vendor class is compiled in");

namespace {

struct Walk {
    int interfaces = 0, iads = 0, vendor_itfs = 0;
    std::uint8_t vendor_itf_no = 0xFF;
    std::array<std::uint8_t, 16> eps{};
    std::array<std::uint16_t, 16> sizes{};
    std::array<std::uint8_t, 16> attrs{};
    int n_eps = 0;
};

Walk walk(const std::uint8_t* c) {
    Walk w;
    const unsigned total = c[2] | (c[3] << 8);
    unsigned off = 0;
    for (unsigned i = 0; i < total; i += c[i]) {
        if (c[i + 1] == TUSB_DESC_INTERFACE) {
            w.interfaces++;
            if (c[i + 5] == TUSB_CLASS_VENDOR_SPECIFIC) { w.vendor_itfs++; w.vendor_itf_no = c[i + 2]; }
        }
        if (c[i + 1] == TUSB_DESC_INTERFACE_ASSOCIATION) w.iads++;
        if (c[i + 1] == TUSB_DESC_ENDPOINT) {
            w.eps[w.n_eps] = c[i + 2]; w.attrs[w.n_eps] = c[i + 3]; w.sizes[w.n_eps] = static_cast<std::uint16_t>(c[i + 4] | (c[i + 5] << 8));
            w.n_eps++;
        }
        off = i + c[i];
    }
    CHECK_EQ(off, total);                    // the descriptors tile the configuration exactly
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

// The device descriptor of a WebUSB mode is the golden one with bcdUSB 2.10 (and the IAD class triple when a CDC function
// is associated).
void check_device(bool iad) {
    const std::uint8_t* d = tud_descriptor_device_cb();
    for (int i = 0; i < 18; i++) {
        if (i == 2 || i == 3) continue;
        if (iad && i >= 4 && i <= 6) continue;
        CHECK_EQ(d[i], golden::device[i]);
    }
    CHECK_EQ(d[2], 0x10); CHECK_EQ(d[3], 0x02);
    if (iad) { CHECK_EQ(d[4], TUSB_CLASS_MISC); CHECK_EQ(d[5], MISC_SUBCLASS_COMMON); CHECK_EQ(d[6], MISC_PROTOCOL_IAD); }
    else { CHECK_EQ(d[4], 0); CHECK_EQ(d[5], 0); CHECK_EQ(d[6], 0); }
    // The strings and the HID report callback are shared by every mode.
    CHECK_EQ(same_str(tud_descriptor_string_cb(1, 0x409), golden::str1), 1);
    CHECK_EQ(same_str(tud_descriptor_string_cb(2, 0x409), golden::str2), 1);
    CHECK_EQ(same_str(tud_descriptor_string_cb(3, 0x409), golden::str3), 1);
    CHECK_EQ(tud_descriptor_string_cb(5, 0x409) == nullptr, 1);
}

void check_bos(bool present) {
    const std::uint8_t* b = tud_descriptor_bos_cb();
    if (!present) { CHECK_EQ(b == nullptr, 1); return; }
    CHECK_EQ(b != nullptr, 1);
    CHECK_EQ(std::memcmp(b, k2::webusb::kBos.data(), k2::webusb::kBos.size()), 0);
}

// The vendor interface bytes: class FF, two bulk endpoints of 64 B, no interface string.
void check_vendor(const std::uint8_t* c, std::uint8_t itf) {
    const unsigned total = c[2] | (c[3] << 8);
    for (unsigned i = 0; i < total; i += c[i]) {
        if (c[i + 1] == TUSB_DESC_INTERFACE && c[i + 2] == itf) {
            CHECK_EQ(c[i + 4], 2);                          // two endpoints
            CHECK_EQ(c[i + 5], TUSB_CLASS_VENDOR_SPECIFIC);
            CHECK_EQ(c[i + 6], 0); CHECK_EQ(c[i + 7], 0); CHECK_EQ(c[i + 8], 0);
            CHECK_EQ(c[i + 9], 7); CHECK_EQ(c[i + 10], TUSB_DESC_ENDPOINT);
            CHECK_EQ(c[i + 11], 0x03); CHECK_EQ(c[i + 12], TUSB_XFER_BULK); CHECK_EQ(c[i + 13], 64); CHECK_EQ(c[i + 14], 0);
            CHECK_EQ(c[i + 16], 7); CHECK_EQ(c[i + 17], TUSB_DESC_ENDPOINT);
            CHECK_EQ(c[i + 18], 0x83); CHECK_EQ(c[i + 19], TUSB_XFER_BULK); CHECK_EQ(c[i + 20], 64); CHECK_EQ(c[i + 21], 0);
            return;
        }
    }
    CHECK_EQ(0, 1);   // interface not found
}

}  // namespace

int main() {
    using k2::usbcfg::Setting;
    // WebUSB only: the vendor function is interface 0, class 0 device, no IAD.
    k2::usbcfg::current = Setting{false, false, false, false, true};
    const std::uint8_t* c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 9 + 23);
    CHECK_EQ(c[4], 1);
    CHECK_EQ(c[5], 1); CHECK_EQ(c[7], 0xA0); CHECK_EQ(c[8], 50);      // bConfigurationValue, attributes and current as every mode
    check_vendor(c, 0);
    {
        const Walk w = walk(c);
        CHECK_EQ(w.interfaces, 1); CHECK_EQ(w.vendor_itfs, 1); CHECK_EQ(w.iads, 0);
        CHECK_EQ(unique_eps(w), 1); CHECK_EQ(w.n_eps, 2);
    }
    check_device(false);
    check_bos(true);
    CHECK_EQ(k2::webusb::vendor_interface(false, false), 0);

    // WebUSB + Shell: vendor 0, CDC control 1 + data 2 (the IAD class triple appears).
    k2::usbcfg::current = Setting{false, false, false, true, true};
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 9 + 23 + TUD_CDC_DESC_LEN);
    CHECK_EQ(c[4], 3);
    check_vendor(c, 0);
    {
        const Walk w = walk(c);
        CHECK_EQ(w.interfaces, 3); CHECK_EQ(w.iads, 1);
        CHECK_EQ(unique_eps(w), 1);
        CHECK_EQ(has_ep(w, 0x84) && has_ep(w, 0x05) && has_ep(w, 0x85), 1);
        CHECK_EQ(has_ep(w, 0x03) && has_ep(w, 0x83), 1);
        CHECK_EQ(has_ep(w, 0x01) || has_ep(w, 0x81), 0);                // no HID
    }
    check_device(true);
    check_bos(true);

    // WebUSB + Drive: vendor 0, MSC 1 (MSC bytes as in every drive mode), no shell without the flag.
    k2::usbcfg::current = Setting{false, true, false, false, true};
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 9 + 23 + 23);
    CHECK_EQ(c[4], 2);
    check_vendor(c, 0);
    CHECK_EQ(std::memcmp(c + 9 + 23, golden::msc.data(), golden::msc.size()), 0);
    {
        const Walk w = walk(c);
        CHECK_EQ(unique_eps(w), 1); CHECK_EQ(w.vendor_itfs, 1);
    }
    check_device(false);

    // WebUSB + Drive + Shell: vendor 0, MSC 1, CDC 2 + 3.
    k2::usbcfg::current = Setting{false, true, false, true, true};
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 9 + 23 + 23 + TUD_CDC_DESC_LEN);
    CHECK_EQ(c[4], 4);
    check_vendor(c, 0);
    CHECK_EQ(std::memcmp(c + 9 + 23, golden::msc.data(), golden::msc.size()), 0);
    CHECK_EQ(c[9 + 23 + 23 + 1], TUSB_DESC_INTERFACE_ASSOCIATION);
    CHECK_EQ(c[9 + 23 + 23 + 2], 2);                                      // the CDC function starts at interface 2
    {
        const Walk w = walk(c);
        CHECK_EQ(w.interfaces, 4); CHECK_EQ(unique_eps(w), 1);
        CHECK_EQ(has_ep(w, 0x02) && has_ep(w, 0x82) && has_ep(w, 0x03) && has_ep(w, 0x83), 1);
    }
    check_device(true);

    // HID + WebUSB: the HID bytes are untouched and stay interface 0; the vendor function is interface 1.
    k2::usbcfg::current = Setting{true, false, false, false, true};
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 41 + 23);
    CHECK_EQ(c[4], 2);
    CHECK_EQ(std::memcmp(c + 9, golden::config.data() + 9, 32), 0);          // the HID interface (9 + 9 + 7 + 7 bytes)
    check_vendor(c, 1);
    CHECK_EQ(k2::webusb::vendor_interface(true, false), 1);
    {
        const Walk w = walk(c);
        CHECK_EQ(w.vendor_itf_no, 1); CHECK_EQ(unique_eps(w), 1);
        CHECK_EQ(has_ep(w, 0x01) && has_ep(w, 0x81), 1);
    }
    check_device(false);
    CHECK_EQ(same(tud_hid_descriptor_report_cb(0), golden::report), 1);

    // HID + WebUSB + Shell: HID 0, vendor 1, CDC 2 + 3.
    k2::usbcfg::current = Setting{true, false, false, true, true};
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 41 + 23 + TUD_CDC_DESC_LEN);
    CHECK_EQ(c[4], 4);
    check_vendor(c, 1);
    CHECK_EQ(c[41 + 23 + 2], 2);
    CHECK_EQ(unique_eps(walk(c)), 1);
    check_device(true);

    // HID + WebUSB + Drive does not fit the packet memory: a raw setting that asks for it gets WebUSB + Drive.
    k2::usbcfg::current = Setting{true, true, false, false, true};
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 9 + 23 + 23);
    CHECK_EQ(k2::webusb::vendor_interface(true, true), 0);

    // WebUSB wins over a stray Net request.
    k2::usbcfg::current = Setting{false, false, true, false, true};
    c = tud_descriptor_configuration_cb(0);
    CHECK_EQ(c[2] | (c[3] << 8), 9 + 23);

    // Modes without WebUSB: bcdUSB stays 2.00 and there is no BOS descriptor (the host never asks for it, and a stall is the
    // right answer if it does).
    k2::usbcfg::current = Setting{true, false};
    CHECK_EQ(same(tud_descriptor_device_cb(), golden::device), 1);
    check_bos(false);
    CHECK_EQ(same(tud_descriptor_configuration_cb(0), golden::config), 1);
    k2::usbcfg::current = Setting{true, true, false, true};
    CHECK_EQ(tud_descriptor_device_cb()[3], 0x02); CHECK_EQ(tud_descriptor_device_cb()[2], 0x00);
    check_bos(false);

    k2::usbcfg::current = {true, true};
    return test_report("test_usb_desc_webusb");
}
