// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::emk_variant: the attempt table, the header builders and the Discover Identity frames of a cable read.
#include <array>

#include "k2/pd/emk_variant.hpp"
#include "test_util.h"

using namespace k2;
namespace ev = k2::emk_variant;

// Named variants (braces would split a macro argument at the comma).
static constexpr ev::Variant kPd3Svdm2{2, 2};   // PD 3.x header, SVDM 2.0
static constexpr ev::Variant kPd3Svdm1{2, 1};   // PD 3.x header, SVDM 1.0
static constexpr ev::Variant kPd2Svdm1{1, 1};   // PD 2.0 header, SVDM 1.0

static void test_table_order() {
    static_assert(ev::kTable.size() == ev::kAttempts);
    CHECK_EQ(ev::kAttempts, 3u);
    CHECK_EQ(ev::kTable[0] == kPd3Svdm2, true);
    CHECK_EQ(ev::kTable[1] == kPd3Svdm1, true);
    CHECK_EQ(ev::kTable[2] == kPd2Svdm1, true);
    CHECK_EQ(ev::kFirst == ev::kTable[0], true);
    // No variant twice.
    for (std::size_t i = 0; i < ev::kAttempts; i++)
        for (std::size_t j = i + 1; j < ev::kAttempts; j++) CHECK_EQ(ev::kTable[i] == ev::kTable[j], false);
}

static void test_vdm_header_carries_the_svdm_version_not_the_pd_revision() {
    CHECK_EQ(ev::vdm_header(0xFF00, 1, kPd3Svdm2), 0xFF00A001u);
    CHECK_EQ(ev::vdm_header(0xFF00, 1, kPd3Svdm1), 0xFF008001u);   // PD 3.x header, SVDM 1.0
    CHECK_EQ(ev::vdm_header(0xFF00, 1, kPd2Svdm1), 0xFF008001u);
    CHECK_EQ(ev::vdm_header(0x22D9, 31, kPd3Svdm2), 0x22D9A01Fu);
    CHECK_EQ(ev::vdm_header(0x22D9, 3, kPd3Svdm1), 0x22D98003u);
}

static void test_pd_header_carries_the_spec_revision() {
    CHECK_EQ(ev::vdm_pd_header(kPd3Svdm2), 0x108Fu);
    CHECK_EQ(ev::vdm_pd_header(kPd3Svdm1), 0x108Fu);
    CHECK_EQ(ev::vdm_pd_header(kPd2Svdm1), 0x104Fu);
}

static void test_frames_match_the_tokens_the_scan_always_sent() {
    // The two frames the scan hard-coded before the table: PD 3.0 + SVDM 2.0 and PD 2.0 + SVDM 1.0.
    const std::array<std::uint8_t, 14> pd3{0x12, 0x12, 0x1B, 0x1B, 0x86, 0x8F, 0x10, 0x01, 0xA0, 0x00, 0xFF, 0xFF, 0x14, 0xFE};
    const std::array<std::uint8_t, 14> pd2{0x12, 0x12, 0x1B, 0x1B, 0x86, 0x4F, 0x10, 0x01, 0x80, 0x00, 0xFF, 0xFF, 0x14, 0xFE};
    CHECK_EQ(ev::discover_identity_frame(kPd3Svdm2) == pd3, true);
    CHECK_EQ(ev::discover_identity_frame(kPd2Svdm1) == pd2, true);
    // The new one: PD 3.x header, SVDM 1.0 VDM header.
    const std::array<std::uint8_t, 14> mix{0x12, 0x12, 0x1B, 0x1B, 0x86, 0x8F, 0x10, 0x01, 0x80, 0x00, 0xFF, 0xFF, 0x14, 0xFE};
    CHECK_EQ(ev::discover_identity_frame(kPd3Svdm1) == mix, true);
}

static void test_variant_text() {
    CHECK_EQ(ev::header_text(kPd3Svdm2) == "PD 3.x", true);
    CHECK_EQ(ev::header_text(kPd2Svdm1) == "PD 2.0", true);
    CHECK_EQ(ev::svdm_text(kPd3Svdm2) == "2.0", true);
    CHECK_EQ(ev::svdm_text(kPd3Svdm1) == "1.0", true);
}

int main() {
    test_table_order();
    test_vdm_header_carries_the_svdm_version_not_the_pd_revision();
    test_pd_header_carries_the_spec_revision();
    test_frames_match_the_tokens_the_scan_always_sent();
    test_variant_text();
    return test_report("test_emk_variant");
}
