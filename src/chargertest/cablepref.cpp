// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The remembered cable identity on the littlefs volume. See include/k2/chargertest/cablepref.hpp.
#include "k2/chargertest/cablepref.hpp"

#include "k2/drivers/fs_flash.hpp"
#include "k2/core/log.hpp"

namespace k2::cablepref {

ctcable::Choice stored() { return load(fs::instance()); }

bool set(ctcable::Choice c) {
    const bool ok = save(fs::instance(), c);
    K2LOG("[CABLE] selection kind=%u seq=%u (%s)\n", unsigned(c.kind), unsigned(c.seq), ok ? "saved" : "FLASH ERROR");
    return ok;
}

}  // namespace k2::cablepref
