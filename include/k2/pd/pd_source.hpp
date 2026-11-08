// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Source-capability access for the UI and the remote channel. active_caps() is the live
// FUSB302 table unless the debug override is on (loaded by `k2_remote.py debug pdos --fake N`),
// so the PD page can be exercised without a charger. Pure logic apart from that one call.
#pragma once
#include <cstdint>

#include "k2/drivers/fusb302.hpp"
#include "k2/pd/pd.hpp"

namespace k2::pd {

// Fill `out` with n synthetic PDOs (FIX 5/9/15/20 V, then two PPS and one AVS); n is clamped
// to kMaxPdos. Marked valid, PD 3.1, rx_count 0.
void fake_fill(SourceCaps& out, unsigned n);

void set_fake(unsigned n);  // 0 clears the override
bool is_fake();
const SourceCaps& active_caps();

}  // namespace k2::pd
