// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the memory readout's pure part (include/k2/core/memstat.hpp, src/core/memstat.cpp): the stack low-water
// levels of the file storage (the one figure the user can act on), the row texts with their units and widths, and the footer.
#include <cstring>
#include <string>

#include "k2/core/memstat.hpp"
#include "test_util.h"

using namespace k2::memstat;

#define CHECK(c) CHECK_COND(c)

static Snapshot sample() {
    Snapshot s;
    s.ram_total = 98304; s.ram_static = 64412;
    s.psp_size = 10944; s.psp_now = 9800; s.psp_low = 7500;
    s.msp_size = 1344; s.msp_low = 1100;
    s.fs_blocks = 145; s.fs_used = 12; s.fs_block_bytes = 2048; s.fs_valid = true;
    s.image_bytes = 695000; s.flash_bytes = 718848;
    s.ok = true;
    return s;
}

static std::string text(Row r, const Snapshot& s) {
    char buf[64];
    const std::size_t n = row_text(r, s, std::span<char>(buf, sizeof buf));
    return std::string(buf, n);
}

// Storage: caution under kFsCautionPct (20 %) of the volume free, error under kFsErrorPct (10 %); the edges are exclusive.
static void test_levels() {
    CHECK_EQ(kFsCautionPct, 20u);
    CHECK_EQ(kFsErrorPct, 10u);
    CHECK_EQ(static_cast<int>(level_for_fs(20, 100)), static_cast<int>(Level::Ok));
    CHECK_EQ(static_cast<int>(level_for_fs(19, 100)), static_cast<int>(Level::Caution));
    CHECK_EQ(static_cast<int>(level_for_fs(10, 100)), static_cast<int>(Level::Caution));
    CHECK_EQ(static_cast<int>(level_for_fs(9, 100)), static_cast<int>(Level::Error));
    CHECK_EQ(static_cast<int>(level_for_fs(0, 100)), static_cast<int>(Level::Error));
    CHECK_EQ(static_cast<int>(level_for_fs(30, 146)), static_cast<int>(Level::Ok));        // the K2's 146-block volume
    CHECK_EQ(static_cast<int>(level_for_fs(29, 146)), static_cast<int>(Level::Caution));   // 19.9 %
    CHECK_EQ(static_cast<int>(level_for_fs(15, 146)), static_cast<int>(Level::Caution));
    CHECK_EQ(static_cast<int>(level_for_fs(14, 146)), static_cast<int>(Level::Error));     // 9.6 %
    CHECK_EQ(static_cast<int>(level_for_fs(0, 0)), static_cast<int>(Level::Ok));           // no volume: nothing to warn about
}

static void test_rows() {
    Snapshot s = sample();
    CHECK(text(Row::StackNow, s) == "9800 B");
    CHECK(text(Row::StackLow, s) == "7500 B");
    CHECK(text(Row::IrqLow, s) == "1100 B");
    CHECK(text(Row::RamStatic, s) == "62.9 of 96.0 KB");
    CHECK(text(Row::FsUsed, s) == "24 KB (12 blk)");
    CHECK(text(Row::FsFree, s) == "266 KB (133 blk)");
    CHECK(text(Row::Image, s) == "678.7 KB");
    CHECK(text(Row::ImageFree, s) == "23.3 KB");
    CHECK(text(Row::Heap, s) == "none");
    CHECK(std::strcmp(row_label(Row::StackLow), "Stack low") == 0);
    CHECK(std::strcmp(row_label(Row::ImageFree), "Image free") == 0);
}

static void test_unknown() {
    Snapshot s;                                           // before the first poll
    for (int i = 0; i < kRows; i++) {
        const Row r = static_cast<Row>(i);
        if (r == Row::Heap) continue;
        CHECK(text(r, s) == "--");
        CHECK_EQ(static_cast<int>(row_level(r, s)), static_cast<int>(Level::Ok));   // an unknown stack is not an alarm
    }
    CHECK_EQ(static_cast<int>(footer_level(s)), static_cast<int>(Level::Ok));
    s = sample();
    s.fs_valid = false;                                   // the tree has not been walked yet
    CHECK(text(Row::FsUsed, s) == "--");
    CHECK(text(Row::FsFree, s) == "--");
    CHECK(text(Row::StackLow, s) == "7500 B");
}

