// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Interrupt-masking critical section (PRIMASK save / cpsid i / restore). Nests: the destructor
// restores the PRIMASK seen on entry, so an inner section never unmasks an outer one. Use it for a
// read-modify-write of a register an ISR also writes (GPIO CRL/CRH: the SysTick load guard reconfigures
// PA8 while the main loop reconfigures other pins of port A). Host builds: no-op.
#pragma once
#include <cstdint>

namespace k2 {

class Critical {
public:
#if defined(__arm__)
    Critical() { asm volatile("mrs %0, primask\n cpsid i" : "=r"(saved_)::"memory"); }
    ~Critical() { asm volatile("msr primask, %0" ::"r"(saved_) : "memory"); }
#else
    Critical() = default;
    ~Critical() = default;
#endif
    Critical(const Critical&) = delete;
    Critical& operator=(const Critical&) = delete;

private:
    [[maybe_unused]] std::uint32_t saved_ = 0;
};

}  // namespace k2
