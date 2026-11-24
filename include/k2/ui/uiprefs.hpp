// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remembered user preferences that are not the USB link: "Try QC after PD" of the charger test, the Screenshot
// switch, the display smoothing preset and the screen orientation settings. One 8-byte record in the file
// /prefs.cfg of the littlefs volume: "K2UP", flags (bit0 try QC after PD, bit1 screenshot DISABLED), ~flags,
// smoothing byte, orientation byte.
// Smoothing byte: 0 = the default preset (Medium), else 1 + the smoothing::Preset index (1 Off, 2 Low, 3 Medium,
// 4 High); an unknown value reads as the default. Records written before the preset existed have 0 there, and
// the default preset is written as 0 too, so old and new firmware read each other's file. Bits 4..5 of the same byte
// were a theme (Night, Paper, Classic) for a few builds; the Classic palette is the only one now, so they are ignored
// on read (a stored Night or Paper loads as Classic) and written 0. Bits 6..7: Settings > Test level, the charger test's
// default level (charger_test::Level: 0 Medium, the default, 1 Light, 2 Deep; 3 reads Medium); older firmware reads only
// bits 0..3 (and 4..5) of the byte, so the file reads both ways.
// Optional 9th byte: bits 0..5 Settings > Long Back, the ring position long Back jumps to (ring::Pos, 0 Home = the default);
// bits 6..7 Settings > Sweep depth, (ctsweep::Depth - 1) & 3 (0 = 200 mV, the default; 1 Phone, 2 20 mV, 3 100 mV). Written
// only when either is not 0, so the defaults keep the 8-byte file older firmware wrote; a value past the ring reads Home. Older
// firmware wants exactly 8 bytes: a file with a chosen target or depth is a default file to it.
// Orientation byte (the last byte, written 0 by older firmware and never checked by its decode, so the file reads
// both ways): bit0..1 manual rotation 0..3 (x 90 deg), bit2 manual (0 = automatic, the default), bit3 rotate
// buttons (Settings > Rotate buttons, default off), bit4 wake sleeping port (Settings > Wake sleeping port, default
// off: the idle K2 presents no Rd), bit5..6 the Home hero (Long Back on Home: 0 W, the default; 1 V; 2 A; 3 reads
// as W); bit7 Settings > Huawei/Samsung tests (1 = FCP, SCP and AFC shown in the charger test; stored shown so every older
// file and the default read hidden; older firmware ignores it). The rotation value only counts when bit2 is set. 0 = today's
// defaults: automatic orientation, keys as printed, no idle Rd, the W hero, the three legacy units hidden.
// Missing or damaged = defaults (QC off, screenshot on, smoothing Medium, orientation automatic; bit1 is stored
// inverted so an old record keeps screenshots on). The record was a raw flash page (0x080E6800) before
// littlefs; stored() imports it once.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/core/fs.hpp"
#include "k2/ui/smoothing.hpp"
#include "k2/ui/ui_ring.hpp"

