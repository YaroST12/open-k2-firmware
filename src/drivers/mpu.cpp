// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// See k2/drivers/mpu.hpp.
#include "k2/drivers/mpu.hpp"

#include "k2/drivers/mpu_calc.hpp"
#include "k2/drivers/regs.hpp"

namespace k2::mpu {

bool guard(std::uint32_t base, std::uint32_t size) {
    reg::Mpu& m = reg::mpu();
    if (mpu::region_count(m.TYPE) == 0 || !valid_region(base, size)) return false;
    disable();
    m.RBAR = rbar_value(base, 0);              // VALID: the region number rides in RBAR, RNR is not needed
    m.RASR = rasr_no_access(size);
    m.CTRL = kCtrlEnable;
    __asm volatile("dsb\n isb" ::: "memory");  // the new map applies to everything after this point
    return true;
}

void disable() {
    reg::mpu().CTRL = 0;
    __asm volatile("dsb\n isb" ::: "memory");
}

}  // namespace k2::mpu