static void test_row_level_and_footer() {
    Snapshot s = sample();   // 145 blocks
    char buf[48];
    // The stack rows are plain numbers: no level and no warning footer, however low (user decision 2026-11-14).
    s.psp_low = 10; s.msp_low = 10; s.psp_now = 10;
    for (Row r : {Row::StackNow, Row::StackLow, Row::IrqLow})
        CHECK_EQ(static_cast<int>(row_level(r, s)), static_cast<int>(Level::Ok));
    CHECK_EQ(static_cast<int>(footer_level(s)), static_cast<int>(Level::Ok));
    std::size_t n = footer_text(s, std::span<char>(buf, sizeof buf));
    CHECK(std::string(buf, n) == "Stack never below 10 B");
    s.psp_low = 7500;
    s.fs_used = 116;         // 29 free: 20.0 %, still fine
    CHECK_EQ(static_cast<int>(row_level(Row::FsFree, s)), static_cast<int>(Level::Ok));
    CHECK_EQ(static_cast<int>(footer_level(s)), static_cast<int>(Level::Ok));
    s.fs_used = 117;         // 28 free: 19.3 %
    CHECK_EQ(static_cast<int>(row_level(Row::FsFree, s)), static_cast<int>(Level::Caution));
    CHECK_EQ(static_cast<int>(footer_level(s)), static_cast<int>(Level::Caution));
    n = footer_text(s, std::span<char>(buf, sizeof buf));
    CHECK(std::string(buf, n) == "Storage low: delete cards");
    s.fs_used = 131;         // 14 free: 9.7 %
    CHECK_EQ(static_cast<int>(row_level(Row::FsFree, s)), static_cast<int>(Level::Error));
    CHECK_EQ(static_cast<int>(footer_level(s)), static_cast<int>(Level::Error));
    n = footer_text(s, std::span<char>(buf, sizeof buf));
    CHECK(std::string(buf, n) == "Storage full: delete cards");
    // only the free-space row carries it; a volume not walked yet is no alarm
    for (Row r : {Row::StackNow, Row::StackLow, Row::IrqLow, Row::FsUsed, Row::Image, Row::ImageFree})
        CHECK_EQ(static_cast<int>(row_level(r, s)), static_cast<int>(Level::Ok));
    s.fs_valid = false;
    CHECK_EQ(static_cast<int>(row_level(Row::FsFree, s)), static_cast<int>(Level::Ok));
    CHECK_EQ(static_cast<int>(footer_level(s)), static_cast<int>(Level::Ok));
}

// The widest values any field can take still fit the 16-char value box and the 27-char footer.
static void test_worst_case_widths() {
    Snapshot s = sample();
    s.psp_now = 99999; s.psp_low = 99999; s.msp_low = 99999;                 // 5-digit stack figures
    s.ram_static = 98304; s.ram_total = 98304;
    s.fs_blocks = 999; s.fs_used = 999;
    s.image_bytes = 718848; s.flash_bytes = 718848;
    char buf[64];
    for (int i = 0; i < kRows; i++) {
        const std::size_t n = row_text(static_cast<Row>(i), s, std::span<char>(buf, sizeof buf));
        CHECK(n <= kValueMax);
        CHECK(std::strlen(row_label(static_cast<Row>(i))) <= kLabelMax);
    }
    s.psp_low = 4294967295u;
    CHECK(footer_text(s, std::span<char>(buf, sizeof buf)) <= kFooterMax);
    s.fs_used = s.fs_blocks;   // the storage footers
    CHECK(footer_text(s, std::span<char>(buf, sizeof buf)) <= kFooterMax);
    s.fs_used = s.fs_blocks - s.fs_blocks / 6;
    CHECK(footer_text(s, std::span<char>(buf, sizeof buf)) <= kFooterMax);
    // a destination smaller than the text is cut, never overrun
    char tiny[4];
    CHECK(row_text(Row::RamStatic, sample(), std::span<char>(tiny, sizeof tiny)) <= sizeof tiny);
}

static void test_free_helpers() {
    Snapshot s = sample();
    CHECK_EQ(fs_used_bytes(s), 24576u);
    CHECK_EQ(fs_free_blocks(s), 133u);
    CHECK_EQ(image_free_bytes(s), 23848u);
    s.fs_used = 200;                                       // never underflows
    CHECK_EQ(fs_free_blocks(s), 0u);
    s.image_bytes = 800000;
    CHECK_EQ(image_free_bytes(s), 0u);
}

int main() {
    test_levels();
    test_rows();
    test_unknown();
    test_row_level_and_footer();
    test_worst_case_widths();
    test_free_helpers();
    return test_report("test_memstat");
}
