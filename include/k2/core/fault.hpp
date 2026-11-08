// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// CPU fault handlers (NMI, HardFault, MemManage, BusFault, UsageFault). A fault used to loop in
// Default_Handler with USB attached: a dead device and no information. Now the handler logs two
// [FAULT] lines (stacked PC/LR/PSR, CFSR/HFSR/BFAR/MMFAR, SP), pumps the USB stack by hand for a
// moment (interrupts cannot run at fault priority), detaches from USB, restores the clocks and
// takes a system reset. A second fault inside the handler skips the logging and just resets.
#pragma once
#include <cstdint>

namespace k2::fault {

// Enable the MemManage/BusFault/UsageFault exceptions (SHCSR) so each reports with its own
// status bits instead of escalating to HardFault, and switch on the MPU guard under the application
// stack (k2/drivers/mpu.hpp). Call early in main.
void init();

// Debug trigger for the on-device test of the handler. 0: undefined instruction (UsageFault),
// 1: read of SRAM above the default 96 KB (BusFault if that region is not mapped), 2: recurse until the
// stack overflows into the MPU guard (reports "[FAULT] SO"). Returns when the read did not fault.
void trigger(std::uint8_t kind);

}  // namespace k2::fault