namespace k2::uiprefs {

inline constexpr std::uint32_t kLegacyAddr = 0x080E6800;   // pre-littlefs raw page, read once for the import
inline constexpr const char* kPath = "/prefs.cfg";
inline constexpr std::array<std::uint8_t, 4> kMagic{'K', '2', 'U', 'P'};

inline constexpr std::array<std::uint8_t, 3> kDefaultHomeBoxes{1, 2, 0};   // Pd, Energy, Rotating

struct Prefs {
    bool qc_after_pd = false;   // obsolete: the charger test always runs the legacy units (no option row any more); kept so stored prefs files stay valid
    bool screenshot = true;     // Settings > Screenshot: remote and web screenshots render the page
    smoothing::Preset smoothing = smoothing::kDefault;   // Settings > Smoothing: One Euro preset of the readouts and graphs
    bool rotation_manual = false;      // Settings > Autorotate (or the +/- hold): false = follows gravity, true = fixed at `rotation`
    std::uint8_t rotation = 0;         // manual orientation 0..3 (x 90 deg); only meaningful when rotation_manual
    bool rotate_buttons = false;       // Settings > Rotate buttons: the +/- keys follow the displayed strip
    bool idle_wake = false;            // Settings > Wake sleeping port: an idle K2 holds its Rd on a standby-signature line (off = no Rd at idle)
    std::uint8_t home_hero = 0;        // Home's hero quantity (home::qty_from): 0 W, 1 V, 2 A
    std::uint8_t ct_level = 0;         // Settings > Test level: the charger test's default level (charger_test::Level; 0 Medium)
    std::uint8_t long_back = 0;        // Settings > Long Back: the ring::Pos long Back jumps to (0 Home)
    bool legacy_units = false;         // Settings > Huawei/Samsung tests: FCP, SCP and AFC in the charger test (off = hidden)
    std::uint8_t sweep_depth = 1;      // Settings > Sweep depth: ctsweep::Depth of the deep sweeps (1 = Step200, the default)
    std::array<std::uint8_t, 3> home_boxes = kDefaultHomeBoxes;   // Settings > Home boxes (home::BoxKind per slot)
    bool operator==(const Prefs&) const = default;
};

// This boot's switch, read by the remote, the web server and the capture module without touching
// the file system. main() loads it from stored(); the Settings page updates it with set().
inline bool screenshot_enabled = true;
// Huawei/Samsung units shown in the charger test. Defaults to shown so the host UI tests see today's pages; main() sets it
// from the prefs, where the default is hidden.
inline bool legacy_units_shown = true;

constexpr std::array<std::uint8_t, 8> encode(Prefs p) {
    const std::uint8_t f = static_cast<std::uint8_t>((p.qc_after_pd ? 1 : 0) | (p.screenshot ? 0 : 2));
    const std::uint8_t s = static_cast<std::uint8_t>((p.smoothing == smoothing::kDefault ? 0 : 1 + static_cast<unsigned>(p.smoothing)) |
                                                 ((p.ct_level < 3 ? p.ct_level : 0) << 6));
    const std::uint8_t o = static_cast<std::uint8_t>((p.rotation & 3) | (p.rotation_manual ? 4 : 0) | (p.rotate_buttons ? 8 : 0) | (p.idle_wake ? 16 : 0) |
                                                 ((p.home_hero < 3 ? p.home_hero : 0) << 5) | (p.legacy_units ? 128 : 0));
    return {kMagic[0], kMagic[1], kMagic[2], kMagic[3], f, static_cast<std::uint8_t>(~f), s, o};
}

// The record with optional bytes 8 (long_back + sweep_depth) and 9..10 (home_boxes); record_len says how much is written.
constexpr std::array<std::uint8_t, 11> encode_ext(Prefs p) {
    const auto r = encode(p);
    const std::uint8_t lb = p.long_back < ring::kCount ? p.long_back : std::uint8_t{0};
    const std::uint8_t b0 = p.home_boxes[0] < 10 ? p.home_boxes[0] : kDefaultHomeBoxes[0];
    const std::uint8_t b1 = p.home_boxes[1] < 10 ? p.home_boxes[1] : kDefaultHomeBoxes[1];
    const std::uint8_t b2 = p.home_boxes[2] < 10 ? p.home_boxes[2] : kDefaultHomeBoxes[2];
    return {r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
            static_cast<std::uint8_t>(lb | (((p.sweep_depth - 1u) & 3u) << 6)),
            static_cast<std::uint8_t>(b0 | (b1 << 4)), b2};
}
constexpr std::size_t record_len(Prefs p) {
    if (p.home_boxes != kDefaultHomeBoxes) return 11;
    return encode_ext(p)[8] != 0 ? 9 : 8;
}

// 8 bytes (base record), 9 (with ring target / sweep depth), or 11 (with home boxes); any other length is no record.
constexpr Prefs decode(std::span<const std::uint8_t> r) {
    if (r.size() != 8 && r.size() != 9 && r.size() != 11) return {};
    for (std::size_t i = 0; i < kMagic.size(); i++) if (r[i] != kMagic[i]) return {};
    if (static_cast<std::uint8_t>(r[4] ^ r[5]) != 0xFF || (r[4] & ~3u)) return {};
    const unsigned preset = r[6] & 0x0Fu;   // bits 4..5: the retired theme, ignored
    const smoothing::Preset sm = preset == 0 ? smoothing::kDefault : smoothing::preset_from_index(preset - 1u);
    std::array<std::uint8_t, 3> hb = kDefaultHomeBoxes;
    if (r.size() == 11) {
        const std::uint8_t b0 = r[9] & 0x0Fu, b1 = (r[9] >> 4) & 0x0Fu, b2 = r[10] & 0x0Fu;
        hb = {b0 < 10 ? b0 : kDefaultHomeBoxes[0], b1 < 10 ? b1 : kDefaultHomeBoxes[1], b2 < 10 ? b2 : kDefaultHomeBoxes[2]};
    }
    return {(r[4] & 1) != 0, (r[4] & 2) == 0, sm, (r[7] & 4) != 0, static_cast<std::uint8_t>(r[7] & 3), (r[7] & 8) != 0, (r[7] & 16) != 0,
            static_cast<std::uint8_t>(((r[7] >> 5) & 3) == 3 ? 0 : (r[7] >> 5) & 3),
            static_cast<std::uint8_t>((r[6] >> 6) == 3 ? 0 : r[6] >> 6),
            static_cast<std::uint8_t>(r.size() >= 9 && (r[8] & 0x3Fu) < ring::kCount ? r[8] & 0x3Fu : 0), (r[7] & 128) != 0,
            static_cast<std::uint8_t>(r.size() >= 9 ? ((r[8] >> 6) + 1u) & 3u : 1u), hb};
}

inline Prefs load(fs::Fs& f) {
    std::array<std::uint8_t, 11> r{};
    std::size_t n = 0;
    if (f.read_file(kPath, r, n) != fs::Err::Ok) return {};
    return decode(std::span<const std::uint8_t>(r.data(), n));
}

inline bool save(fs::Fs& f, Prefs p) {
    const auto r = encode_ext(p);
    return f.write_file(kPath, std::span<const std::uint8_t>(r.data(), record_len(p))) == fs::Err::Ok;
}

// Firmware side (src/ui/uiprefs.cpp, stubbed in the host UI tests).
Prefs stored();            // what the volume holds now
bool set(Prefs p);         // write; true when flash took it

}  // namespace k2::uiprefs
