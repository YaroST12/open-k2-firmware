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

std::string cap_text(std::uint16_t word, End end, std::uint16_t vmax_mv = 0, std::uint16_t stop_ms = 0,
                     svooc::Abort why = svooc::Abort::None) {
    char b[48];
    sc::format_cap({word, end, vmax_mv, stop_ms, std::uint8_t(why)}, b);
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

// The no-load stop: an adapter that spoke and then went quiet for kQuietMs ended the session while the trigger still listens.
void test_end_of_reads_a_quiet_adapter_as_stopped() {
    sc::Live l; l.running = true; l.commands = 9;
    l.quiet_ms = sc::kQuietMs - 1; CHECK_EQ(int(sc::end_of(l, 4000)), int(End::None));
    l.quiet_ms = sc::kQuietMs;     CHECK_EQ(int(sc::end_of(l, 4000)), int(End::Stopped));
    l.commands = 0;                CHECK_EQ(int(sc::end_of(l, 4000)), int(End::None));   // never spoke: the wakes go on
    l.commands = 9; l.judged = true; CHECK_EQ(int(sc::end_of(l, 4000)), int(End::Judged));
    CHECK(std::strcmp(sc::name(End::Stopped), "stopped") == 0);
}

void test_result_text() {
    // The ask is what the K2 claimed in the protocol, never a drawn current.
    CHECK_STR(cap_text(0x00F, End::Judged).c_str(), "1.5 A claimed");
    CHECK_STR(cap_text(0x028, End::Cap).c_str(), "4.0 A claimed");
    CHECK_STR(cap_text(0x03F, End::Cap).c_str(), "6.3 A claimed");
    CHECK_STR(cap_text(0x06D, End::Judged).c_str(), "10.9 A claimed");
    CHECK_STR(cap_text(0x1FF, End::Judged).c_str(), "51.1 A claimed");
    CHECK_STR(cap_text(0, End::Refused).c_str(), "refused");
    CHECK_STR(cap_text(0, End::NoAdapter).c_str(), "no adapter");
    CHECK_STR(cap_text(0, End::Aborted).c_str(), "aborted, none claimed");
    CHECK_STR(cap_text(0, End::Judged).c_str(), "none claimed");
    CHECK_STR(cap_text(0, End::None).c_str(), "-");
    // The highest VBUS the trigger saw, tenths rounded; "stopped at" the adapter's last edge after the wake.
    CHECK_STR(cap_text(0x06D, End::Judged, 11120).c_str(), "10.9 A claimed, 11.1 V max");
    CHECK_STR(cap_text(0x028, End::Stopped, 9100, 3300).c_str(), "stopped at 3.3 s, 4.0 A claimed, 9.1 V max");
    CHECK_STR(cap_text(0, End::Stopped, 5270, 12500).c_str(), "stopped at 12.5 s, none claimed, 5.3 V max");
    CHECK_STR(cap_text(0x028, End::Stopped, 0, 3349).c_str(), "stopped at 3.3 s, 4.0 A claimed");
    CHECK_STR(cap_text(0, End::Refused, 5270, 3300).c_str(), "refused");   // nothing ran: no numbers
    // An abort says so, with its cause in a short word (hardware 2026-11-15 read "none claimed" for a session the K2 cut).
    CHECK_STR(cap_text(0, End::Aborted, 7983, 1305, svooc::Abort::Sink).c_str(), "aborted: sink, none claimed, 8.0 V max");
    CHECK_STR(cap_text(0x028, End::Aborted, 9100, 0, svooc::Abort::Overvoltage).c_str(), "aborted: over 12 V, 4.0 A claimed, 9.1 V max");
    // The longest phrase still fits the 47 characters of the Results note.
    CHECK_STR(cap_text(0x06D, End::Aborted, 11120, 0, svooc::Abort::OffScript).c_str(),
              "aborted: off-script, 10.9 A claimed, 11.1 V max");
    for (unsigned a = 0; a <= unsigned(svooc::Abort::ContractChanged) + 1; ++a) {
        const char* w = sc::abort_word(std::uint8_t(a));
        CHECK(w != nullptr && std::strlen(w) <= 10);
        CHECK_EQ(int(cap_text(0x06D, End::Aborted, 11120, 0, svooc::Abort(a)).size()) <= 47, true);
    }
    // A short buffer truncates and still terminates.
    char tiny[4];
    CHECK_EQ(int(sc::format_cap({0x06D, End::Judged}, tiny)), 3);
    CHECK(std::strcmp(tiny, "10.") == 0);
    CHECK_EQ(int(sc::format_cap({1, End::Judged}, std::span<char>{})), 0);
    CHECK(std::strlen(sc::name(sc::Mode::UsbA)) > 0);
    CHECK_EQ(int(sc::kModeCount), 3);
    CHECK_EQ(int(sc::kEndCount), 8);
}

}  // namespace

int main() {
    test_decode_is_100ma_per_count();
    test_ladder_judged_needs_every_rung_and_the_last_standing();
    test_end_of_reads_why_the_trigger_stopped();
    test_end_of_reads_a_quiet_adapter_as_stopped();
    test_result_text();
    return test_report("test_svooc_ceiling");
}
