// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The WebUSB side of the USB descriptors, as pure constexpr builders so the host tests can check every byte:
//  - the BOS descriptor with two platform capabilities: WebUSB (landing page) and Microsoft OS 2.0 (so Windows binds the
//    vendor interface to the inbox WinUSB driver without an INF);
//  - the landing-page URL descriptor, served on the WebUSB vendor request;
//  - the Microsoft OS 2.0 descriptor set (configuration subset, function subset keyed to the vendor interface number,
//    compatible ID "WINUSB", DeviceInterfaceGUIDs), served on the Microsoft vendor request with wIndex 7;
//  - control_reply(): which of those a vendor control request gets (src/drivers/usb_descriptors.cpp forwards
//    tud_vendor_control_xfer_cb to it).
// Browsers only offer a device to the page when its bcdUSB is at least 2.10 and it carries the WebUSB capability; the
// descriptor file raises bcdUSB in the WebUSB modes. Provenance: inferred from the public WebUSB and OS 2.0 descriptor
// layouts and checked against the host tests (tests/test_usb_webusb.cpp); the first hardware run is the real proof.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace k2::webusb {

// Vendor request codes (bRequest) the BOS capabilities announce; any value works as long as BOS and the handler agree.
inline constexpr std::uint8_t kReqWebusb = 0x01;   // GET_URL (wIndex 2): the landing page
inline constexpr std::uint8_t kReqMsos = 0x02;     // MS OS 2.0 descriptor set (wIndex 7)
inline constexpr std::uint16_t kIndexGetUrl = 2;
inline constexpr std::uint16_t kIndexMsos = 7;
inline constexpr std::uint8_t kLandingIndex = 1;   // iLandingPage: GET_URL's wValue

// The page a browser suggests when the K2 is plugged in (Chrome shows a notification on Android and desktop): the
// dashboard (tools/web/k2.html) on the project's GitHub Pages site, published by `make site` as k2.html at the site root.
// The URL descriptor's scheme byte is 1 (https), so the string carries no scheme.
inline constexpr std::string_view kLandingUrl = "yarost12.github.io/open-k2-firmware/k2.html";

// DeviceInterfaceGUIDs of the vendor function (registry property of the OS 2.0 set); a random (version 4) GUID generated
// once for this project, it only has to be unique to this device class.
inline constexpr std::string_view kInterfaceGuid = "{81bdae64-520b-4114-aba4-de8eedd252bf}";

inline constexpr std::uint8_t kBosHeaderLen = 5;
inline constexpr std::uint8_t kWebusbCapLen = 24;
inline constexpr std::uint8_t kMsosCapLen = 28;
inline constexpr std::uint16_t kBosLen = kBosHeaderLen + kWebusbCapLen + kMsosCapLen;   // 57

inline constexpr std::uint32_t kWindowsVersion = 0x06030000;   // Windows 8.1 and later

inline constexpr std::uint16_t kUrlDescLen = 3 + kLandingUrl.size();

// Microsoft OS 2.0 descriptor set layout: header 10, configuration subset 8, function subset 8, compatible ID 20,
// registry property (4 + 2 + 2 + name + 2 + data).
inline constexpr std::string_view kGuidPropName = "DeviceInterfaceGUIDs";
inline constexpr std::uint16_t kPropNameBytes = (kGuidPropName.size() + 1) * 2;          // UTF-16 with NUL
inline constexpr std::uint16_t kPropDataBytes = (kInterfaceGuid.size() + 1) * 2 + 2;     // REG_MULTI_SZ: one string + list NUL
inline constexpr std::uint16_t kPropLen = 4 + 2 + 2 + kPropNameBytes + 2 + kPropDataBytes;
inline constexpr std::uint16_t kFunctionSubsetLen = 8 + 20 + kPropLen;
inline constexpr std::uint16_t kConfigSubsetLen = 8 + kFunctionSubsetLen;
inline constexpr std::uint16_t kMsosSetLen = 10 + kConfigSubsetLen;

namespace detail {
constexpr void put16(std::uint8_t* p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8); }
constexpr void put32(std::uint8_t* p, std::uint32_t v) { put16(p, static_cast<std::uint16_t>(v)); put16(p + 2, static_cast<std::uint16_t>(v >> 16)); }

// Platform capability UUIDs, already in wire (mixed-endian) order.
inline constexpr std::array<std::uint8_t, 16> kUuidWebusb{0x38, 0xB6, 0x08, 0x34, 0xA9, 0x09, 0xA0, 0x47,
                                                           0x8B, 0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65};
inline constexpr std::array<std::uint8_t, 16> kUuidMsos{0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
                                                         0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F};

constexpr std::size_t put_utf16(std::uint8_t* p, std::string_view s, bool nul) {
    std::size_t n = 0;
    for (char c : s) { p[n++] = static_cast<std::uint8_t>(c); p[n++] = 0; }
    if (nul) { p[n++] = 0; p[n++] = 0; }
    return n;
}
}  // namespace detail

