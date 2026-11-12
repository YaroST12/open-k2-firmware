// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// RAM image of the store flash for the ring-log tests: every sector of the littlefs base range and of the run
// (flash::fs_block_addr(0..144)), reachable both as littlefs block i (fs::BlockDevice) and by flash address (ringlog::Io),
// so a test can mount a volume and run the rings over the same bytes, as on the device. NOR semantics like RamDisk: erase
// sets a sector to 0xFF, a program clears bits half-word by half-word. cut_after(n): the next n program/erase calls work,
// the one after that is torn (a program writes its first half, an erase blanks the first half of the sector) and every
// later call fails until revive(). Sweeping the cut point over a whole sequence proves it power-cut safe.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "k2/core/fs.hpp"
#include "k2/core/ringlog.hpp"
#include "k2/drivers/flash.hpp"

namespace k2test {

class RamFlash final : public k2::fs::BlockDevice, public k2::ringlog::Io {
public:
    static constexpr std::uint32_t kSectors = k2::flash::kFsGrownBlocks;   // 145
    static constexpr std::uint32_t kSize = k2::flash::kSectorSize;

    RamFlash() : mem_(std::size_t(kSectors) * kSize, 0xFF) {}

    // ---- by flash address (ringlog::Io) ----
    const std::uint8_t* view(std::uint32_t addr) override {
        const long i = index(addr);
        return i < 0 ? nullptr : &mem_[std::size_t(i) * kSize + addr % kSize];
    }
    bool program(std::uint32_t addr, std::span<const std::uint8_t> d) override {
        const long i = index(addr);
        if (i < 0 || addr % 2 || addr % kSize + d.size() > kSize) { ++refused; return false; }
        return prog_at(std::size_t(i) * kSize + addr % kSize, d.data(), std::uint32_t(d.size()));
    }
    bool erase_sector(std::uint32_t addr) override {
        const long i = index(addr);
        if (i < 0 || addr % kSize) { ++refused; return false; }
        return erase_at(std::size_t(i));
    }

    // ---- by littlefs block (fs::BlockDevice) ----
    int read(std::uint32_t b, std::uint32_t off, void* buf, std::uint32_t n) override {
        if (b >= kSectors || off + n > kSize) return k2::fs::kIoFail;
        std::memcpy(buf, &mem_[std::size_t(b) * kSize + off], n);
        return 0;
    }
    int prog(std::uint32_t b, std::uint32_t off, const void* buf, std::uint32_t n) override {
        if (b >= kSectors || off + n > kSize || (off | n) & 1) return k2::fs::kIoFail;
        return prog_at(std::size_t(b) * kSize + off, static_cast<const std::uint8_t*>(buf), n) ? 0 : k2::fs::kIoFail;
    }
    int erase(std::uint32_t b) override {
        if (b >= kSectors) return k2::fs::kIoFail;
        return erase_at(b) ? 0 : k2::fs::kIoFail;
    }

    void cut_after(int ops) { cut_ = ops; dead_ = false; }   // -1 = never
    void revive() { cut_ = -1; dead_ = false; }
    bool dead() const { return dead_; }
    int ops() const { return int(progs + erases); }
    bool blank(std::uint32_t addr) {
        const std::uint8_t* p = view(addr);
        for (std::uint32_t k = 0; k < kSize; ++k) if (p[k] != 0xFF) return false;
        return true;
    }
    std::vector<std::uint8_t>& image() { return mem_; }

    std::uint64_t progs = 0, erases = 0, halfwords = 0, refused = 0;

private:
    std::vector<std::uint8_t> mem_;
    int cut_ = -1;
    bool dead_ = false;

    static long index(std::uint32_t addr) {
        using namespace k2::flash;
        if (addr >= kFsBase && addr < kFsEnd) return long((addr - kFsBase) / kSize);
        if (addr >= kFsRunBase && addr < kFsRunEnd) return long(kFsBlocks + (addr - kFsRunBase) / kSize);
        return -1;
    }
    bool prog_at(std::size_t at, const std::uint8_t* s, std::uint32_t n) {
        ++progs;
        if (dead_) return false;
        std::uint32_t take = n;
        if (cut_ == 0) { dead_ = true; take = n / 2 & ~1u; }
        else if (cut_ > 0) --cut_;
        for (std::uint32_t i = 0; i < n; i += 2)
            if (s[i] != 0xFF || (i + 1 < n && s[i + 1] != 0xFF)) ++halfwords;
        for (std::uint32_t i = 0; i < take; ++i) mem_[at + i] &= s[i];
        return !dead_;
    }
    bool erase_at(std::size_t i) {
        ++erases;
        if (dead_) return false;
        std::uint32_t take = kSize;
        if (cut_ == 0) { dead_ = true; take = kSize / 2; }
        else if (cut_ > 0) --cut_;
        std::memset(&mem_[i * kSize], 0xFF, take);
        return !dead_;
    }
};

}  // namespace k2test
