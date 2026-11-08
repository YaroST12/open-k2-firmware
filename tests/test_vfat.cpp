// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the synthesized FAT16 volume (k2::vfat). The image is also dumped to
// $K2_DUMP/vfat.img so tests/test_vfat_image.py can run fsck.fat and a Python FAT reader over it.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "k2/drivers/vfat.hpp"
#include "test_util.h"

using namespace k2::vfat;

// Content: byte i of content c is (c * 31 + i) & 0xFF, so every offset is checkable.
static void fill(void*, std::uint32_t content, std::uint32_t offset, std::span<std::uint8_t> out) {
    for (std::size_t i = 0; i < out.size(); i++) out[i] = static_cast<std::uint8_t>(content * 31 + offset + i);
}
static const char* lfn_name(int i);
// Long names are produced on demand: content 1 is the generic long name, content 10+i is lfn_name(i).
static std::size_t lfn(void*, std::uint32_t content, std::span<char> out) {
    const char* n = content == 1 ? "A-very-long-file-name-over-26-chars.dat" : lfn_name(static_cast<int>(content) - 10);
    const std::size_t len = std::strlen(n);
    for (std::size_t i = 0; i < len && i < out.size(); i++) out[i] = n[i];
    return len;
}
static const Provider kProv{fill, nullptr, lfn};

enum { kReadme, kDir, kSub, kEmpty, kBig, kNum };
static const std::array<FileSpec, kNum> kSpecs = {{
    {name83("README.TXT"), -1, false, 0, 100, 1},
    {name83("EMARKERS"), -1, true, 0, 0, 0},
    {name83("A.BIN"), 1, false, 0, 66, 2},
    {name83("EMPTY.TXT"), -1, false, 0, 0, 3},
    {name83("BIG.DAT"), -1, false, 0, 5000, 4},   // 5 clusters
}};

static std::vector<std::uint8_t> render(const Image& img) {
    std::vector<std::uint8_t> v(static_cast<std::size_t>(img.sectors()) * kSectorSize);
    for (std::uint32_t s = 0; s < img.sectors(); s++)
        img.read(s, std::span<std::uint8_t, kSectorSize>(&v[s * kSectorSize], kSectorSize), kProv);
    return v;
}
static unsigned le16(const std::uint8_t* p) { return p[0] | (p[1] << 8); }
static unsigned le32(const std::uint8_t* p) { return le16(p) | (le16(p + 2) << 16); }

static void test_name83() {
    CHECK_EQ(std::memcmp(name83("README.TXT").data(), "README  TXT", 11), 0);
    CHECK_EQ(std::memcmp(name83("EMARKERS").data(), "EMARKERS   ", 11), 0);
    CHECK_EQ(std::memcmp(name83("a.bin").data(), "A       BIN", 11), 0);
}

static void test_geometry_and_boot() {
    Image img;
    CHECK_EQ(img.build(kSpecs), 1);
    CHECK_EQ(img.sectors(), 16384u);
    const auto v = render(img);
    const std::uint8_t* b = v.data();
    CHECK_EQ(b[0], 0xEB); CHECK_EQ(b[2], 0x90);
    CHECK_EQ(le16(b + 11), 512u);          // bytes per sector
    CHECK_EQ(b[13], 2);                    // sectors per cluster
    CHECK_EQ(le16(b + 14), 1u);            // reserved
    CHECK_EQ(b[16], 2);                    // FATs
    CHECK_EQ(le16(b + 17), 512u);          // root entries
    CHECK_EQ(le16(b + 19), 16384u);        // total sectors
    CHECK_EQ(b[21], 0xF8);                 // media
    CHECK_EQ(le16(b + 22), 32u);           // sectors per FAT
    CHECK_EQ(le32(b + 28), 0u);            // hidden sectors
    CHECK_EQ(b[38], 0x29);                 // extended boot signature
    CHECK_EQ(std::memcmp(b + 54, "FAT16   ", 8), 0);
    CHECK_EQ(b[510], 0x55); CHECK_EQ(b[511], 0xAA);
    // 16-bit FAT: cluster count in [4085, 65525)
    const unsigned data_start = 1 + 2 * 32 + 32;
    const unsigned clusters = (16384 - data_start) / 2;
    CHECK_EQ(clusters >= 4085 && clusters < 65525, 1);
    CHECK_EQ(img.cluster_count(), clusters);
}

