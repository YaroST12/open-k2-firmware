// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The memory readout's pure part: row labels, value texts with their units, levels and the footer (k2/core/memstat.hpp).
#include "k2/core/memstat.hpp"

#include <algorithm>
#include <cstring>

namespace k2::memstat {

namespace {

// A bounded writer: never writes past the span, so a cut text stays a prefix.
struct Out {
    std::span<char> buf;
    std::size_t n = 0;
    Out& put(const char* s) {
        while (*s && n < buf.size()) buf[n++] = *s++;
        return *this;
    }
    Out& u32(std::uint32_t v) {
        char tmp[10];
        int i = 0;
        do { tmp[i++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (i && n < buf.size()) buf[n++] = tmp[--i];
        return *this;
    }
    // Bytes in KB (1024) with one decimal, rounded: 64412 -> "62.9".
    Out& kb1(std::uint32_t bytes) {
        const std::uint32_t t = (bytes * 10 + 512) / 1024;   // bytes stay far below 400 MB: no 64-bit needed;
        return u32(t / 10).put(".").u32(t % 10);
    }
};

constexpr const char* kLabels[kRows] = {"Stack now", "Stack low", "IRQ low", "RAM static", "FS used",
                                        "FS free",   "Image",     "Image free", "Heap"};
constexpr bool labels_fit() {
    for (int i = 0; i < kRows; i++)
        if (__builtin_strlen(kLabels[i]) > kLabelMax) return false;
    return true;
}
static_assert(labels_fit(), "a memory label is wider than its column");

Out& blocks(Out& o, std::uint32_t used_or_free_blocks, std::uint32_t block_bytes) {
    return o.u32(used_or_free_blocks * block_bytes / 1024).put(" KB (").u32(used_or_free_blocks).put(" blk)");
}

}  // namespace

const char* row_label(Row r) {
    const int i = static_cast<int>(r);
    return i >= 0 && i < kRows ? kLabels[i] : "";
}

std::size_t row_text(Row r, const Snapshot& s, std::span<char> out) {
    Out o{out.first(std::min(out.size(), kValueMax))};
    if (r == Row::Heap) return o.put("none").n;
    const bool fs_row = r == Row::FsUsed || r == Row::FsFree;
    if (!s.ok || (fs_row && !s.fs_valid)) return o.put("--").n;
    switch (r) {
    case Row::StackNow:  o.u32(s.psp_now).put(" B"); break;
    case Row::StackLow:  o.u32(s.psp_low).put(" B"); break;
    case Row::IrqLow:    o.u32(s.msp_low).put(" B"); break;
    case Row::RamStatic: o.kb1(s.ram_static).put(" of ").kb1(s.ram_total).put(" KB"); break;
    case Row::FsUsed:    blocks(o, s.fs_used, s.fs_block_bytes); break;
    case Row::FsFree:    blocks(o, fs_free_blocks(s), s.fs_block_bytes); break;
    case Row::Image:     o.kb1(s.image_bytes).put(" KB"); break;
    case Row::ImageFree: o.kb1(image_free_bytes(s)).put(" KB"); break;
    default: break;
    }
    return o.n;
}

Level storage_level(const Snapshot& s) {
    return s.ok && s.fs_valid ? level_for_fs(fs_free_blocks(s), s.fs_blocks) : Level::Ok;
}

Level row_level(Row r, const Snapshot& s) { return r == Row::FsFree ? storage_level(s) : Level::Ok; }

Level footer_level(const Snapshot& s) { return storage_level(s); }

std::size_t footer_text(const Snapshot& s, std::span<char> out) {
    Out o{out.first(std::min(out.size(), kFooterMax))};
    if (!s.ok) return o.put("Reading memory").n;
    switch (storage_level(s)) {
    case Level::Ok:      break;
    case Level::Caution: return o.put("Storage low: delete cards").n;
    case Level::Error:   return o.put("Storage full: delete cards").n;
    }
    return o.put("Stack never below ").u32(s.psp_low).put(" B").n;
}

}  // namespace k2::memstat
