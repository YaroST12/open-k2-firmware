// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB packet memory (PMA) budget per link mode. The AT32F403A USB core has 512 bytes of packet
// memory shared by the BTABLE, EP0 and every endpoint buffer; TinyUSB's fsdev driver allocates
// them at configuration time and fails the endpoint open when they do not fit, which shows up as
// a device that enumerates half-way. These constants are the single source of truth for the
// endpoint sizes in src/drivers/usb_descriptors.cpp, and the static_asserts below fail the build
// (and every host test that includes this header) when a mode outgrows the memory.
#pragma once
#include <cstdint>

#include "k2/drivers/usbcfg.hpp"

namespace k2::usbpma {

inline constexpr unsigned kTotal = 512;
inline constexpr unsigned kBtable = 8 * 8;     // 8 endpoint entries x 8 bytes
inline constexpr unsigned kEp0 = 2 * 64;       // control IN + OUT buffers

inline constexpr std::uint8_t kHidEpSize = 64;     // EP1 IN + OUT interrupt
inline constexpr std::uint8_t kMscEpSize = 64;     // EP2 IN + OUT bulk (drive)
inline constexpr std::uint8_t kVendorEpSize = 64;  // EP3 IN + OUT bulk (WebUSB vendor interface, shares EP3 with the Net data; never both)
inline constexpr std::uint8_t kNetNotifSize = 16;  // NCM speed-change / connect notification (8 + 8)
inline constexpr std::uint8_t kNetEpSize = 64;     // EP3 IN + OUT bulk (NCM data)
inline constexpr std::uint8_t kShellNotifSize = 8;  // CDC-ACM notification EP4 IN (never used, must exist)
inline constexpr std::uint8_t kShellEpSize = 16;    // EP5 IN + OUT bulk (CDC data); 32 would not fit beside HID+Drive

// TinyUSB pma_align_buffer_size(): 2-byte blocks up to 62 bytes, 32-byte blocks above.
constexpr unsigned buffer(unsigned size) { return size > 62 ? (size + 31) / 32 * 32 : (size + 1) / 2 * 2; }

constexpr unsigned kHid = 2 * buffer(kHidEpSize);
constexpr unsigned kMsc = 2 * buffer(kMscEpSize);
constexpr unsigned kVendor = 2 * buffer(kVendorEpSize);
constexpr unsigned kNet = buffer(kNetNotifSize) + 2 * buffer(kNetEpSize);
constexpr unsigned kShell = buffer(kShellNotifSize) + 2 * buffer(kShellEpSize);

constexpr unsigned budget(usbcfg::Setting s) {
    if (!usbcfg::any(s)) return 0;
    return kBtable + kEp0 + (s.hid ? kHid : 0) + (s.drive ? kMsc : 0) + (s.net ? kNet : 0) + (s.webusb ? kVendor : 0) + (s.shell && usbcfg::shell_wanted(s) ? kShell : 0);
}

static_assert(budget({true, false}) == 320, "HID only");
static_assert(budget({true, false}) <= kTotal);
static_assert(budget({false, true}) <= kTotal, "Drive only");
static_assert(budget({true, true}) == 448 && budget({true, true}) <= kTotal, "HID + Drive");
static_assert(budget({true, false, true}) == 464 && budget({true, false, true}) <= kTotal, "HID + Net");
static_assert(budget({true, false, false, true}) == 360, "HID + Shell");
static_assert(budget({true, true, false, true}) == 488 && budget({true, true, false, true}) <= kTotal, "HID + Drive + Shell");
// WebUSB takes HID's place as the remote channel (one 64 B bulk pair, same size as the HID pair), with or without HID.
static_assert(budget({false, false, false, false, true}) == 320, "WebUSB only");
static_assert(budget({false, false, false, true, true}) == 360, "WebUSB + Shell");
static_assert(budget({false, true, false, false, true}) == 448, "WebUSB + Drive");
static_assert(budget({false, true, false, true, true}) == 488 && budget({false, true, false, true, true}) <= kTotal, "WebUSB + Drive + Shell");
static_assert(budget({true, false, false, true, true}) == 488, "HID + WebUSB + Shell (the fallback beside WebUSB)");
static_assert(budget({true, true, false, false, true}) > kTotal, "HID + WebUSB + Drive is rejected by normalize()");
// Drive and Net together would not fit: usbcfg::normalize() never lets that mode through.
static_assert(kBtable + kEp0 + kHid + kMsc + kNet > kTotal, "Net + Drive is rejected by normalize()");

}  // namespace k2::usbpma