static void test_fat_and_directory() {
    Image img;
    CHECK_EQ(img.build(kSpecs), 1);
    const auto v = render(img);
    const std::uint8_t* fat1 = &v[1 * 512];
    const std::uint8_t* fat2 = &v[(1 + 32) * 512];
    CHECK_EQ(std::memcmp(fat1, fat2, 32 * 512), 0);                    // both copies equal
    CHECK_EQ(le16(fat1), 0xFFF8u); CHECK_EQ(le16(fat1 + 2), 0xFFFFu);
    // clusters in table order: README(2) EMARKERS(3) A.BIN(4) EMPTY(none) BIG(5..9)
    CHECK_EQ(le16(fat1 + 2 * 2), 0xFFFFu);
    CHECK_EQ(le16(fat1 + 2 * 3), 0xFFFFu);
    CHECK_EQ(le16(fat1 + 2 * 4), 0xFFFFu);
    for (int c = 5; c < 9; c++) CHECK_EQ(le16(fat1 + 2 * c), static_cast<unsigned>(c + 1));
    CHECK_EQ(le16(fat1 + 2 * 9), 0xFFFFu);
    CHECK_EQ(le16(fat1 + 2 * 10), 0u);                                  // free

    const std::uint8_t* root = &v[(1 + 64) * 512];
    CHECK_EQ(std::memcmp(root, "K2 DRIVE   ", 11), 0);                  // volume label
    CHECK_EQ(root[11], 0x08);
    auto ent = [&](int i) { return root + 32 * (1 + i); };
    CHECK_EQ(std::memcmp(ent(0), "README  TXT", 11), 0);
    CHECK_EQ(ent(0)[11], 0x20);                                         // archive
    CHECK_EQ(le16(ent(0) + 26), 2u); CHECK_EQ(le32(ent(0) + 28), 100u);
    CHECK_EQ(le16(ent(0) + 24), 0x5C21u);                               // 2026-01-01
    CHECK_EQ(std::memcmp(ent(1), "EMARKERS   ", 11), 0);
    CHECK_EQ(ent(1)[11], 0x10);                                         // directory
    CHECK_EQ(le16(ent(1) + 26), 3u); CHECK_EQ(le32(ent(1) + 28), 0u);
    CHECK_EQ(le16(ent(2) + 26), 0u); CHECK_EQ(le32(ent(2) + 28), 0u);   // EMPTY.TXT: no cluster
    CHECK_EQ(le16(ent(3) + 26), 5u); CHECK_EQ(le32(ent(3) + 28), 5000u);
    CHECK_EQ(ent(4)[0], 0);                                             // end of directory

    const std::uint8_t* sub = &v[(97 + (3 - 2) * 2) * 512];             // cluster 3
    CHECK_EQ(std::memcmp(sub, ".          ", 11), 0);
    CHECK_EQ(le16(sub + 26), 3u);
    CHECK_EQ(std::memcmp(sub + 32, "..         ", 11), 0);
    CHECK_EQ(le16(sub + 32 + 26), 0u);                                  // parent is the root
    CHECK_EQ(std::memcmp(sub + 64, "A       BIN", 11), 0);
    CHECK_EQ(le16(sub + 64 + 26), 4u); CHECK_EQ(le32(sub + 64 + 28), 66u);
    CHECK_EQ(sub[96], 0);
}

static void test_file_data() {
    Image img;
    CHECK_EQ(img.build(kSpecs), 1);
    const auto v = render(img);
    auto cluster = [&](unsigned c) { return &v[(97 + (c - 2) * 2) * 512]; };
    for (unsigned i = 0; i < 100; i++) CHECK_EQ(cluster(2)[i], static_cast<std::uint8_t>(31 + i));
    for (unsigned i = 100; i < 1024; i++) CHECK_EQ(cluster(2)[i], 0);   // zero padding past the size
    for (unsigned i = 0; i < 66; i++) CHECK_EQ(cluster(4)[i], static_cast<std::uint8_t>(62 + i));
    for (unsigned i = 0; i < 5000; i++) CHECK_EQ(cluster(5)[i], static_cast<std::uint8_t>(124 + i));
    for (unsigned i = 5000 - 4096; i < 1024; i++) CHECK_EQ(cluster(9)[i], 0);
    // an unused cluster reads as zeros
    for (unsigned i = 0; i < 1024; i++) CHECK_EQ(cluster(100)[i], 0);
    // past the end of the volume: zeros, no overrun
    std::array<std::uint8_t, 512> s;
    s.fill(0xAA);
    img.read(img.sectors() + 5, std::span<std::uint8_t, 512>(s), kProv);
    CHECK_EQ(s[0], 0);
}

static void test_limits() {
    Image img;
    CHECK_EQ(img.build(std::span<const FileSpec>()), 1);                // empty volume is fine
    std::array<FileSpec, 1> huge = {{{name83("HUGE.BIN"), -1, false, 0, 9u * 1024 * 1024, 1}}};
    CHECK_EQ(img.build(huge), 0);                                       // does not fit 8 MB
    std::array<FileSpec, 1> bad = {{{name83("X.BIN"), 3, false, 0, 1, 1}}};
    CHECK_EQ(img.build(bad), 0);                                        // parent index out of range
    std::array<FileSpec, 2> notdir = {{{name83("F.BIN"), -1, false, 0, 1, 1}, {name83("G.BIN"), 0, false, 0, 1, 1}}};
    CHECK_EQ(img.build(notdir), 0);                                     // parent must be a directory
    std::array<FileSpec, kMaxEntries + 1> many{};
    for (auto& f : many) f = {name83("N.BIN"), -1, false, 0, 1, 1};
    CHECK_EQ(img.build(many), 0);
}


