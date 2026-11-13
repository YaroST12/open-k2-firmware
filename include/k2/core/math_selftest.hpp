// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Self-contained math/FPU/architecture selftest & DWT cycle micro-benchmark, callable from the USB shell.
// Depends only on pre-existing pure project headers so the exact same files run in pre- and post-optimization builds.
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
// Returns true iff every check passed.
bool run_math(WriteFn write, void* ctx, Summary* out = nullptr);

}  // namespace k2::selftest
