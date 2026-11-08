// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the reset-cause decode (CRM CTRLSTS) and the SRAM window helper.
#include <cstring>

#define K2_HOST_TEST 1
#include "k2/drivers/sysreset.hpp"
#include "test_util.h"

using namespace k2;

int main() {
    char buf[40];
    // power-on: PINRSTF | PORRSTF
    CHECK_EQ(sysreset::describe_flags(0x0C000000u, buf, sizeof buf), 7);
    CHECK_EQ(std::strcmp(buf, "PIN POR"), 0);
    // software reset after power-on keeps the earlier flags until cleared
    sysreset::describe_flags(0x1C000000u, buf, sizeof buf);
    CHECK_EQ(std::strcmp(buf, "PIN POR SW"), 0);
    sysreset::describe_flags(0x24000000u, buf, sizeof buf);
    CHECK_EQ(std::strcmp(buf, "PIN IWDG"), 0);
    sysreset::describe_flags(0x40000000u, buf, sizeof buf);
    CHECK_EQ(std::strcmp(buf, "WWDG"), 0);
    sysreset::describe_flags(0x80000000u, buf, sizeof buf);
    CHECK_EQ(std::strcmp(buf, "LPWR"), 0);
    // no flag set, and low bits (LSI enable/ready) are not reset causes
    sysreset::describe_flags(0x00000003u, buf, sizeof buf);
    CHECK_EQ(std::strcmp(buf, "-"), 0);
    // truncation stays NUL terminated
    char tiny[5];
    sysreset::describe_flags(0x7C000000u, tiny, sizeof tiny);
    CHECK_EQ(tiny[4], 0);
    CHECK_EQ(std::strlen(tiny) <= 4, 1);
    // warm reset: the chip stayed powered. No POR flag and a reset cause (pin, software, either watchdog); a power-on, or
    // no flag at all, is cold. Decides whether the FUSB302's Rd bits are inherited at boot.
    CHECK_EQ(sysreset::warm(0x0C000000u), 0);   // PIN POR
    CHECK_EQ(sysreset::warm(0x14000000u), 1);   // PIN SW
    CHECK_EQ(sysreset::warm(0x24000000u), 1);   // PIN IWDG
    CHECK_EQ(sysreset::warm(0x44000000u), 1);   // PIN WWDG
    CHECK_EQ(sysreset::warm(0x04000000u), 1);   // PIN alone (NRST)
    CHECK_EQ(sysreset::warm(0x1C000000u), 0);   // POR wins over the SW flag it carries
    CHECK_EQ(sysreset::warm(0x00000003u), 0);   // no cause flag: unknown counts as cold
    CHECK_EQ(sysreset::warm(0x80000000u), 0);   // standby wake: the chip is not trusted either
    // mode decode used by DFU_ENTER
    CHECK_EQ(static_cast<int>(sysreset::Mode::Sys), 0);
    CHECK_EQ(static_cast<int>(sysreset::Mode::Iwdg), 1);
    // SRAM span the wipe covers: the 96 KB mapped by default (EOPB0 selects 224 KB; touching the
    // rest without that bus-faulted on the first hardware run)
    CHECK_EQ(sysreset::kSramEnd - sysreset::kSramBase, 96u * 1024u);
    return test_report("test_sysreset");
}
