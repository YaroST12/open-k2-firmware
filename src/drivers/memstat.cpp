// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The memory readout on the K2: see k2/core/memstat.hpp. Stack figures from the painted-stack scans (startup.cpp), static
// sizes from linker symbols, the littlefs figure from the Fs cache. poll() runs in the main loop only, so the cached Snapshot
// needs no lock; readers (UI, remote, shell) run in the same loop and copy it.
#include "k2/core/memstat.hpp"

#include "k2/drivers/fs_flash.hpp"

extern "C" {
extern std::uint32_t _estack, _psp_top, _eguard, _sdata, _ebss, _sidata, _edata, _flash_start, _flash_end;
std::uint32_t k2_stack_free_bytes();
std::uint32_t k2_msp_free_bytes();
}

namespace k2::memstat {

namespace {

constexpr std::uint32_t kScanGapMs = 1000;   // the stack scans (about 1300 words each) run at most once a second
constexpr std::uint32_t kWalkGapMs = 2000;   // a littlefs tree walk (tens of ms) at most every 2 s, and only when stale

Snapshot g;
std::uint32_t g_last_scan_ms, g_last_walk_ms;

std::uint32_t addr(const std::uint32_t& sym) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&sym)); }

void refresh_stack() {
    g.ram_total = addr(_estack) - addr(_sdata);
    g.ram_static = addr(_ebss) - addr(_sdata);
    g.psp_size = addr(_psp_top) - addr(_eguard);
    g.msp_size = addr(_estack) - addr(_psp_top);
    g.psp_low = k2_stack_free_bytes();
    g.msp_low = k2_msp_free_bytes();
    // The frame of this function sits a little below the main loop's own frame: close enough for "headroom now".
    const std::uint32_t sp = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));
    g.psp_now = sp > addr(_eguard) && sp <= addr(_psp_top) ? sp - addr(_eguard) : 0;
    const std::uint32_t image_end = addr(_sidata) + (addr(_edata) - addr(_sdata));
    g.image_bytes = image_end - addr(_flash_start);
    g.flash_bytes = addr(_flash_end) - addr(_flash_start);
}

}  // namespace

void poll(std::uint32_t now_ms, bool may_walk) {
    if (!g.ok || now_ms - g_last_scan_ms >= kScanGapMs) {
        g_last_scan_ms = now_ms;
        refresh_stack();
        g.ok = true;
    }
    fs::Fs& f = fs::instance();
    g.fs_blocks = f.block_count();
    g.fs_block_bytes = static_cast<std::uint16_t>(fs::kBlockSize);
    if (f.used_known()) {
        g.fs_used = f.used_blocks();                        // cached: no walk
        g.fs_valid = true;
    } else if (may_walk && now_ms - g_last_walk_ms >= kWalkGapMs) {
        g_last_walk_ms = now_ms;
        const std::uint32_t used = f.used_blocks();         // the one walk; a failed walk is not cached and stays unknown
        if (f.used_known()) { g.fs_used = used; g.fs_valid = true; }
    }
}

Snapshot snapshot() { return g; }

bool scrub_storage(std::uint32_t& erased) {
    return fs::instance().scrub_free_blocks(erased) == fs::Err::Ok;
}

}  // namespace k2::memstat
