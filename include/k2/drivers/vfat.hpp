// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Synthesized FAT16 volume, pure logic. Nothing is stored: every sector is computed on request
// from a table of files, so the same code serves the MSC read handler and the host tests.
//
// Geometry (fixed): 512 B sectors, 2 sectors per cluster, 16384 sectors (8 MB), 1 reserved sector,
// 2 FATs of 32 sectors, a 512-entry root directory (32 sectors), 8143 clusters (FAT16 needs
// 4085..65524). No MBR: the volume starts at LBA 0 (superfloppy). Every file or directory owns
// one contiguous cluster chain, laid out in table order; a directory gets as many clusters as its
// entries need (a file with a long name takes 1 + ceil(len/13) entries, VFAT style).
// All timestamps are 2026-01-01 00:00 (the K2 has no clock).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace k2::vfat {

inline constexpr std::uint32_t kSectorSize = 512;
inline constexpr std::uint32_t kSectorsPerCluster = 2;
inline constexpr std::uint32_t kClusterSize = kSectorSize * kSectorsPerCluster;
inline constexpr std::uint32_t kTotalSectors = 16384;
inline constexpr std::uint32_t kReserved = 1;
inline constexpr std::uint32_t kFatSectors = 32;
inline constexpr std::uint32_t kRootEntries = 512;
inline constexpr std::uint32_t kRootSectors = kRootEntries * 32 / kSectorSize;
inline constexpr std::uint32_t kRootStart = kReserved + 2 * kFatSectors;
inline constexpr std::uint32_t kDataStart = kRootStart + kRootSectors;
inline constexpr std::uint32_t kClusters = (kTotalSectors - kDataStart) / kSectorsPerCluster;
inline constexpr std::size_t kMaxEntries = 160;      // files plus directories in one volume
inline constexpr std::string_view kVolumeLabel = "K2 DRIVE";

static_assert(kClusters >= 4085 && kClusters < 65525, "must stay FAT16");
static_assert(kClusters + 2 <= kFatSectors * kSectorSize / 2, "FAT must cover every cluster");

using Name83 = std::array<char, 11>;

// "README.TXT" -> "README  TXT" (upper case; the caller keeps to 8.3).
constexpr Name83 name83(std::string_view s) {
    Name83 n{};
    for (auto& c : n) c = ' ';
    std::size_t i = 0, o = 0;
    for (; i < s.size() && s[i] != '.' && o < 8; i++, o++) n[o] = (s[i] >= 'a' && s[i] <= 'z') ? static_cast<char>(s[i] - 32) : s[i];
    while (i < s.size() && s[i] != '.') i++;
    if (i < s.size()) i++;
    for (o = 8; i < s.size() && o < 11; i++, o++) n[o] = (s[i] >= 'a' && s[i] <= 'z') ? static_cast<char>(s[i] - 32) : s[i];
    return n;
}

struct FileSpec {
    Name83 name;
    std::int16_t parent;         // index of the containing directory in the table, -1 for the root
    bool dir;
    std::uint8_t lfn_len;        // length of the optional long name (ASCII, 0 = none); the bytes come from Provider::lfn
    std::uint32_t size;          // bytes, ignored for directories
    std::uint32_t content;       // opaque id handed to the provider
};

// Supplies file bytes: fill `out` with bytes [offset, offset + out.size()) of file `content`.
// Only called inside the file's size.
struct Provider {
    void (*fill)(void* ctx, std::uint32_t content, std::uint32_t offset, std::span<std::uint8_t> out);
    void* ctx;
    // Long name of file `content`: write its ASCII bytes into `out` (at most out.size()) and return the
    // count, which must equal the spec's lfn_len. Called per directory read, so names need no storage.
    std::size_t (*lfn)(void* ctx, std::uint32_t content, std::span<char> out) = nullptr;
};

class Image {
public:
    // Lay out the table. False (volume left empty) when it has too many entries, a bad parent, a
    // non-directory parent, or does not fit. The table must outlive the Image.
    bool build(std::span<const FileSpec> files);

    std::uint32_t sectors() const { return kTotalSectors; }
    std::uint32_t cluster_count() const { return kClusters; }

    // Compute one sector. Sectors past the end read as zeros.
    void read(std::uint32_t lba, std::span<std::uint8_t, kSectorSize> out, const Provider& prov) const;

private:
    struct Placed {
        std::uint16_t first = 0;     // first cluster, 0 for an empty file
        std::uint16_t count = 0;
    };
    std::span<const FileSpec> files_;
    std::array<Placed, kMaxEntries> placed_{};

    void boot_sector(std::span<std::uint8_t, kSectorSize> out) const;
    void fat_sector(std::uint32_t index, std::span<std::uint8_t, kSectorSize> out) const;
    void dir_bytes(int dir, std::uint32_t offset, std::span<std::uint8_t> out, const Provider& prov) const;
    void dir_entry(int dir, std::uint32_t slot, std::span<std::uint8_t, 32> e, const Provider& prov) const;
    int owner_of(std::uint32_t cluster) const;
    std::uint32_t dir_slots(int dir) const;   // 32-byte entries a directory uses (without label)
};

}  // namespace k2::vfat
