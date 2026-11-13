// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// On-device selftest and DWT cycle benchmark of the hot paths (math, formatting, text and fill, mem*, PD codec and planner,
// PD trace ring, TinyUSB FIFO, flash placement), callable from the USB shell. See docs/MATH_SELFTEST.md.
// Every check runs the production function on deterministic input, validates it, and compares its sum with a golden harvested
// on the host (float checks use a tolerance); drawing goes to a RAM sink, never to the panel.
#pragma once
#include <cstdint>
#include <string_view>

namespace k2::selftest {

using WriteFn = void (*)(std::string_view text, void* ctx);

struct Summary {
    std::uint8_t passed = 0;
    std::uint8_t total = 0;
    std::uint32_t total_cycles = 0;
};

// Runs all deterministic correctness + timing kernels, streaming results via `write`.
// Returns true iff every check passed. Call from the main loop only (the drawing checks borrow the scratch arena and the
// lcd sink); with the arena held, the checks that need it report FAIL rather than touch it.
bool run_math(WriteFn write, void* ctx, Summary* out = nullptr);

}  // namespace k2::selftest
