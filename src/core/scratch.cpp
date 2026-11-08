// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// See k2/core/scratch.hpp.
#include "k2/core/scratch.hpp"

#include "k2/core/log.hpp"
#include "k2/drivers/regs.hpp"

namespace k2::scratch {
namespace {
alignas(kAlign) std::uint8_t g_arena[kSize];
const char* g_owner = nullptr;
ViolationHandler g_handler = nullptr;

bool in_exception() {
#if defined(__arm__)
    return (reg::scb().ICSR & 0x1FFu) != 0;      // VECTACTIVE: non-zero inside any exception handler
#else
    return false;
#endif
}

void violate(const char* want, bool isr) {
    const Violation v{want, g_owner, isr};
    if (g_handler != nullptr) {
        g_handler(v);
        return;
    }
    K2LOG("[SCRATCH] %s refused: %s\n", want, isr ? "taken from an exception handler" : g_owner != nullptr ? g_owner : "?");
    __builtin_trap();      // UsageFault -> HardFault -> the [FAULT] report
}
}  // namespace

void set_violation_handler(ViolationHandler h) { g_handler = h; }

void acquire(const char* owner_tag) {
    const bool isr = in_exception();
    if (isr || g_owner != nullptr) violate(owner_tag, isr);
    g_owner = owner_tag;
}

void release() { g_owner = nullptr; }
bool held() { return g_owner != nullptr; }
const char* owner() { return g_owner; }

std::uint8_t* arena() {
    if (g_owner == nullptr) violate("(no lease)", false);
    return g_arena;
}

}  // namespace k2::scratch
