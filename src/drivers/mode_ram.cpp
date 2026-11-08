// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The session's lease on the USB-mode overlay (k2/drivers/mode_ram.hpp).
#include "k2/drivers/mode_ram.hpp"

namespace k2::mode_ram {
namespace {
Lease g_lease;
}  // namespace

bool claim(Tenant t) { return g_lease.claim(t); }

bool release(Tenant t) {
    // The caller zeroes its own object first: the next tenant expects the zeroed region Reset_Handler left.
    return g_lease.release(t);
}

}  // namespace k2::mode_ram
