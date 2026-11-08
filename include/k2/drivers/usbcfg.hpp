// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The remembered USB link setting. USB is off by default: the device does not enumerate at all
// until the user enables the HID logger/remote and/or mass-storage drive in Settings > USB Link.
// The choice lives in the file /usb.cfg of the littlefs volume (k2/core/fs.hpp), as an 8-byte record:
// "K2US", flags (bit0 hid, bit1 drive, bit2 net), ~flags, 2 zero bytes. Missing or damaged = off. The
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

inline constexpr std::uint32_t kLegacyAddr = 0x080E6000;   // pre-littlefs raw page, read once for the import
inline constexpr const char* kPath = "/usb.cfg";
inline constexpr std::array<std::uint8_t, 4> kMagic{'K', '2', 'U', 'S'};

struct Setting {
    bool hid = false;     // the HID logger + remote channel (interface 0)
    bool drive = false;   // the FAT16 mass-storage drive
    bool net = false;     // CDC-NCM network link + web dashboard (implies hid, excludes drive)
    bool shell = false;   // CDC-ACM debug shell; derived, never stored: on with HID unless Net (see with_shell)
};
constexpr bool any(Setting s) { return s.hid || s.drive || s.net; }

// Valid modes: off, HID, Drive, HID+Drive, HID+Net. Net implies HID; Net with Drive does not fit
// the 512 B packet memory (k2/drivers/usb_pma.hpp) and collapses to off. Without the network function (K2_NET=0) a
// request for it is dropped, and what else the record says stays: HID+Net runs as HID.
constexpr Setting normalize(Setting s, bool net_built = kNetBuilt) {
    if (!net_built) s.net = false;
    if (s.net && s.drive) return {};
    if (s.net) s.hid = true;
    return s;
}

// The debug shell (k2/core/shell.hpp) is a CDC-ACM function added whenever HID is on and Net is not
// (Net has its own web remote). It is not a user setting and not stored: init() derives it.
constexpr bool shell_wanted(Setting s) { return s.hid && !s.net; }
constexpr Setting with_shell(Setting s) { s.shell = shell_wanted(s); return s; }

// The rows of the USB link page. toggle() flips one and keeps the result a valid mode:
// Net turns HID on and the Drive off, Drive on turns Net off, HID off turns Net off.
enum class Row : std::uint8_t { Hid, Drive, Net };

// Without the network function the Net row does not exist: the page shows rows(false) = 2 and the toggle is a no-op.
constexpr int rows(bool net_built = kNetBuilt) { return net_built ? 3 : 2; }

constexpr Setting toggle(Setting s, Row r, bool net_built = kNetBuilt) {
    switch (r) {
        case Row::Hid:   s.hid = !s.hid; if (!s.hid) s.net = false; break;
        case Row::Drive: s.drive = !s.drive; if (s.drive) s.net = false; break;
        case Row::Net:
            if (!net_built) break;
            s.net = !s.net;
            if (s.net) { s.hid = true; s.drive = false; }
            break;
    }
    return s;
}

// What this boot runs with. The firmware sets it from flash before usb::init(); the host tests of
// the descriptors leave the default (both on) so they keep comparing the full configuration.
inline Setting current{true, true};

constexpr std::array<std::uint8_t, 8> encode(Setting s) {
    const std::uint8_t f = static_cast<std::uint8_t>((s.hid ? 1 : 0) | (s.drive ? 2 : 0) | (s.net ? 4 : 0));
    return {kMagic[0], kMagic[1], kMagic[2], kMagic[3], f, static_cast<std::uint8_t>(~f), 0, 0};
}

constexpr bool valid_record(const std::array<std::uint8_t, 8>& r) {
    for (std::size_t i = 0; i < kMagic.size(); i++) if (r[i] != kMagic[i]) return false;
    return static_cast<std::uint8_t>(r[4] ^ r[5]) == 0xFF && !(r[4] & ~7u);
}

constexpr Setting decode(const std::array<std::uint8_t, 8>& r, bool net_built = kNetBuilt) {
    if (!valid_record(r)) return {};
    return normalize({(r[4] & 1) != 0, (r[4] & 2) != 0, (r[4] & 4) != 0}, net_built);
}

// A valid record whose net flag is set: usbcfg::init() logs it when this build has no network function.
constexpr bool requests_net(const std::array<std::uint8_t, 8>& r) { return valid_record(r) && (r[4] & 4) != 0; }

constexpr const char* describe(Setting s) {
    return s.net ? "HID+Net"
                 : s.hid ? (s.drive ? "HID+Drive" : "HID") : (s.drive ? "Drive" : "off");
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
