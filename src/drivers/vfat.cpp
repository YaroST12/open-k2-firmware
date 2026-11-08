// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/drivers/vfat.hpp"

#include <algorithm>
#include <cstring>

namespace k2::vfat {

namespace {
constexpr std::uint16_t kDate = ((2026 - 1980) << 9) | (1 << 5) | 1;   // 2026-01-01
constexpr std::uint16_t kEoc = 0xFFFF;

void le16(std::uint8_t* p, std::uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
void le32(std::uint8_t* p, std::uint32_t v) { le16(p, v & 0xFFFF); le16(p + 2, v >> 16); }
std::uint32_t clusters_for(std::uint32_t bytes) { return (bytes + kClusterSize - 1) / kClusterSize; }
std::uint32_t lfn_count(const FileSpec& f) { return f.lfn_len ? (f.lfn_len + 12u) / 13u : 0u; }
std::uint8_t short_sum(const Name83& n) {
    std::uint8_t s = 0;
    for (char ch : n) s = static_cast<std::uint8_t>(((s & 1) << 7) + (s >> 1) + static_cast<std::uint8_t>(ch));
    return s;
}
constexpr std::uint8_t kLfnOff[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
}  // namespace

bool Image::build(std::span<const FileSpec> files) {
    files_ = {};
    placed_ = {};
    if (files.size() > kMaxEntries) return false;
    std::uint32_t next = 2;
    std::array<Placed, kMaxEntries> p{};
    for (std::size_t i = 0; i < files.size(); i++) {
        const FileSpec& f = files[i];
        if (f.parent >= static_cast<int>(files.size()) || f.parent < -1) return false;
        if (f.parent >= 0 && !files[f.parent].dir) return false;
        std::uint32_t n = clusters_for(f.size);
        if (f.dir) {
            std::uint32_t slots = 2;
            for (const auto& k : files) if (k.parent == static_cast<int>(i)) slots += 1 + lfn_count(k);
            n = clusters_for(slots * 32);
        }
        if (next + n > kClusters + 2) return false;
        p[i].first = n ? static_cast<std::uint16_t>(next) : 0;
        p[i].count = static_cast<std::uint16_t>(n);
        next += n;
    }
    {   // the root directory is fixed size: label plus entries must fit
        std::uint32_t slots = 1;
        for (const auto& f : files) if (f.parent < 0) slots += 1 + lfn_count(f);
        if (slots > kRootEntries) return false;
    }
    files_ = files;
    placed_ = p;
    return true;
}

int Image::owner_of(std::uint32_t cluster) const {
    for (std::size_t i = 0; i < files_.size(); i++)
        if (placed_[i].count && cluster >= placed_[i].first && cluster < placed_[i].first + placed_[i].count) return static_cast<int>(i);
    return -1;
}

std::uint32_t Image::dir_slots(int dir) const {
    std::uint32_t n = 0;
    for (std::size_t i = 0; i < files_.size(); i++)
        if (files_[i].parent == dir) n += 1 + lfn_count(files_[i]);
    return n;
}

void Image::boot_sector(std::span<std::uint8_t, kSectorSize> b) const {
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    std::memcpy(&b[3], "K2DRIVE ", 8);
    le16(&b[11], kSectorSize);
    b[13] = kSectorsPerCluster;
    le16(&b[14], kReserved);
    b[16] = 2;
    le16(&b[17], kRootEntries);
    le16(&b[19], kTotalSectors);
    b[21] = 0xF8;
    le16(&b[22], kFatSectors);
    le16(&b[24], 32);            // sectors per track (nominal)
    le16(&b[26], 64);            // heads (nominal)
    le32(&b[28], 0);
    le32(&b[32], 0);             // total sectors (32-bit), unused
    b[36] = 0x80;                // drive number
    b[38] = 0x29;
    le32(&b[39], 0x4B320001);    // volume serial
    for (int i = 0; i < 11; i++) b[43 + i] = i < static_cast<int>(kVolumeLabel.size()) ? kVolumeLabel[i] : ' ';
    std::memcpy(&b[54], "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
}

void Image::fat_sector(std::uint32_t index, std::span<std::uint8_t, kSectorSize> out) const {
    for (std::uint32_t k = 0; k < kSectorSize / 2; k++) {
        const std::uint32_t c = index * (kSectorSize / 2) + k;
        std::uint32_t v = 0;
        if (c == 0) v = 0xFFF8;
        else if (c == 1) v = kEoc;
        else {
            const int o = owner_of(c);
            if (o >= 0) v = (c + 1 < static_cast<std::uint32_t>(placed_[o].first) + placed_[o].count) ? c + 1 : kEoc;
        }
        le16(&out[k * 2], v);
    }
}

void Image::dir_entry(int dir, std::uint32_t slot, std::span<std::uint8_t, 32> e, const Provider& prov) const {
    std::memset(e.data(), 0, 32);
    std::uint32_t s = slot;
    if (dir < 0) {
        if (s == 0) {                                   // volume label
            for (int i = 0; i < 11; i++) e[i] = i < static_cast<int>(kVolumeLabel.size()) ? kVolumeLabel[i] : ' ';
            e[11] = 0x08;
            le16(&e[24], kDate);
            return;
        }
        s -= 1;
    } else {
        if (s < 2) {                                    // "." and ".."
            std::memset(e.data(), ' ', 11);
            e[0] = '.';
            if (s == 1) e[1] = '.';
            e[11] = 0x10;
            std::uint32_t cl = placed_[dir].first;
            if (s == 1) {
                const int par = files_[dir].parent;
                cl = par < 0 ? 0 : placed_[par].first;
            }
            le16(&e[24], kDate); le16(&e[16], kDate); le16(&e[18], kDate);
            le16(&e[26], cl);
            return;
        }
        s -= 2;
    }
    int c = -1;
    std::uint32_t lfn_n = 0, idx = 0;                   // idx: entry within the child's run
    for (std::size_t i = 0; i < files_.size(); i++) {
        if (files_[i].parent != dir) continue;
        const std::uint32_t run = 1 + lfn_count(files_[i]);
        if (s < run) { c = static_cast<int>(i); lfn_n = run - 1; idx = s; break; }
        s -= run;
    }
    if (c < 0) return;                                  // all-zero entry: end of directory
    const FileSpec& f = files_[c];
    if (idx < lfn_n) {                                  // long-name entry, last piece first
        const std::uint32_t n = lfn_n - idx;            // 1-based piece number
        e[0] = static_cast<std::uint8_t>(n | (n == lfn_n ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = short_sum(f.name);
        std::array<char, 256> nm{};
        if (prov.lfn) prov.lfn(prov.ctx, f.content, std::span<char>(nm.data(), f.lfn_len));
        for (std::uint32_t j = 0; j < 13; j++) {
            const std::uint32_t k = (n - 1) * 13 + j;
            std::uint32_t ch = k < f.lfn_len ? static_cast<std::uint8_t>(nm[k]) : (k == f.lfn_len ? 0u : 0xFFFFu);
            le16(&e[kLfnOff[j]], ch);
        }
        return;
    }
    std::memcpy(e.data(), f.name.data(), 11);
    e[11] = f.dir ? 0x10 : 0x20;
    le16(&e[16], kDate); le16(&e[18], kDate); le16(&e[24], kDate);
    le16(&e[26], placed_[c].first);
    le32(&e[28], f.dir ? 0 : f.size);
}

void Image::dir_bytes(int dir, std::uint32_t offset, std::span<std::uint8_t> out, const Provider& prov) const {
    std::array<std::uint8_t, 32> e;
    for (std::size_t i = 0; i < out.size();) {
        const std::uint32_t pos = offset + static_cast<std::uint32_t>(i);
        dir_entry(dir, pos / 32, std::span<std::uint8_t, 32>(e), prov);
        const std::size_t in = pos % 32;
        const std::size_t n = std::min<std::size_t>(32 - in, out.size() - i);
        std::memcpy(&out[i], &e[in], n);
        i += n;
    }
}

void Image::read(std::uint32_t lba, std::span<std::uint8_t, kSectorSize> out, const Provider& prov) const {
    std::memset(out.data(), 0, kSectorSize);
    if (lba == 0) { boot_sector(out); return; }
    if (lba < kRootStart) {
        const std::uint32_t f = (lba - kReserved) % kFatSectors;
        fat_sector(f, out);
        return;
    }
    if (lba < kDataStart) { dir_bytes(-1, (lba - kRootStart) * kSectorSize, out, prov); return; }
    if (lba >= kTotalSectors) return;
    const std::uint32_t rel = lba - kDataStart;
    const std::uint32_t cluster = 2 + rel / kSectorsPerCluster;
    const int o = owner_of(cluster);
    if (o < 0) return;
    const std::uint32_t offset = (cluster - placed_[o].first) * kClusterSize + (rel % kSectorsPerCluster) * kSectorSize;
    if (files_[o].dir) { dir_bytes(o, offset, out, prov); return; }
    if (offset >= files_[o].size) return;
    const std::size_t n = std::min<std::size_t>(kSectorSize, files_[o].size - offset);
    prov.fill(prov.ctx, files_[o].content, offset, std::span<std::uint8_t>(out.data(), n));
}

}  // namespace k2::vfat
