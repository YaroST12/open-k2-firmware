// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// RAM block device for the littlefs tests, with NOR flash semantics: erase sets a block to 0xFF,
// program can only clear bits (new = old & data) in half-word units. It counts operations and can
// lose power: after cut_after() more program/erase operations the next one is torn (a program writes
// only its first half, an erase only blanks the first half of the block) and every later operation
// fails until revive(). Sweeping the cut point over a whole operation proves it power-fail safe.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "k2/core/fs.hpp"

namespace k2test {

class RamDisk final : public k2::fs::BlockDevice {
public:
    explicit RamDisk(std::uint32_t blocks) : mem_(std::size_t(blocks) * k2::fs::kBlockSize, 0xFF), blocks_(blocks) {}

    int read(std::uint32_t b, std::uint32_t off, void* buf, std::uint32_t n) override {
        if (b >= blocks_ || off + n > k2::fs::kBlockSize) return k2::fs::kIoFail;
        read_bytes += n;
        std::memcpy(buf, &mem_[std::size_t(b) * k2::fs::kBlockSize + off], n);
        return 0;
    }
    int prog(std::uint32_t b, std::uint32_t off, const void* buf, std::uint32_t n) override {
        if (b >= blocks_ || off + n > k2::fs::kBlockSize || (off | n) & 1) return k2::fs::kIoFail;
        ++progs;
        if (dead_) return k2::fs::kIoFail;
        for (std::uint32_t i = 0; i < n; i += 2)   // the device's store_program skips blank half-words
            if (static_cast<const std::uint8_t*>(buf)[i] != 0xFF || static_cast<const std::uint8_t*>(buf)[i + 1] != 0xFF) ++halfwords;
        std::uint32_t take = n;
        if (cut_ == 0) { dead_ = true; take = n / 2 & ~1u; }
        else if (cut_ > 0) --cut_;
        auto* d = &mem_[std::size_t(b) * k2::fs::kBlockSize + off];
        const auto* s = static_cast<const std::uint8_t*>(buf);
        for (std::uint32_t i = 0; i < take; ++i) d[i] &= s[i];
        return dead_ ? k2::fs::kIoFail : 0;
    }
    int erase(std::uint32_t b) override {
        if (b >= blocks_) return k2::fs::kIoFail;
        ++erases;
        if (dead_) return k2::fs::kIoFail;
        std::uint32_t take = k2::fs::kBlockSize;
        if (cut_ == 0) { dead_ = true; take = k2::fs::kBlockSize / 2; }
        else if (cut_ > 0) --cut_;
        std::memset(&mem_[std::size_t(b) * k2::fs::kBlockSize], 0xFF, take);
        return dead_ ? k2::fs::kIoFail : 0;
    }

    void cut_after(int ops) { cut_ = ops; dead_ = false; }   // -1 = never
    void revive() { cut_ = -1; dead_ = false; }
    bool dead() const { return dead_; }
    // Operations since the last call (programs + erases): how many cut points a sweep must cover.
    int ops_done() const { return int(progs + erases); }
    void reset_counters() { progs = erases = halfwords = read_bytes = 0; }
    // What the operations since the last reset hold the main loop for on the device, in ms: about 0.2 ms per programmed
    // half-word (interrupts off per half-word, the loop starved for the whole call) and about 35 ms per sector erase
    // (measured 2026-11-08 on the K2: a 57-row begin of 7.5k half-words and 9 erases took 1.47 to 2.18 s).
    double cost_ms() const { return 0.2 * double(halfwords) + 35.0 * double(erases); }
    std::vector<std::uint8_t>& image() { return mem_; }
    std::uint32_t blocks() const { return blocks_; }

    std::uint64_t progs = 0, erases = 0, halfwords = 0;
    std::uint64_t read_bytes = 0;   // bytes littlefs read: the cost proxy of a volume walk (used_blocks reads every metadata block)

private:
    std::vector<std::uint8_t> mem_;
    std::uint32_t blocks_;
    int cut_ = -1;
    bool dead_ = false;
};

}  // namespace k2test