// Long names: a CABLES directory of 40 files whose names need 3 LFN pieces each, so the directory
// spans several clusters. Dumped to vfat_lfn.img for the independent reader.
static std::array<char, 40 * 32> g_names;
static std::array<FileSpec, 41> g_lfn_specs;
static const char* lfn_name(int i) { return &g_names[i * 32]; }
static void build_lfn_specs() {
    g_lfn_specs[0] = {name83("CABLES"), -1, true, 0, 0, 0};
    for (int i = 0; i < 40; i++) {
        char* n = &g_names[i * 32];
        const int len = std::snprintf(n, 32, "Apple-USB4-240W-%02d.json", i);
        char sh[16];
        std::snprintf(sh, sizeof sh, "CAB%02d.JSN", i);
        g_lfn_specs[1 + i] = {name83(sh), 0, false, static_cast<std::uint8_t>(len), 66, static_cast<std::uint32_t>(i + 10)};
    }
}

static void test_long_names() {
    build_lfn_specs();
    Image img;
    CHECK_EQ(img.build(g_lfn_specs), 1);
    const auto v = render(img);
    // root: label + CABLES (no long name) -> the directory is 2 + 40 * 3 + ... slots
    const std::uint8_t* root = &v[(1 + 64) * 512];
    CHECK_EQ(std::memcmp(root + 32, "CABLES     ", 11), 0);
    const unsigned dcl = le16(root + 32 + 26);
    const std::uint8_t* dir = &v[(97 + (dcl - 2) * 2) * 512];
    // entry 0 of the dir: "." ; first child starts at slot 2 with the last LFN piece
    const std::uint8_t* e = dir + 64;
    const int len = static_cast<int>(std::strlen(lfn_name(0)));          // 24 -> 2 pieces
    const int pieces = (len + 12) / 13;
    CHECK_EQ(e[0], 0x40 | pieces);
    CHECK_EQ(e[11], 0x0F);
    CHECK_EQ(e[12], 0); CHECK_EQ(le16(e + 26), 0u);
    unsigned sum = 0;
    for (int i = 0; i < 11; i++) sum = (((sum & 1) << 7) + (sum >> 1) + "CAB00   JSN"[i]) & 0xFF;
    CHECK_EQ(e[13], sum);
    // last piece holds chars 13.., NUL after the name, 0xFFFF padding
    const std::uint8_t off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int j = 0; j < 13; j++) {
        const int k = 13 + j;
        const unsigned want = k < len ? static_cast<unsigned char>(lfn_name(0)[k]) : (k == len ? 0u : 0xFFFFu);
        CHECK_EQ(le16(e + off[j]), want);
    }
    CHECK_EQ((e + 32)[0], 1);                                           // piece 1 follows
    CHECK_EQ(std::memcmp(e + 64, "CAB00   JSN", 11), 0);                // then the short entry
    // the directory needs more than one cluster
    const std::uint8_t* fat = &v[1 * 512];
    CHECK_EQ(le16(fat + 2 * dcl) == 0xFFFFu, 0);
    CHECK_EQ(le16(fat + 2 * dcl), dcl + 1);
    // a child far into the second cluster is reachable: CAB39
    bool found = false;
    for (unsigned c = dcl; c < dcl + 4; c++) {
        const std::uint8_t* d = &v[(97 + (c - 2) * 2) * 512];
        for (int s = 0; s < 32; s++)
            if (std::memcmp(d + 32 * s, "CAB39   JSN", 11) == 0) found = true;
    }
    CHECK_EQ(found, 1);
    if (const char* dd = std::getenv("K2_DUMP")) {
        char path[256];
        std::snprintf(path, sizeof path, "%s/vfat_lfn.img", dd);
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(v.data(), 1, v.size(), f); std::fclose(f); }
    }
}

static void test_dir_limits() {
    // root overflow: 512 entries incl. label
    static std::array<FileSpec, 200> many;
    static const char nm[] = "A-very-long-file-name-over-26-chars.dat";
    for (auto& f : many) f = {name83("N.BIN"), -1, false, static_cast<std::uint8_t>(std::strlen(nm)), 0, 1};
    Image img;
    CHECK_EQ(img.build(many), 0);       // 200 * (1 + 3) = 800 > 511
}

static void test_dump() {
    Image img;
    img.build(kSpecs);
    const char* dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char path[256];
    std::snprintf(path, sizeof path, "%s/vfat.img", dir);
    if (FILE* f = std::fopen(path, "wb")) {
        const auto v = render(img);
        std::fwrite(v.data(), 1, v.size(), f);
        std::fclose(f);
    }
}

int main() {
    test_name83();
    test_geometry_and_boot();
    test_fat_and_directory();
    test_file_data();
    test_limits();
    test_long_names();
    test_dir_limits();
    test_dump();
    return test_report("test_vfat");
}
