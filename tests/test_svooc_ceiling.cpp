// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger test's SuperVOOC ceiling stage, the pure part (k2/chargertest/svooc_ceiling.hpp): the 100 mA/LSB decode,
// when the ladder is judged, how the stage ended, how the result reads, and the probe responder's word sequence the stage
// relies on (the same Responder the trigger runs; the on-hardware ISR path is not under test here).
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

#include "k2/chargertest/svooc_ceiling.hpp"
#include "test_util.h"

using namespace k2;
namespace sc = k2::svooc_ceiling;
using sc::End;

#define CHECK(c) CHECK_EQ(!!(c), 1)

namespace {

std::string cap_text(std::uint16_t word, End end) {
    char b[24];
    sc::format_cap(word, end, b);
    return b;
}

void test_decode_is_100ma_per_count() {
    CHECK_EQ(sc::ceiling_ma(0x000), 0u);
    CHECK_EQ(sc::ceiling_ma(0x00F), 1500u);
    CHECK_EQ(sc::ceiling_ma(0x028), 4000u);
    CHECK_EQ(sc::ceiling_ma(0x03F), 6300u);
    CHECK_EQ(sc::ceiling_ma(0x06D), 10900u);
    CHECK_EQ(sc::ceiling_ma(0x1FF), 51100u);
    CHECK_EQ(sc::ceiling_ma(0xFFFF), 51100u);   // a 9-bit word: anything above is clamped, never wraps
}

void test_ladder_judged_needs_every_rung_and_the_last_standing() {
    using Ask = svooc::Responder::Ask;
    std::array<Ask, 4> a{Ask{100, 0x00F, 6}, Ask{2000, 0x028, 6}, Ask{4000, 0x03F, 6}, Ask{6000, 0x06D, 3}};
    CHECK(!sc::ladder_judged(std::span<const Ask>(a.data(), 0)));
    CHECK(!sc::ladder_judged(std::span<const Ask>(a.data(), 3)));        // three rungs asked: the fourth is still to come
    CHECK(!sc::ladder_judged(std::span<const Ask>(a.data(), 4)));        // the last stood 3 polls only: not yet accepted
    a[3].after = svooc::kAcceptPolls;
    CHECK(sc::ladder_judged(std::span<const Ask>(a.data(), 4)));
    a[3].after = 200;
    CHECK(sc::ladder_judged(std::span<const Ask>(a.data(), 4)));
}

void test_end_of_reads_why_the_trigger_stopped() {
    sc::Live l;
    // Running: goes on until judged, or the cap plus the exit grace.
    l.running = true;
    CHECK_EQ(int(sc::end_of(l, 0)), int(End::None));
    CHECK_EQ(int(sc::end_of(l, sc::kCapMs + sc::kExitGraceMs - 1)), int(End::None));
    CHECK_EQ(int(sc::end_of(l, sc::kCapMs + sc::kExitGraceMs)), int(End::Cap));
    l.judged = true;
    CHECK_EQ(int(sc::end_of(l, 100)), int(End::Judged));
    // Not running: judged wins, then the abort says what happened.
    l = {};
    l.commands = 5; l.abort = std::uint8_t(svooc::Abort::None);
    CHECK_EQ(int(sc::end_of(l, 5000)), int(End::Aborted));          // stopped without a reason but the adapter had spoken
    l.abort = std::uint8_t(svooc::Abort::TimeCap);
    CHECK_EQ(int(sc::end_of(l, 5000)), int(End::Cap));
    l.abort = std::uint8_t(svooc::Abort::Stopped);
    CHECK_EQ(int(sc::end_of(l, 5000)), int(End::Cancelled));
    l.abort = std::uint8_t(svooc::Abort::OffScript);
    CHECK_EQ(int(sc::end_of(l, 5000)), int(End::Aborted));
    l.abort = std::uint8_t(svooc::Abort::Overvoltage);
    CHECK_EQ(int(sc::end_of(l, 5000)), int(End::Aborted));
    l.abort = std::uint8_t(svooc::Abort::NoAdapter);
    l.commands = 0;
    CHECK_EQ(int(sc::end_of(l, 5000)), int(End::NoAdapter));
    l.abort = std::uint8_t(svooc::Abort::None); l.commands = 0;
    CHECK_EQ(int(sc::end_of(l, 5000)), int(End::NoAdapter));       // never a word: the wake got no answer
    l.judged = true;
    CHECK_EQ(int(sc::end_of(l, 5000)), int(End::Judged));
}

void test_result_text() {
    CHECK(cap_text(0x00F, End::Judged) == "1.5 A asked");
    CHECK(cap_text(0x028, End::Cap) == "4.0 A asked");
    CHECK(cap_text(0x03F, End::Aborted) == "6.3 A asked");
    CHECK(cap_text(0x06D, End::Judged) == "10.9 A asked");
    CHECK(cap_text(0x1FF, End::Judged) == "51.1 A asked");
    CHECK(cap_text(0, End::Refused) == "refused");
    CHECK(cap_text(0, End::NoAdapter) == "no adapter");
    CHECK(cap_text(0, End::Aborted) == "none");
    CHECK(cap_text(0, End::Judged) == "none");
    CHECK(cap_text(0, End::None) == "-");
    // A short buffer truncates and still terminates.
    char tiny[4];
    CHECK_EQ(int(sc::format_cap(0x06D, End::Judged, tiny)), 3);
    CHECK(std::strcmp(tiny, "10.") == 0);
    CHECK_EQ(int(sc::format_cap(1, End::Judged, std::span<char>{})), 0);
    CHECK(std::strlen(sc::name(sc::Mode::UsbA)) > 0);
    CHECK_EQ(int(sc::kModeCount), 3);
    CHECK_EQ(int(sc::kEndCount), 7);
}

}  // namespace

int main() {
    test_decode_is_100ma_per_count();
    test_ladder_judged_needs_every_rung_and_the_last_standing();
    test_end_of_reads_why_the_trigger_stopped();
    test_result_text();
    return test_report("test_svooc_ceiling");
}
