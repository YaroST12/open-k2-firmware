// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Shared checks for the USB descriptor tests (tests/test_usb_desc_*.cpp). Each of
// those includes src/drivers/usb_descriptors.cpp, sets k2::usbcfg::current to its mode, then calls check_common().
//
// The golden bytes were captured from the descriptors of commit 4b9ee31 (before the MSC interface
// existed) by compiling src/drivers/usb_descriptors.cpp on the host. The HID side of the device is what
// k2_remote, the flasher, WebHID and the hands-free DFU rely on, so it must never change.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#include "test_util.h"

namespace golden {

inline constexpr std::array<std::uint8_t, 18> device = {
    0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x16, 0x07, 0x60, 0x50, 0x00, 0x02, 0x01, 0x02, 0x03, 0x01};

// Configuration of the HID-only build: 9-byte header + HID interface (9) + HID class (9) + 2 endpoints (7 + 7).
inline constexpr std::array<std::uint8_t, 41> config = {
    0x09, 0x02, 0x29, 0x00, 0x01, 0x01, 0x00, 0xA0, 0x32,                                            // configuration
    0x09, 0x04, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,                                            // interface 0, HID
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x1B, 0x00,                                            // HID, report len 27
    0x07, 0x05, 0x01, 0x03, 0x40, 0x00, 0x01,                                                        // EP1 OUT interrupt 64 B 1 ms
    0x07, 0x05, 0x81, 0x03, 0x40, 0x00, 0x01};                                                       // EP1 IN interrupt 64 B 1 ms

inline constexpr std::array<std::uint8_t, 27> report = {
    0x05, 0x01, 0x09, 0x00, 0xA1, 0x01, 0x15, 0x00, 0x25, 0xFF, 0x75, 0x08, 0x95, 0x40,
    0x19, 0x01, 0x29, 0x08, 0x81, 0x06, 0x19, 0x01, 0x29, 0x08, 0x91, 0x02, 0xC0};

// String descriptors 0..3 as UTF-16 words including the header word.
inline constexpr std::array<std::uint16_t, 2> str0 = {0x0304, 0x0409};
inline constexpr std::array<std::uint16_t, 6> str1 = {0x030C, 'W', 'I', 'T', 'R', 'N'};
inline constexpr std::array<std::uint16_t, 15> str2 = {0x031E, 'O', 'p', 'e', 'n', '-', 'K', '2', ' ', 'L', 'o', 'g', 'g', 'e', 'r'};
inline constexpr std::array<std::uint16_t, 5> str3 = {0x030A, '0', '0', '0', '1'};

// The MSC interface the HID+Drive mode appends after the HID interface: interface 1, class 08
// (mass storage), subclass 06 (SCSI transparent), protocol 50 (bulk-only), EP2 OUT / EP2 IN bulk 64 B.
inline constexpr std::array<std::uint8_t, 23> msc = {
    0x09, 0x04, 0x01, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00};

}  // namespace golden

template <std::size_t N>
static bool same(const std::uint8_t* p, const std::array<std::uint8_t, N>& g) {
    return std::memcmp(p, g.data(), N) == 0;
}
template <std::size_t N>
static bool same_str(const std::uint16_t* p, const std::array<std::uint16_t, N>& g) {
    return p && std::memcmp(p, g.data(), N * 2) == 0;
}

// Everything except the configuration descriptor is identical in every mode but the net one.
static void check_common() {
    CHECK_EQ(same(tud_descriptor_device_cb(), golden::device), 1);
    CHECK_EQ(same(tud_hid_descriptor_report_cb(0), golden::report), 1);
    CHECK_EQ(sizeof(desc_hid_report), golden::report.size());
    CHECK_EQ(same_str(tud_descriptor_string_cb(0, 0), golden::str0), 1);
    CHECK_EQ(same_str(tud_descriptor_string_cb(1, 0x409), golden::str1), 1);
    CHECK_EQ(same_str(tud_descriptor_string_cb(2, 0x409), golden::str2), 1);
    CHECK_EQ(same_str(tud_descriptor_string_cb(3, 0x409), golden::str3), 1);
    CHECK_EQ(tud_descriptor_string_cb(4, 0x409) == nullptr, 1);        // no network name outside the net mode
    CHECK_EQ(tud_descriptor_string_cb(5, 0x409) == nullptr, 1);        // no MAC string either
}
