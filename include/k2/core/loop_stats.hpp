// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Main-loop stall counters: what a long LCD paint costs the cooperative loop (docs/ARCH_REWORK.md). Instrumentation only, it
// changes no behaviour. src/core/main_loop.cpp notes
//   - `loop`: the top of every pass (a healthy gap is 1 ms: SysTick ends the wfi), as a histogram with its maximum;
//   - `service`: every 1 ms PD service (fusb302::service from the pass and from the LCD idle hook), the cadence PD sees;
//   - `pages`: the longest pass on each page (Ui::page_index at the end of the pass) and the pixels it sent.
// The remote reads them with `debug loop` (DebugLcd op 1); the host world replay reads the same counters.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace k2::loopstat {

// Bucket i holds gaps up to kEdgesMs[i] ms (inclusive); the last bucket holds everything longer than 100 ms.
inline constexpr std::array<std::uint16_t, 7> kEdgesMs{1, 2, 5, 10, 20, 50, 100};
inline constexpr std::size_t kBuckets = kEdgesMs.size() + 1;
// Rotation pages and submenus (Ui::page_index: 6 pages + 14 menus today); a larger index shares the last slot.
inline constexpr std::size_t kPageSlots = 24;

struct Gaps {
    static constexpr std::size_t bucket(std::uint32_t gap_ms) {
        for (std::size_t i = 0; i < kEdgesMs.size(); i++)
            if (gap_ms <= kEdgesMs[i]) return i;
        return kBuckets - 1;
    }
    void note(std::uint32_t now_ms) {
        if (seen_) {
            const std::uint32_t d = now_ms - last_;
            hist[bucket(d)]++;
            samples++;
            if (d > max_ms) max_ms = d;
        }
        last_ = now_ms;
        seen_ = true;
    }
    void reset() {   // the reference stays: the gap running across the reset is counted at the next note
        hist = {};
        max_ms = 0;
        samples = 0;
    }
    std::array<std::uint32_t, kBuckets> hist{};
    std::uint32_t max_ms = 0, samples = 0;

private:
    std::uint32_t last_ = 0;
    bool seen_ = false;
};

struct Pass {
    std::uint32_t us = 0, px = 0;
};

struct PageWorst {
    static constexpr std::size_t slot(std::uint8_t page) { return page < kPageSlots ? page : kPageSlots - 1; }
    void note(std::uint8_t page, std::uint32_t us, std::uint32_t px) {
        Pass& p = by_page[slot(page)];
        if (us > p.us) p = {us, px};
        if (us > worst.us) { worst = {us, px}; worst_page = page; }
    }
    Pass get(std::uint8_t page) const { return by_page[slot(page)]; }
    void reset() { *this = PageWorst{}; }
    std::array<Pass, kPageSlots> by_page{};
    Pass worst{};
    std::uint8_t worst_page = 0;
};

struct Stats {
    void note_pass(std::uint8_t page, std::uint32_t us, std::uint32_t px) {
        passes++;
        pages.note(page, us, px);
    }
    void reset(std::uint32_t now_ms) {
        loop.reset();
        service.reset();
        pages.reset();
        passes = 0;
        since_ms = now_ms;
    }
    Gaps loop, service;
    PageWorst pages;
    std::uint32_t passes = 0, since_ms = 0;
};

inline Stats stats;

}  // namespace k2::loopstat
