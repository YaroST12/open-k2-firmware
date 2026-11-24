// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The diagnosis text writer, shared by the full lines (diag.cpp: "20 V 3.25 A") and the K2 screen's compact form
// (diag_screen.cpp: "20V3.25A", units without a space as the screen's copy rules ask). Internal to the two.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::pd::diag::detail {

struct Out {
    std::span<char> b;
    std::size_t n = 0;
    bool tight = false;   // the screen form: no space before a unit, "12m05s" durations
    [[gnu::noinline]] void c(char ch);
    [[gnu::noinline]] void s(const char* t);
    [[gnu::noinline]] void cap(const char* t);   // capitalised first letter
    [[gnu::noinline]] void unit(const char* t);
    [[gnu::noinline]] void u(std::uint32_t v);
    [[gnu::noinline]] void milli(std::int32_t v, int dec);
    [[gnu::noinline]] void volts(std::int32_t mv);
    [[gnu::noinline]] void volts2(std::int32_t mv);
    [[gnu::noinline]] void amps(std::int32_t ma);
    [[gnu::noinline]] void watts(std::int32_t mw);
    [[gnu::noinline]] void two(std::uint32_t v);
    [[gnu::noinline]] void clock(std::uint32_t ms);
    [[gnu::noinline]] void dur(std::uint32_t ms);
};

}  // namespace k2::pd::diag::detail
