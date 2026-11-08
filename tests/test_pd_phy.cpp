// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_phy.hpp"
#include "test_util.h"

using namespace k2::pd::phy;

namespace {
int value_of(std::uint8_t cc, Reg r) {
    for (const RegWrite& w : passive_config(cc)) if (w.reg == r) return w.value;
    return -1;
}
}  // namespace

int main() {
    // Passive register values.
    for (std::uint8_t cc : {0, 1, 2}) {
        CHECK_EQ(value_of(cc, kMask), 0xEF);
        CHECK_EQ(value_of(cc, kMaskA), 0xFC);
        CHECK_EQ(value_of(cc, kMaskB), 0x01);
        CHECK_EQ(value_of(cc, kControl0), 0x00);
        CHECK_EQ(value_of(cc, kControl1), 0x67);
        CHECK_EQ(value_of(cc, kPower), 0x0F);
        CHECK_EQ(value_of(cc, kControl3), 0x07);
        CHECK_EQ(value_of(cc, kSwitches1) & kAutoCrc, 0);   // passive: no GoodCRC from us
        CHECK_EQ(value_of(cc, kSwitches0) & kSw0PullDowns, 0);   // passive: no Rd (plain boot)
        CHECK_EQ(int(passive_config(cc).back().reg), int(kReset));          // PD_RESET comes last
        CHECK_EQ(int(passive_config(cc).back().value), 0x02);
    }
    CHECK_EQ(value_of(1, kSwitches1), 0x21);   // SPECREV=01, TXCC1
    CHECK_EQ(value_of(2, kSwitches1), 0x22);   // SPECREV=01, TXCC2
    CHECK_EQ(value_of(0, kSwitches1), 0x20);
    CHECK_EQ(value_of(1, kSwitches0), 0x04);   // MEAS_CC1, no Rd
    CHECK_EQ(value_of(2, kSwitches0), 0x08);

    // Active: only the Auto-CRC bit differs (0x26 on CC2).
    CHECK_EQ(sw1(2, true), 0x26);
    CHECK_EQ(sw1(1, true), 0x25);
    CHECK_EQ(sw1(2, true) ^ sw1(2, false), kAutoCrc);
    // CONTROL1 follows Auto-CRC: SOP only while we ACK, so a cable's SOP' is never GoodCRC'd by us.
    CHECK_EQ(control1(true), 0x04);
    CHECK_EQ(control1(false), 0x67);
    CHECK_EQ(control1(true, true), 0x05);                   // Auto-CRC plus SOP' receive (armed cable spoof)
    CHECK_EQ(control1(false, true), 0x67);
    CHECK_EQ(control1(true) & (kEnSop | kEnSopDebug), 0);
    // A cable read (our own SOP' Discover Identity, VCONN up) listens on SOP' and SOP'' whatever Auto-CRC says.
    CHECK_EQ(control1(true, false, true), 0x67);
    CHECK_EQ(control1(true, true, true), 0x67);
    CHECK_EQ(control1(false, false, true), 0x67);
    CHECK_EQ(control1(true, false, true) & 0x03, 0x03);     // ENSOP1 | ENSOP2
    return test_report("test_pd_phy");
}
