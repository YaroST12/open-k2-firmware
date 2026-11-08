// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the MPU region arithmetic behind the stack guard (include/k2/drivers/mpu_calc.hpp). The register
// writes themselves are hardware-only; what can be wrong on the bench is the numbers, so they are checked here.
#include "k2/drivers/mpu_calc.hpp"
#include "test_util.h"

using namespace k2;

int main() {
    // Region validity: power-of-two size >= 32, base aligned to the size.
    CHECK_EQ(mpu::valid_region(0x20016000, 128), 1);
    CHECK_EQ(mpu::valid_region(0x20016080, 128), 1);
    CHECK_EQ(mpu::valid_region(0x20016040, 128), 0);    // not aligned to its size
    CHECK_EQ(mpu::valid_region(0x20016000, 96), 0);     // not a power of two
    CHECK_EQ(mpu::valid_region(0x20016000, 16), 0);     // below the 32-byte minimum
    CHECK_EQ(mpu::valid_region(0x20016000, 32), 1);
    CHECK_EQ(mpu::valid_region(0x20016000, 0), 0);
    CHECK_EQ(mpu::valid_region(0x20016000, 256), 1);
    CHECK_EQ(mpu::valid_region(0x20016100, 256), 1);
    CHECK_EQ(mpu::valid_region(0x20016080, 256), 0);

    // SIZE field: region size = 2^(SIZE+1).
    CHECK_EQ(mpu::size_field(32), 4);
    CHECK_EQ(mpu::size_field(128), 6);
    CHECK_EQ(mpu::size_field(256), 7);
    CHECK_EQ(mpu::size_field(4096), 11);

    // RBAR: base with the VALID bit and the region number in the low bits.
    CHECK_EQ(mpu::rbar_value(0x20016000, 0), 0x20016010);
    CHECK_EQ(mpu::rbar_value(0x20016080, 3), 0x20016093);

    // RASR: no access for everyone (AP = 000), execute never, SIZE, ENABLE.
    CHECK_EQ(mpu::rasr_no_access(128), (1u << 28) | (6u << 1) | 1u);
    CHECK_EQ(mpu::rasr_no_access(32), 0x10000009);
    CHECK_EQ((mpu::rasr_no_access(128) >> 24) & 7, 0);   // AP field

    // CTRL: ENABLE plus PRIVDEFENA (privileged code keeps the default map outside the region);
    // HFNMIENA stays clear so the HardFault and NMI handlers run with the MPU off.
    CHECK_EQ(mpu::kCtrlEnable, 5);

    // The DREGION field of TYPE tells how many regions exist; the guard needs one.
    CHECK_EQ(mpu::region_count(0x00000800), 8);
    CHECK_EQ(mpu::region_count(0), 0);
    return test_report("test_mpu_calc");
}
