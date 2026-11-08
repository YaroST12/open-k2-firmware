// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Memory protection unit: one no-access region, used as the guard under the application stack (link.ld
// .stack_guard). A write that runs off the bottom of the stack then raises MemManage (reported as a stack
// overflow by k2/core/fault.hpp) instead of silently corrupting .bss. The region arithmetic is pure code in
// k2/drivers/mpu_calc.hpp.
#pragma once
#include <cstdint>

namespace k2::mpu {

// Make [base, base + size) inaccessible, privileged code included, in region 0, and switch the MPU on (the rest of
// the address space keeps the default map). Returns false, touching nothing, when the region is not a valid MPU
// region (power-of-two size of at least 32 bytes, base aligned to it) or the part has no MPU.
bool guard(std::uint32_t base, std::uint32_t size);

// Switch the MPU off. The fault handler calls this first, so it can read the stack the fault left behind, and the
// SRAM wipe calls it before it zeroes the guard as well.
void disable();

}  // namespace k2::mpu