// BOS descriptor: header + WebUSB capability (vendor code kReqWebusb, landing page index) + OS 2.0 capability.
constexpr std::array<std::uint8_t, kBosLen> make_bos() {
    std::array<std::uint8_t, kBosLen> b{};
    std::uint8_t* p = b.data();
    p[0] = kBosHeaderLen; p[1] = 0x0F; detail::put16(p + 2, kBosLen); p[4] = 2;
    p += kBosHeaderLen;
    p[0] = kWebusbCapLen; p[1] = 0x10; p[2] = 0x05; p[3] = 0;
    for (std::size_t i = 0; i < 16; i++) p[4 + i] = detail::kUuidWebusb[i];
    detail::put16(p + 20, 0x0100); p[22] = kReqWebusb; p[23] = kLandingIndex;
    p += kWebusbCapLen;
    p[0] = kMsosCapLen; p[1] = 0x10; p[2] = 0x05; p[3] = 0;
    for (std::size_t i = 0; i < 16; i++) p[4 + i] = detail::kUuidMsos[i];
    detail::put32(p + 20, kWindowsVersion); detail::put16(p + 24, kMsosSetLen); p[26] = kReqMsos; p[27] = 0;
    return b;
}

// URL descriptor: length, type 3, scheme 1 (https), the rest of the URL.
constexpr std::array<std::uint8_t, kUrlDescLen> make_url() {
    std::array<std::uint8_t, kUrlDescLen> u{};
    u[0] = static_cast<std::uint8_t>(kUrlDescLen); u[1] = 3; u[2] = 1;
    for (std::size_t i = 0; i < kLandingUrl.size(); i++) u[3 + i] = static_cast<std::uint8_t>(kLandingUrl[i]);
    return u;
}

// OS 2.0 descriptor set for a vendor function whose first interface is `itf`.
constexpr std::array<std::uint8_t, kMsosSetLen> make_msos(std::uint8_t itf) {
    std::array<std::uint8_t, kMsosSetLen> s{};
    std::uint8_t* p = s.data();
    detail::put16(p, 10); detail::put16(p + 2, 0); detail::put32(p + 4, kWindowsVersion); detail::put16(p + 8, kMsosSetLen);
    p += 10;
    detail::put16(p, 8); detail::put16(p + 2, 1); p[4] = 0; p[5] = 0; detail::put16(p + 6, kConfigSubsetLen);   // configuration subset
    p += 8;
    detail::put16(p, 8); detail::put16(p + 2, 2); p[4] = itf; p[5] = 0; detail::put16(p + 6, kFunctionSubsetLen);   // function subset
    p += 8;
    detail::put16(p, 20); detail::put16(p + 2, 3);                                                                 // compatible ID
    const std::string_view id = "WINUSB";
    for (std::size_t i = 0; i < id.size(); i++) p[4 + i] = static_cast<std::uint8_t>(id[i]);                       // 8 bytes, NUL padded; sub ID zero
    p += 20;
    detail::put16(p, kPropLen); detail::put16(p + 2, 4); detail::put16(p + 4, 7);                                  // registry property, REG_MULTI_SZ
    detail::put16(p + 6, kPropNameBytes);
    std::size_t n = 8;
    n += detail::put_utf16(p + n, kGuidPropName, true);
    detail::put16(p + n, kPropDataBytes); n += 2;
    n += detail::put_utf16(p + n, kInterfaceGuid, true);
    detail::put16(p + n, 0);   // the list terminator
    return s;
}

inline constexpr auto kBos = make_bos();
inline constexpr auto kUrl = make_url();
inline constexpr auto kMsos0 = make_msos(0);   // vendor interface 0: WebUSB alone, or with the Drive
inline constexpr auto kMsos1 = make_msos(1);   // vendor interface 1: after the HID interface

// The vendor function's interface number in a configuration: 0, except after the HID interface (HID + WebUSB, no Drive;
// HID with the Drive never has WebUSB, see usbcfg::normalize). Used by the descriptors and by the OS 2.0 set.
constexpr std::uint8_t vendor_interface(bool hid, bool drive) { return hid && !drive ? 1 : 0; }

struct Reply { const std::uint8_t* data = nullptr; std::uint16_t len = 0; };

// The data stage a vendor control request (device-to-host) gets; data == nullptr means stall.
// `vendor_itf` is the vendor interface number of the active configuration (0 or 1).
constexpr Reply control_reply(std::uint8_t request, std::uint16_t value, std::uint16_t index, std::uint8_t vendor_itf) {
    if (request == kReqWebusb && index == kIndexGetUrl && value == kLandingIndex) return {kUrl.data(), kUrl.size()};
    if (request == kReqMsos && index == kIndexMsos) {
        return vendor_itf == 0 ? Reply{kMsos0.data(), kMsos0.size()} : Reply{kMsos1.data(), kMsos1.size()};
    }
    return {};
}

}  // namespace k2::webusb
