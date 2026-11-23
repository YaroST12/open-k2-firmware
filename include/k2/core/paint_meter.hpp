// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// What painting costs: pixels sent to the panel, core cycles spent inside the lcd stream calls (wire wait and the
// usb::service / idle-hook rounds served inside them included) and inside file-system flash writes (page program and
// sector erase). The lcd driver and fs_flash feed the global `meter`; the charger test opens a window when a row starts
// and logs it when the row ends ([CT] gap ... paint= px= fs=), and `debug lcd` reads the running totals.
// The totals are kept in cycles (64 bit) and cut to ms only when read, so many short calls do not lose their sub-ms parts.
#pragma once
#include <cstdint>

#include "k2/core/math_opt.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::paint {

inline constexpr std::uint32_t kCyclesPerMs = systick::kSysclkHz / 1000;

constexpr std::uint32_t sat32(std::uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v); }
constexpr std::uint32_t ms_of(std::uint64_t cycles) { return sat32(math::div_u64_u32(cycles, kCyclesPerMs)); }

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
    // Pixels sent since boot, not saturated: a difference of two reads is exact (the UI prices an erase replay with it).
    std::uint64_t px_count() const { return cur_.px; }
    // Everything counted since begin_window().
    void begin_window() { base_ = cur_; }
    Reading window() const { return read({cur_.px - base_.px, cur_.lcd - base_.lcd, cur_.fs - base_.fs}); }

    // Overlay watch (the UI's toast pill): the panel windows opened between watch_record() and watch_set() join the
    // watched rectangle; a later panel window that crosses it sets watch_hit(). Panel coordinates only (the lcd driver
    // feeds panel_window() past its sink check), so every rotation and viewport map is already applied. Pills share one
    // band but differ in width, so the rectangle only grows, until a whole-panel window (a rotation, a full clear) drops
    // it. With no rectangle, watch_hit() is true: the caller repaints its overlay as if covered.
    // Two overlays can be watched (kWatches): slot 0 the toast pill, slot 1 the shell-access band.
    void panel_window(std::uint16_t x0, std::uint16_t y0, std::uint16_t x1, std::uint16_t y1) {
        if (rec_) {
            if (x0 < r_[0]) r_[0] = x0;
            if (y0 < r_[1]) r_[1] = y0;
            if (x1 > r_[2]) r_[2] = x1;
            if (y1 > r_[3]) r_[3] = y1;
            return;
        }
        const bool whole = x0 == 0 && y0 == 0 && x1 >= kPanelMax && y1 >= kPanelMax;
        for (unsigned i = 0; i < kWatches; i++) {
            if (whole) valid_[i] = false;
            if (x0 <= w_[i][2] && x1 >= w_[i][0] && y0 <= w_[i][3] && y1 >= w_[i][1]) hit_[i] = true;
        }
    }
    void watch_record(unsigned slot = 0) { rec_ = true; slot_ = slot; r_[0] = r_[1] = 0xFFFF; r_[2] = r_[3] = 0; }
    void watch_set() {
        rec_ = false;
        if (r_[0] <= r_[2]) {
            std::uint16_t (&w)[4] = w_[slot_];
            if (!valid_[slot_]) { w[0] = w[1] = 0xFFFF; w[2] = w[3] = 0; }
            for (int i = 0; i < 2; i++) if (r_[i] < w[i]) w[i] = r_[i];
            for (int i = 2; i < 4; i++) if (r_[i] > w[i]) w[i] = r_[i];
            valid_[slot_] = true;
            hit_[slot_] = false;   // only an overlay that reached the panel is uncovered (one drawn into a capture sink is not)
        }
    }
    bool watch_hit(unsigned slot = 0) const { return hit_[slot] || !valid_[slot]; }

private:
    struct Sums {
        std::uint64_t px = 0, lcd = 0, fs = 0;
    };
    static Reading read(const Sums& s) { return {sat32(s.px), ms_of(s.lcd), ms_of(s.fs)}; }
    Sums cur_, base_;
    static constexpr std::uint16_t kPanelMax = 239;   // lcd::kWidth - 1 (this header stays free of driver headers)
    static constexpr unsigned kWatches = 2;
    std::uint16_t r_[4] = {0xFFFF, 0xFFFF, 0, 0}, w_[kWatches][4] = {{0xFFFF, 0xFFFF, 0, 0}, {0xFFFF, 0xFFFF, 0, 0}};
    unsigned slot_ = 0;
    bool rec_ = false, hit_[kWatches] = {}, valid_[kWatches] = {};
};

inline Meter meter;

}  // namespace k2::paint
