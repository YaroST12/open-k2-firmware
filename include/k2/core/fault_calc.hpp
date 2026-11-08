// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure formatting of the CPU fault report (host tested in tests/test_fault.cpp). Each line fits
// one 64-byte USB log slot (63 characters).
#pragma once
#include <cstddef>
#include <cstdint>

namespace k2::fault {

// Stack is a MemManage fault that classify() below attributes to the stack guard (a stack overflow).
enum class Kind : std::uint8_t { Nmi, Hard, Mem, Bus, Usage, Stack };

struct Info {
    Kind kind;
    std::uint32_t pc, lr, xpsr;      // stacked exception frame
    std::uint32_t cfsr, hfsr, bfar, mmfar;
    std::uint32_t sp;                // stack pointer the frame was read from
};

// DEBUG_FAULT trigger kinds: 0 undefined instruction, 1 read above the default 96 KB of SRAM,
// 2 runaway recursion into the stack guard.
constexpr bool valid_trigger(std::uint8_t kind) { return kind <= 2; }

// The MPU no-access region under the application stack: [lo, hi).
struct Guard {
    std::uint32_t lo, hi;
};

// A MemManage fault is a stack overflow when exception stacking failed (CFSR.MSTKERR: the stack pointer is already
// in the guard) or a data access hit the guard (DACCVIOL with a valid MMFAR inside it). Everything else keeps its kind.
constexpr Kind classify(Kind kind, std::uint32_t cfsr, std::uint32_t mmfar, Guard g) {
    constexpr std::uint32_t kDaccviol = 1u << 1, kMstkerr = 1u << 4, kMmarvalid = 1u << 7;
    if (kind != Kind::Mem) return kind;
    if (cfsr & kMstkerr) return Kind::Stack;
    if ((cfsr & kDaccviol) && (cfsr & kMmarvalid) && mmfar >= g.lo && mmfar < g.hi) return Kind::Stack;
    return kind;
}

namespace detail {
struct Out {
    char* p;
    std::size_t n, len = 0;
    void put(char c) { if (len + 1 < n) p[len++] = c; }
    void str(const char* s) { while (*s) put(*s++); }
    void hex(const char* tag, std::uint32_t v) {
        str(tag);
        for (int s = 28; s >= 0; s -= 4) put("0123456789ABCDEF"[(v >> s) & 0xF]);
    }
    std::size_t done() { if (n) p[len] = '\0'; return len; }
};
inline const char* kind_tag(Kind k) {
    switch (k) {
    case Kind::Nmi: return "NMI";
    case Kind::Hard: return "HF";
    case Kind::Mem: return "MM";
    case Kind::Bus: return "BF";
    case Kind::Usage: return "UF";
    case Kind::Stack: return "SO";
    }
    return "??";
}
}  // namespace detail

// "[FAULT] HF PC=xxxxxxxx LR=xxxxxxxx CFSR=xxxxxxxx BFAR=xxxxxxxx" (62 characters)
inline std::size_t line_regs(const Info& i, char* buf, std::size_t n) {
    detail::Out o{buf, n};
    if (n == 0) return 0;
    o.str("[FAULT] "); o.str(detail::kind_tag(i.kind));
    o.hex(" PC=", i.pc); o.hex(" LR=", i.lr); o.hex(" CFSR=", i.cfsr); o.hex(" BFAR=", i.bfar);
    return o.done();
}

// "[FAULT] HFSR=xxxxxxxx MMFAR=xxxxxxxx SP=xxxxxxxx PSR=xxxxxxxx"
inline std::size_t line_status(const Info& i, char* buf, std::size_t n) {
    detail::Out o{buf, n};
    if (n == 0) return 0;
    o.str("[FAULT]");
    o.hex(" HFSR=", i.hfsr); o.hex(" MMFAR=", i.mmfar); o.hex(" SP=", i.sp); o.hex(" PSR=", i.xpsr);
    return o.done();
}

}  // namespace k2::fault
