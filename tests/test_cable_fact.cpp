// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::cablefact: what a reader of "cable not read" may say about a cable that gave no identity. A silent chip (a pull-down
// on a CC line, or query variants it acknowledged) is not "no e-marker"; a probed cable with no pull-down is "no chip";
// everything else (a source attached, a refused probe, an older firmware) is only "not read".
#include "k2/pd/cable_fact.hpp"
#include "test_util.h"

using namespace k2;
using cablefact::Fact;
using raclass::PullDown;
using emk_attached::Mode;

static Fact f(bool read, Mode m, std::uint8_t acks, PullDown p) {
    return cablefact::classify(read, emk_attached::ReadInfo{m, acks}, p);
}

int main() {
    // A cable that answered is read, whatever else the driver filed.
    CHECK_EQ(int(f(true, Mode::Alone, 0, PullDown::None)), int(Fact::Read));
    CHECK_EQ(int(f(true, Mode::Attached, 2, PullDown::Cc2)), int(Fact::Read));
    // Alone, no answer: the pull-down decides.
    CHECK_EQ(int(f(false, Mode::Alone, 0, PullDown::Cc1)), int(Fact::ChipSilent));
    CHECK_EQ(int(f(false, Mode::Alone, 0, PullDown::Cc2)), int(Fact::ChipSilent));
    CHECK_EQ(int(f(false, Mode::Alone, 0, PullDown::None)), int(Fact::NoChip));
    CHECK_EQ(int(f(false, Mode::Alone, 0, PullDown::Unclear)), int(Fact::NotRead));
    CHECK_EQ(int(f(false, Mode::Alone, 0, PullDown::NotProbed)), int(Fact::NotRead));
    // A chip that acknowledged a query is present even when the probe says none or did not run.
    CHECK_EQ(int(f(false, Mode::Alone, 1, PullDown::NotProbed)), int(Fact::ChipSilent));
    CHECK_EQ(int(f(false, Mode::Alone, 3, PullDown::None)), int(Fact::ChipSilent));
    // With a source attached the pull-down was never probed: an acknowledged query still shows the chip, nothing else does.
    CHECK_EQ(int(f(false, Mode::Attached, 0, PullDown::NotProbed)), int(Fact::NotRead));
    CHECK_EQ(int(f(false, Mode::Attached, 2, PullDown::NotProbed)), int(Fact::ChipSilent));
    // No read since the attach.
    CHECK_EQ(int(f(false, Mode::None, 0, PullDown::NotProbed)), int(Fact::NotRead));
    // The words: one per fact, never "no e-marker" for a chip that is there.
    CHECK_STR(cablefact::name(Fact::Read), "Read");
    CHECK_STR(cablefact::name(Fact::ChipSilent), "Chip silent");
    CHECK_STR(cablefact::name(Fact::NoChip), "No chip");
    CHECK_STR(cablefact::name(Fact::NotRead), "Not read");
    return test_report("test_cable_fact");
}
