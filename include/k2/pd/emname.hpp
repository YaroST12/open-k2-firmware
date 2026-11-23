// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Auto name of a saved cable, the same shape as a charger card's: "<Brand> <wattage>", e.g. "Anker 100W",
// "UGREEN 240W", when the ID header VID is listed (pd::vendor_find). An unlisted VID keeps
// "0xVVVV-<version>-<wattage>" (version: cable VDO 1 bits 2:0: USB2, USB3.2G1, USB3.2G2, USB4G3, USB4G4), so two unknown
// makers still differ. Pure logic over a 66-byte e-marker record (k2/pd/emrec.hpp), host-tested.
//   wattage: max voltage x max current from the cable VDO, as pd::parse_emarker: 60W (3 A),
//            100W (5 A, 20 V) or 240W (5 A, 50 V); the deprecated 30 V / 40 V codes read as 20 V
// Every name is a valid user name (emlib::name_valid) of at most kMaxLen characters (the NUL is extra); a long
// manufacturer is cut, never the rest. `ordinal` >= 2 appends "-2", "-3", ... for a second and later cable that would
// get the same name (emlib::unique_name): a same-brand, same-wattage cable of another speed is told apart that way.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/pd/emrec.hpp"

namespace k2::emname {

inline constexpr std::size_t kMaxLen = 24;   // characters, the same limit as a user's name (emlib::kNameMax)

std::array<char, kMaxLen + 1> make(const emrec::Record& rec, unsigned ordinal = 1);

// 8.3 short name "BASE    JSN" (11 bytes, space padded) for a drive file: the name upper-cased
// with every character outside [A-Za-z0-9_-] dropped ("CABLE" when nothing is left). A base longer
// than 8 characters, or one already in `taken`, becomes <first 6 or fewer>~N (N = 1, 2, ...).
using Short83 = std::array<char, 11>;
Short83 short_name(std::string_view name, std::span<const Short83> taken);
// Same, with the taken set asked through a callback (ctx is handed back): lets a caller that already
// holds the names elsewhere skip building an array of them on its stack.
using TakenFn = bool (*)(const void* ctx, const Short83& name);
Short83 short_name(std::string_view name, TakenFn taken, const void* ctx);

}  // namespace k2::emname
