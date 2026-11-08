// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The remembered cable-identity selection (ctcable::Choice: Off / a built-in chip / a saved clone) of the shared list
// (ui_cable_list.hpp). One 8-byte record in its own file /cable.cfg of the littlefs volume (the 8-byte /prefs.cfg format is
// not touched): "K2CB", kind, seq low, seq high, check = kind ^ seq low ^ seq high ^ 0xA5. A missing, short, long or damaged
// file, and a well-formed record that names an impossible choice, read as Off. Whether a saved clone still exists is the
// list's business (it reads the library); this file only keeps what was chosen.
//
// It is written only when the selection changes (the list compares with what it last read or wrote), so a boot, a START or a
// repeated press costs no flash write. It exists so a self-powered boot can arm the chosen identity before the charger's
// first attach (k2/pd/boot_ra.hpp): the selection used to live in RAM and a fresh boot found none.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/chargertest/ct_cable.hpp"
#include "k2/core/fs.hpp"

namespace k2::cablepref {

inline constexpr const char* kPath = "/cable.cfg";
inline constexpr std::array<std::uint8_t, 4> kMagic{'K', '2', 'C', 'B'};
inline constexpr std::uint8_t kCheckSalt = 0xA5;

constexpr std::array<std::uint8_t, 8> encode(ctcable::Choice c) {
    c = ctcable::canonical(c);
    const std::uint8_t kind = static_cast<std::uint8_t>(c.kind);
    const std::uint8_t lo = static_cast<std::uint8_t>(c.seq & 0xFF), hi = static_cast<std::uint8_t>(c.seq >> 8);
    return {kMagic[0], kMagic[1], kMagic[2], kMagic[3], kind, lo, hi, static_cast<std::uint8_t>(kind ^ lo ^ hi ^ kCheckSalt)};
}

constexpr ctcable::Choice decode(const std::array<std::uint8_t, 8>& r) {
    for (std::size_t i = 0; i < kMagic.size(); i++) if (r[i] != kMagic[i]) return {};
    if (static_cast<std::uint8_t>(r[4] ^ r[5] ^ r[6] ^ kCheckSalt) != r[7]) return {};
    if (r[4] > static_cast<std::uint8_t>(ctcable::Kind::Saved)) return {};
    const ctcable::Choice c{static_cast<ctcable::Kind>(r[4]), static_cast<std::uint16_t>(r[5] | (r[6] << 8))};
    return ctcable::valid(c) ? ctcable::canonical(c) : ctcable::Choice{};
}

inline ctcable::Choice load(fs::Fs& f) {
    std::array<std::uint8_t, 8> r{};
    std::size_t n = 0;
    if (f.read_file(kPath, r, n) != fs::Err::Ok || n != r.size()) return {};
    return decode(r);
}

inline bool save(fs::Fs& f, ctcable::Choice c) {
    const auto r = encode(c);
    return f.write_file(kPath, r) == fs::Err::Ok;
}

// Firmware side (src/chargertest/cablepref.cpp, stubbed in the host UI tests).
ctcable::Choice stored();            // what the volume holds now
bool set(ctcable::Choice c);         // write; true when flash took it

}  // namespace k2::cablepref
