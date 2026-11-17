// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Xiaomi 0x2717 (Mi Turbo Charge) auth key table — PUBLIC DECLARATIONS ONLY. The key bytes live
// in the gitignored priv/xiaomi_keys_priv.h and are never committed; tracked code references the
// entries only by variable name (k2::pd::mi::kSessionSeed / kSecret).
// A checkout without priv/ builds against the zero stubs below: everything compiles and runs,
// the auth digests just match no real charger and the chain ends un-authed.
#pragma once

#include <cstdint>

namespace k2::pd::mi {

inline constexpr unsigned kKeyCount = 10;
inline constexpr unsigned kActiveKeyIndex = 7;   // the seed we send in cmd 0x27170104

#if __has_include("../../../priv/xiaomi_keys_priv.h")
#include "../../../priv/xiaomi_keys_priv.h"   // defines kSessionSeed / kSecret in this namespace
#else
inline constexpr std::uint32_t kSessionSeed[kKeyCount][4] = {};
inline constexpr std::uint8_t kSecret[kKeyCount][32] = {};
#endif

}  // namespace k2::pd::mi
