// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the Xiaomi unlock diff (k2/chargertest/mi_unlock.hpp). The lists are the raw words of hardware runs.
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/chargertest/mi_unlock.hpp"
#include "test_util.h"

namespace mu = k2::mi_unlock;

namespace {
// (a) Xiaomi 20K bank, 5 A cable identity (runs/xiaomi20k/2026-11-18-pd-epr-5a): PPS 5-11 V 3 A -> 5 A after the chain. PDO 1
// also lost its DRD bit, which is no unlock.
constexpr std::array<uint32_t, 7> kBankBefore = {0x0201912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000640C8, 0xC0DC323C, 0xC1903228};
constexpr std::array<uint32_t, 7> kBankAfter = {0x0001912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000640C8, 0xC0DC3264, 0xC1903228};
// (b) Baseus FH22 2C1A (runs/newcharger-2026-11-17/mi-fix-cuk6a): the chain completes and the list stays the same.
constexpr std::array<uint32_t, 6> kFh22 = {0x0A81912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC8DC3264, 0xC9A43264};
// (c) CUKTECH 120 W (runs/xiaomi-handoff-2026-11-17/reference-cuktech-c1-6a-worked): both APDOs 5 A -> 6 A.
constexpr std::array<uint32_t, 7> kCukBefore = {0x0A01912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000641F4, 0xC8DC3264, 0xC9903264};
constexpr std::array<uint32_t, 7> kCukAfter = {0x0A01912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000641F4, 0xC8DC3278, 0xC9903278};

bool text_is(const k2::step_label::Text& t, std::string_view want) { return t.view() == want; }
}  // namespace

static void test_bank_pps_3a_to_5a_is_one_mode() {
    const mu::Diff d = mu::diff(kBankBefore, kBankAfter);
    CHECK_EQ(int(d.n), 1);
    CHECK_EQ(d.modes[0].before, 0xC0DC323Cu);
    CHECK_EQ(d.modes[0].after, 0xC0DC3264u);
    CHECK_EQ(int(d.modes[0].pos), 6);
    const mu::Point p = mu::test_point(d.modes[0].after);
    CHECK_EQ(int(p.mv), 11000); CHECK_EQ(int(p.ma), 5000);
    CHECK_EQ(mu::point_w(d.modes[0].after), 55u);
    CHECK_EQ(text_is(mu::mode_text(d.modes[0]), "PPS 5-11V 3A\xE2\x86\x92" "5A"), true);
}

static void test_fh22_unchanged_list_has_no_mode() {
    CHECK_EQ(int(mu::diff(kFh22, kFh22).n), 0);
}

static void test_cuktech_both_apdos_5a_to_6a() {
    const mu::Diff d = mu::diff(kCukBefore, kCukAfter);
    CHECK_EQ(int(d.n), 2);
    CHECK_EQ(int(d.modes[0].pos), 6); CHECK_EQ(int(d.modes[1].pos), 7);
    CHECK_EQ(text_is(mu::mode_text(d.modes[0]), "PPS 5-11V 5A\xE2\x86\x92" "6A"), true);
    CHECK_EQ(text_is(mu::mode_text(d.modes[1]), "PPS 5-20V 5A\xE2\x86\x92" "6A"), true);
    const mu::Point p = mu::test_point(d.modes[0].after);
    CHECK_EQ(int(p.mv), 11000); CHECK_EQ(int(p.ma), 6000);
    CHECK_EQ(mu::point_w(d.modes[1].after), 120u);
}

// A fixed PDO whose current rose (CUKTECH's 20 V 3 A -> 5 A on another attach) is a mode at its voltage and new current.
static void test_fixed_current_raise() {
    const std::array<uint32_t, 2> before = {0x0001912C, 0x0006412C};   // 5 V 3 A, 20 V 3 A
    const std::array<uint32_t, 2> after = {0x0001912C, 0x000641F4};    // 5 V 3 A, 20 V 5 A
    const mu::Diff d = mu::diff(before, after);
    CHECK_EQ(int(d.n), 1);
    CHECK_EQ(int(mu::test_point(d.modes[0].after).mv), 20000);
    CHECK_EQ(int(mu::test_point(d.modes[0].after).ma), 5000);
    CHECK_EQ(text_is(mu::mode_text(d.modes[0]), "PD 20V 3A\xE2\x86\x92" "5A"), true);
}

// A PDO the list did not carry before is a new mode; one inserted ahead of ours does not shift the matching.
static void test_a_new_pdo_is_a_mode_and_slots_do_not_matter() {
    const std::array<uint32_t, 3> before = {0x0001912C, 0x0002D12C, 0xC0DC323C};                  // 5 V, 9 V, PPS 5-11 V 3 A
    const std::array<uint32_t, 4> after = {0x0001912C, 0x0002D12C, 0xC1903278, 0xC0DC323C};       // + PPS 5-20 V 6 A ahead
    const mu::Diff d = mu::diff(before, after);
    CHECK_EQ(int(d.n), 1);
    CHECK_EQ(d.modes[0].before, 0u);
    CHECK_EQ(int(d.modes[0].pos), 3);
    CHECK_EQ(text_is(mu::mode_text(d.modes[0]), "PPS 5-20V 6A new"), true);
    CHECK_EQ(text_is(mu::mode_text(d.modes[0], false), "PPS 5-20V 6A"), true);
}

