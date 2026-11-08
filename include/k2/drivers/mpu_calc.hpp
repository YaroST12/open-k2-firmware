// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure arithmetic for one Cortex-M MPU region (host tested in tests/test_mpu_calc.cpp). The driver in
// src/drivers/mpu.cpp writes these values to the registers; nothing here touches hardware.
#pragma once
#include <cstdint>

namespace k2::mpu {

constexpr std::uint32_t kMinRegionBytes = 32;

// MPU_CTRL: ENABLE and PRIVDEFENA (privileged code keeps the default memory map outside every region).
// HFNMIENA stays clear, so the HardFault and NMI handlers run with the MPU off.
constexpr std::uint32_t kCtrlEnable = (1u << 0) | (1u << 2);

// A region must be a power of two of at least 32 bytes, and its base aligned to its size.
constexpr bool valid_region(std::uint32_t base, std::uint32_t size) {
    return size >= kMinRegionBytes && (size & (size - 1)) == 0 && (base & (size - 1)) == 0;
}

// MPU_RASR.SIZE: the region covers 2^(SIZE + 1) bytes. `size` must be valid.
constexpr std::uint32_t size_field(std::uint32_t size) {
    std::uint32_t log2 = 0;
    while ((size >> (log2 + 1)) != 0) log2++;
    return log2 - 1;
}

// MPU_RBAR: base address, VALID (bit 4, selects the region number in the low bits).
constexpr std::uint32_t rbar_value(std::uint32_t base, unsigned region) {
    return base | (1u << 4) | (region & 0xF);
}

// MPU_RASR for a region nobody may access: AP = 000 (no access, privileged included), XN, ENABLE.
constexpr std::uint32_t rasr_no_access(std::uint32_t size) {
    return (1u << 28) | (size_field(size) << 1) | 1u;
}

// MPU_TYPE.DREGION (bits 15:8): the number of regions the part implements.
constexpr unsigned region_count(std::uint32_t type) { return (type >> 8) & 0xFF; }

}  // namespace k2::mpu
