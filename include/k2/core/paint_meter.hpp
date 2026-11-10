// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// What painting costs: pixels sent to the panel, core cycles spent inside the lcd stream calls (wire wait and the
// usb::service / idle-hook rounds served inside them included) and inside file-system flash writes (page program and
// sector erase). The lcd driver and fs_flash feed the global `meter`; the charger test opens a window when a row starts
// and logs it when the row ends ([CT] gap ... paint= px= fs=), and `debug lcd` reads the running totals.
// The totals are kept in cycles (64 bit) and cut to ms only when read, so many short calls do not lose their sub-ms parts.
#pragma once
#include <cstdint>

#include "k2/drivers/systick.hpp"

namespace k2::paint {

inline constexpr std::uint32_t kCyclesPerMs = systick::kSysclkHz / 1000;

constexpr std::uint32_t sat32(std::uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v); }
constexpr std::uint32_t ms_of(std::uint64_t cycles) { return sat32(cycles / kCyclesPerMs); }

struct Reading {
    std::uint32_t px = 0, paint_ms = 0, fs_ms = 0;
};

class Meter {
public:
    void add_px(std::uint32_t n) { cur_.px += n; }
    void add_lcd(std::uint32_t cycles) { cur_.lcd += cycles; }
    void add_fs(std::uint32_t cycles) { cur_.fs += cycles; }

    // Running totals since boot.
    Reading total() const { return read(cur_); }
    // Everything counted since begin_window().
    void begin_window() { base_ = cur_; }
    Reading window() const { return read({cur_.px - base_.px, cur_.lcd - base_.lcd, cur_.fs - base_.fs}); }

private:
    struct Sums {
        std::uint64_t px = 0, lcd = 0, fs = 0;
    };
    static Reading read(const Sums& s) { return {sat32(s.px), ms_of(s.lcd), ms_of(s.fs)}; }
    Sums cur_, base_;
};

inline Meter meter;

}  // namespace k2::paint
