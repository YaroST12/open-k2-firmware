// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the fault report lines (pure formatting; the handlers are hardware-only).
#include <cstring>

#include "k2/core/fault_calc.hpp"
#include "test_util.h"

using namespace k2;

int main() {
    char buf[64];
    fault::Info i{fault::Kind::Hard, 0x08009123, 0x080091A5, 0x61000000, 0x00000400, 0x40000000,
                  0x20037FC0, 0xE000ED34, 0x20017F00};
    std::size_t n = fault::line_regs(i, buf, sizeof buf);
    CHECK_STR(buf, "[FAULT] HF PC=08009123 LR=080091A5 CFSR=00000400 BFAR=20037FC0");
    CHECK_EQ(n, std::strlen(buf));
    CHECK_EQ(n <= 63, 1);   // one USB log slot is 64 bytes including the NUL
    n = fault::line_status(i, buf, sizeof buf);
    CHECK_STR(buf, "[FAULT] HFSR=40000000 MMFAR=E000ED34 SP=20017F00 PSR=61000000");
    CHECK_EQ(n <= 63, 1);
    // kind tags
    i.kind = fault::Kind::Bus;   fault::line_regs(i, buf, sizeof buf); CHECK_EQ(std::strncmp(buf + 8, "BF ", 3), 0);
    i.kind = fault::Kind::Mem;   fault::line_regs(i, buf, sizeof buf); CHECK_EQ(std::strncmp(buf + 8, "MM ", 3), 0);
    i.kind = fault::Kind::Usage; fault::line_regs(i, buf, sizeof buf); CHECK_EQ(std::strncmp(buf + 8, "UF ", 3), 0);
    i.kind = fault::Kind::Nmi;   fault::line_regs(i, buf, sizeof buf); CHECK_EQ(std::strncmp(buf + 8, "NMI", 3), 0);
    // small buffer: NUL terminated, no overrun
    char tiny[10];
    fault::line_regs(i, tiny, sizeof tiny);
    CHECK_EQ(tiny[9], 0);
    // zero values keep their width
    fault::Info z{fault::Kind::Hard, 0, 0, 0, 0, 0, 0, 0, 0};
    fault::line_regs(z, buf, sizeof buf);
    CHECK_STR(buf, "[FAULT] HF PC=00000000 LR=00000000 CFSR=00000000 BFAR=00000000");
    // debug fault trigger kinds are validated against this table (2 = stack overflow)
    CHECK_EQ(fault::valid_trigger(0), 1);
    CHECK_EQ(fault::valid_trigger(1), 1);
    CHECK_EQ(fault::valid_trigger(2), 1);
    CHECK_EQ(fault::valid_trigger(3), 0);

    // Stack overflow report: tag "SO", same width as the other kinds.
    i.kind = fault::Kind::Stack; fault::line_regs(i, buf, sizeof buf); CHECK_EQ(std::strncmp(buf + 8, "SO ", 3), 0);
    CHECK_EQ(std::strlen(buf) <= 63, 1);

    // Classification: a MemManage fault is a stack overflow when stacking failed (MSTKERR) or a data access
    // hit the guard (DACCVIOL with a valid MMFAR inside it). Anything else keeps its own kind.
    const fault::Guard g{0x20016000, 0x20016080};
    const std::uint32_t DACC = 1u << 1, MSTK = 1u << 4, MMARV = 1u << 7;
    CHECK_EQ(fault::classify(fault::Kind::Mem, MSTK, 0, g) == fault::Kind::Stack, 1);
    CHECK_EQ(fault::classify(fault::Kind::Mem, DACC | MMARV, 0x20016040, g) == fault::Kind::Stack, 1);
    CHECK_EQ(fault::classify(fault::Kind::Mem, DACC | MMARV, 0x20016000, g) == fault::Kind::Stack, 1);   // first byte
    CHECK_EQ(fault::classify(fault::Kind::Mem, DACC | MMARV, 0x2001607F, g) == fault::Kind::Stack, 1);   // last byte
    CHECK_EQ(fault::classify(fault::Kind::Mem, DACC | MMARV, 0x20016080, g) == fault::Kind::Mem, 1);     // just above
    CHECK_EQ(fault::classify(fault::Kind::Mem, DACC | MMARV, 0x20015FFF, g) == fault::Kind::Mem, 1);     // just below
    CHECK_EQ(fault::classify(fault::Kind::Mem, DACC, 0x20016040, g) == fault::Kind::Mem, 1);             // MMFAR not valid
    CHECK_EQ(fault::classify(fault::Kind::Mem, 1u, 0x20016040, g) == fault::Kind::Mem, 1);               // IACCVIOL
    CHECK_EQ(fault::classify(fault::Kind::Bus, MSTK, 0x20016040, g) == fault::Kind::Bus, 1);             // only MemManage
    CHECK_EQ(fault::classify(fault::Kind::Hard, MSTK, 0x20016040, g) == fault::Kind::Hard, 1);
    CHECK_EQ(fault::classify(fault::Kind::Usage, 0, 0, g) == fault::Kind::Usage, 1);
    return test_report("test_fault");
}
