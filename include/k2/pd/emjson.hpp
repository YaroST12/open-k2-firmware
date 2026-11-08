// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// JSON text of one saved cable, generated on read from the binary library entry (the flash format
// stays binary). Layout and field meanings: docs/EMARKER_JSON.md. Pure logic, host-tested.
#pragma once
#include <cstddef>
#include <span>

#include "k2/pd/emlib.hpp"

namespace k2::emjson {

// Writes the document for `e` into `out` (NUL terminated, cut off when it does not fit); returns
// the length without the NUL. About 900 bytes; kMaxSize is always enough.
std::size_t render(const emlib::Entry& e, std::span<char> out);

inline constexpr std::size_t kMaxSize = 1024;

}  // namespace k2::emjson
