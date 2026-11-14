// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The remembered USB link setting. USB is off by default: the device does not enumerate at all
// until the user enables the HID logger/remote and/or mass-storage drive in Settings > USB Link.
// The choice lives in the file /usb.cfg of the littlefs volume (k2/core/fs.hpp), as an 8-byte record:
// "K2US", flags (bit0 hid, bit1 drive, bit2 net, bit3 webusb), ~flags, 2 zero bytes. Missing or damaged = off. The
// record was a raw flash page (0x080E6000) before littlefs; init() imports it once. A change takes effect at the
// next boot, because the configuration descriptor is built once before the host sees the device.
// DFU entry (MINUS held at plug-in) is the bootloader's and is never affected.
#pragma once
#include <array>
#include <cstdint>

#include "k2/core/build_config.h"
#include "k2/core/fs.hpp"

namespace k2::usbcfg {

// Is the network function part of this build (K2_NET, k2/core/build_config.h)? The functions below take it as a
// defaulted argument rather than testing the macro in their bodies, so one host build covers both cases.
inline constexpr bool kNetBuilt = K2_NET != 0;
// Is the WebUSB transport part of this build (K2_WEBUSB)? Same argument convention as net_built.
inline constexpr bool kWebusbBuilt = K2_WEBUSB != 0;

inline constexpr std::uint32_t kLegacyAddr = 0x080E6000;   // pre-littlefs raw page, read once for the import
inline constexpr const char* kPath = "/usb.cfg";
inline constexpr std::array<std::uint8_t, 4> kMagic{'K', '2', 'U', 'S'};

struct Setting {
    bool hid = false;     // the HID logger + remote channel (interface 0)
    bool drive = false;   // the FAT16 mass-storage drive
    bool net = false;     // CDC-NCM network link + web dashboard (implies hid, excludes drive)
    bool shell = false;   // CDC-ACM debug shell; derived, never stored: on with HID or WebUSB unless Net (see with_shell)
    bool webusb = false;  // the WebUSB vendor interface carrying the remote protocol (replaces HID; K2_WEBUSB builds only)
};
constexpr bool any(Setting s) { return s.hid || s.drive || s.net || s.webusb; }

// Valid modes: off, HID, Drive, HID+Drive, HID+Net, WebUSB, WebUSB+Drive, HID+WebUSB. Net implies HID; Net with Drive does
// not fit the 512 B packet memory (k2/drivers/usb_pma.hpp) and collapses to off. HID+WebUSB+Drive does not fit either:
// WebUSB is the primary channel, so HID drops out. WebUSB replaces the Net web path, so WebUSB+Net runs as WebUSB. Without
// the network function (K2_NET=0) or the WebUSB transport (K2_WEBUSB=0) a request for it is dropped, and what else the
// record says stays: HID+Net runs as HID.
constexpr Setting normalize(Setting s, bool net_built = kNetBuilt, bool webusb_built = kWebusbBuilt) {
    if (!net_built) s.net = false;
    if (!webusb_built) s.webusb = false;
    if (s.webusb) s.net = false;
    if (s.net && s.drive) return {};
    if (s.net) s.hid = true;
    if (s.webusb && s.hid && s.drive) s.hid = false;
    return s;
}

// The debug shell (k2/core/shell.hpp) is a CDC-ACM function added whenever HID or WebUSB is on and Net is not
// (Net has its own web remote). It is not a user setting and not stored: init() derives it.
constexpr bool shell_wanted(Setting s) { return (s.hid || s.webusb) && !s.net; }
constexpr Setting with_shell(Setting s) { s.shell = shell_wanted(s); return s; }

// The rows of the USB link page. toggle() flips one and keeps the result a valid mode:
// Net turns HID on and the Drive and WebUSB off, Drive on turns Net off (and HID off when WebUSB is on: all three do not
// fit), HID off turns Net off, WebUSB on turns Net off (and HID off when the Drive is on), HID on beside WebUSB and
// the Drive turns WebUSB off (the fallback was asked for last).
enum class Row : std::uint8_t { Hid, Drive, Net, Webusb };

// Without the network function the Net row does not exist, without the WebUSB transport the WebUSB row does not: the page
// shows rows(false, false) = 2 and those toggles are no-ops. The page lists HID, Drive, [Net], [WebUSB] in that order.
constexpr int rows(bool net_built = kNetBuilt, bool webusb_built = kWebusbBuilt) {
    return 2 + (net_built ? 1 : 0) + (webusb_built ? 1 : 0);
}
constexpr Row row_at(int i, bool net_built = kNetBuilt, bool webusb_built = kWebusbBuilt) {
    if (i == 0) return Row::Hid;
    if (i == 1) return Row::Drive;
    if (i == 2 && net_built) return Row::Net;
    (void)webusb_built;
    return Row::Webusb;
}

constexpr Setting toggle(Setting s, Row r, bool net_built = kNetBuilt, bool webusb_built = kWebusbBuilt) {
    switch (r) {
        case Row::Hid:
            s.hid = !s.hid;
            if (!s.hid) s.net = false;
            else if (s.webusb && s.drive) s.webusb = false;
            break;
        case Row::Drive:
            s.drive = !s.drive;
            if (s.drive) { s.net = false; if (s.webusb) s.hid = false; }
            break;
        case Row::Net:
            if (!net_built) break;
            s.net = !s.net;
            if (s.net) { s.hid = true; s.drive = false; s.webusb = false; }
            break;
        case Row::Webusb:
            if (!webusb_built) break;
            s.webusb = !s.webusb;
            if (s.webusb) { s.net = false; if (s.drive) s.hid = false; }
            break;
    }
    return s;
}

// What this boot runs with. The firmware sets it from flash before usb::init(); the host tests of
// the descriptors leave the default (both on) so they keep comparing the full configuration.
inline Setting current{true, true};

constexpr std::array<std::uint8_t, 8> encode(Setting s) {
    const std::uint8_t f = static_cast<std::uint8_t>((s.hid ? 1 : 0) | (s.drive ? 2 : 0) | (s.net ? 4 : 0) | (s.webusb ? 8 : 0));
    return {kMagic[0], kMagic[1], kMagic[2], kMagic[3], f, static_cast<std::uint8_t>(~f), 0, 0};
}

constexpr bool valid_record(const std::array<std::uint8_t, 8>& r) {
    for (std::size_t i = 0; i < kMagic.size(); i++) if (r[i] != kMagic[i]) return false;
    return static_cast<std::uint8_t>(r[4] ^ r[5]) == 0xFF && !(r[4] & ~15u);
}

constexpr Setting decode(const std::array<std::uint8_t, 8>& r, bool net_built = kNetBuilt, bool webusb_built = kWebusbBuilt) {
    if (!valid_record(r)) return {};
    return normalize({(r[4] & 1) != 0, (r[4] & 2) != 0, (r[4] & 4) != 0, false, (r[4] & 8) != 0}, net_built, webusb_built);
}

// A valid record whose net / webusb flag is set: usbcfg::init() logs it when this build lacks that function.
constexpr bool requests_net(const std::array<std::uint8_t, 8>& r) { return valid_record(r) && (r[4] & 4) != 0; }
constexpr bool requests_webusb(const std::array<std::uint8_t, 8>& r) { return valid_record(r) && (r[4] & 8) != 0; }

constexpr const char* describe(Setting s) {
    if (s.net) return "HID+Net";
    if (s.webusb) return s.hid ? "HID+WebUSB" : s.drive ? "WebUSB+Drive" : "WebUSB";
    return s.hid ? (s.drive ? "HID+Drive" : "HID") : (s.drive ? "Drive" : "off");
}

inline Setting load(fs::Fs& f) {
    std::array<std::uint8_t, 8> r{};
    std::size_t n = 0;
    if (f.read_file(kPath, r, n) != fs::Err::Ok || n != r.size()) return {};
    return decode(r);
}

inline bool save(fs::Fs& f, Setting s) {
    const auto r = encode(s);
    return f.write_file(kPath, r) == fs::Err::Ok;
}

// Firmware side (src/drivers/usbcfg.cpp, not linked into the host tests).
void init();                    // load the stored setting into `current`; call before usb::init()
Setting stored();               // what flash holds now (the setting for the next boot)
bool set(Setting s);            // write the next-boot setting; true when flash took it

}  // namespace k2::usbcfg
