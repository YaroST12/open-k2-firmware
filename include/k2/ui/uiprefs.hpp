// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remembered user preferences that are not the USB link: "Try QC after PD" of the charger test and the
// Screenshot switch and the display smoothing preset. One 8-byte record in the file /prefs.cfg of the littlefs
// volume: "K2UP", flags (bit0 try QC after PD, bit1 screenshot DISABLED), ~flags, smoothing byte, 1 zero byte.
// Smoothing byte: 0 = the default preset (Medium), else 1 + the smoothing::Preset index (1 Off, 2 Low, 3 Medium,
// 4 High); an unknown value reads as the default. Records written before the preset existed have 0 there, and
// the default preset is written as 0 too, so old and new firmware read each other's file. Missing or damaged =
// defaults (QC off, screenshot on, smoothing Medium; bit1 is stored inverted so an old record keeps screenshots
// on). The record was a raw flash page (0x080E6800) before littlefs; stored() imports it once.
#pragma once
#include <array>
#include <cstdint>

#include "k2/core/fs.hpp"
#include "k2/ui/smoothing.hpp"

namespace k2::uiprefs {

inline constexpr std::uint32_t kLegacyAddr = 0x080E6800;   // pre-littlefs raw page, read once for the import
inline constexpr const char* kPath = "/prefs.cfg";
inline constexpr std::array<std::uint8_t, 4> kMagic{'K', '2', 'U', 'P'};

struct Prefs {
    bool qc_after_pd = false;   // obsolete: the charger test always runs the legacy units (no option row any more); kept so stored prefs files stay valid
    bool screenshot = true;     // Settings > Screenshot: remote and web screenshots render the page
    smoothing::Preset smoothing = smoothing::kDefault;   // Settings > Smoothing: One Euro preset of the readouts and graphs
};

// This boot's switch, read by the remote, the web server and the capture module without touching
// the file system. main() loads it from stored(); the Settings page updates it with set().
inline bool screenshot_enabled = true;

constexpr std::array<std::uint8_t, 8> encode(Prefs p) {
    const std::uint8_t f = static_cast<std::uint8_t>((p.qc_after_pd ? 1 : 0) | (p.screenshot ? 0 : 2));
    const std::uint8_t s = p.smoothing == smoothing::kDefault ? 0 : static_cast<std::uint8_t>(1 + static_cast<unsigned>(p.smoothing));
    return {kMagic[0], kMagic[1], kMagic[2], kMagic[3], f, static_cast<std::uint8_t>(~f), s, 0};
}

constexpr Prefs decode(const std::array<std::uint8_t, 8>& r) {
    for (std::size_t i = 0; i < kMagic.size(); i++) if (r[i] != kMagic[i]) return {};
    if (static_cast<std::uint8_t>(r[4] ^ r[5]) != 0xFF || (r[4] & ~3u)) return {};
    const smoothing::Preset sm = r[6] == 0 ? smoothing::kDefault : smoothing::preset_from_index(r[6] - 1u);
    return {(r[4] & 1) != 0, (r[4] & 2) == 0, sm};
}

inline Prefs load(fs::Fs& f) {
    std::array<std::uint8_t, 8> r{};
    std::size_t n = 0;
    if (f.read_file(kPath, r, n) != fs::Err::Ok || n != r.size()) return {};
    return decode(r);
}

inline bool save(fs::Fs& f, Prefs p) {
    const auto r = encode(p);
    return f.write_file(kPath, r) == fs::Err::Ok;
}

// Firmware side (src/ui/uiprefs.cpp, stubbed in the host UI tests).
Prefs stored();            // what the volume holds now
bool set(Prefs p);         // write; true when flash took it

}  // namespace k2::uiprefs