// A PDO that went down is no unlock, and neither is a PDO that left the list.
static void test_a_pdo_that_went_down_is_no_mode() {
    const std::array<uint32_t, 3> before = {0x0001912C, 0x000641F4, 0xC0DC3264};   // 20 V 5 A, PPS 5-11 V 5 A
    const std::array<uint32_t, 2> after = {0x0001912C, 0x0006412C};                 // 20 V 3 A, the APDO gone
    CHECK_EQ(int(mu::diff(before, after).n), 0);
    CHECK_EQ(mu::raised(0x000641F4, 0x0006412C), false);
    CHECK_EQ(mu::raised(0x0006412C, 0x000641F4), true);
}

// A PPS range that grew at the top is a raise of the same APDO (it shares the minimum), not a new one.
static void test_a_grown_range_is_a_raise() {
    const std::array<uint32_t, 1> before = {0xC0DC323C};   // PPS 5-11 V 3 A
    const std::array<uint32_t, 1> after = {0xC190323C};    // PPS 5-20 V 3 A
    const mu::Diff d = mu::diff(before, after);
    CHECK_EQ(int(d.n), 1);
    CHECK_EQ(d.modes[0].before, 0xC0DC323Cu);
    CHECK_EQ(text_is(mu::mode_text(d.modes[0]), "PPS 5-11V\xE2\x86\x92" "5-20V 3A"), true);
    CHECK_EQ(int(mu::test_point(d.modes[0].after).mv), 20000);
}

// EPR lists: only the SPR part counts. EPR PDOs heard after the chain (positions 8..) and the zero words that pad an EPR list's
// SPR part are no new modes.
static void test_epr_positions_and_padding_are_ignored() {
    std::array<uint32_t, 10> after{};
    for (size_t i = 0; i < kFh22.size(); ++i) after[i] = kFh22[i];
    after[7] = 0x000AF1F4;   // EPR fixed 28 V 5 A at position 8 (6 and 7 are the zero padding)
    after[8] = 0xC8DC3264;   // something APDO-shaped past position 7
    CHECK_EQ(int(mu::diff(kFh22, after).n), 0);
    // and the same SPR raise is still seen behind an EPR tail
    std::array<uint32_t, 9> cuk{};
    for (size_t i = 0; i < kCukAfter.size(); ++i) cuk[i] = kCukAfter[i];
    cuk[7] = 0x000AF1F4;
    CHECK_EQ(int(mu::diff(kCukBefore, cuk).n), 2);
}

static void test_chain_codes() {
    CHECK_EQ(int(mu::chain_of(mu::chain_code(false, false, 0, 0))), int(mu::Chain::NoSvid));
    CHECK_EQ(int(mu::chain_of(mu::chain_code(true, false, 0, 0))), int(mu::Chain::NoAnswer));
    const uint8_t stalled = mu::chain_code(true, false, 5, 0);
    CHECK_EQ(int(mu::chain_of(stalled)), int(mu::Chain::Stalled));
    CHECK_EQ(int(mu::chain_step(stalled)), 5);
    CHECK_EQ(int(mu::chain_of(mu::chain_code(true, true, 8, 8))), int(mu::Chain::Verified));
    CHECK_EQ(int(mu::chain_of(mu::chain_code(true, true, 10, 10))), int(mu::Chain::Finished));
    CHECK_EQ(mu::chain_done(mu::chain_code(true, true, 8, 8)), true);
    CHECK_EQ(mu::chain_done(stalled), false);
    for (uint8_t c = 0; c < 32; ++c) CHECK_EQ(mu::chain_code(true, false, c > 10 ? 0 : c, 0) < 32, true);   // five detail bits
    CHECK_EQ(text_is(mu::chain_text(stalled), "failed at step 5"), true);
    CHECK_EQ(text_is(mu::chain_text(mu::kCodeVerified), "done, verified"), true);
    CHECK_EQ(text_is(mu::chain_text(mu::kCodeFinished), "done via 020A"), true);
    CHECK_EQ(text_is(mu::chain_text(mu::kCodeNoAnswer), "no answer"), true);
    CHECK_EQ(text_is(mu::chain_text(mu::kCodeNoSvid), "no 0x2717 SVID"), true);
}

int main() {
    test_bank_pps_3a_to_5a_is_one_mode();
    test_fh22_unchanged_list_has_no_mode();
    test_cuktech_both_apdos_5a_to_6a();
    test_fixed_current_raise();
    test_a_new_pdo_is_a_mode_and_slots_do_not_matter();
    test_a_pdo_that_went_down_is_no_mode();
    test_a_grown_range_is_a_raise();
    test_epr_positions_and_padding_are_ignored();
    test_chain_codes();
    return test_report("test_mi_unlock");
}
