// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// AT32F403A flash controller (STM32F1-compatible layout at 0x40022000), program-only.
// Policy lives in k2/drivers/flash.hpp: only the DFU marker half-words, only 0x0000, are accepted.
//
// Bank decision: the application (0x08004000) and this code are in bank 1. A program/erase
// stalls instruction fetches from the bank being written, and the vector fetch of an interrupt
// would stall too. So the whole sequence (unlock, program, wait, lock) is one function placed
// in RAM (.ramfunc, copied with .data by Reset_Handler) and runs with PRIMASK set. It touches
// the controller through raw volatile pointers and calls nothing, so no flash fetch happens
// while the controller is busy.
#include "k2/drivers/flash.hpp"

#include <cstdint>

#include "k2/drivers/regs.hpp"

namespace k2::flash {

namespace {

constexpr std::uintptr_t kFlashBase = 0x40022000;
constexpr std::uint32_t kKey1 = 0x45670123, kKey2 = 0xCDEF89AB;
// Register offsets: ACR 0x00, UNLOCK 0x04, USD_UNLOCK 0x08, STS 0x0C, CTRL 0x10, ADDR 0x14.
constexpr std::uint32_t kStsBusy = 1u << 0, kStsProgErr = 1u << 2, kStsWrProt = 1u << 4, kStsEop = 1u << 5;
constexpr std::uint32_t kCtrlProg = 1u << 0, kCtrlLock = 1u << 7;
constexpr std::uint32_t kWaitLoops = 2'000'000;   // far above a half-word program (~50 us)
constexpr std::uint32_t kCrmCtrlHick = 1u << 0, kCrmCtrlHickStable = 1u << 1;

// Runs from RAM with interrupts off. Returns a Status value.
__attribute__((section(".ramfunc"), noinline, used))
std::uint8_t ram_program(std::uint32_t addr, std::uint16_t value) {
    volatile std::uint32_t* const unlock = reinterpret_cast<volatile std::uint32_t*>(kFlashBase + 0x04);
    volatile std::uint32_t* const sts = reinterpret_cast<volatile std::uint32_t*>(kFlashBase + 0x0C);
    volatile std::uint32_t* const ctrl = reinterpret_cast<volatile std::uint32_t*>(kFlashBase + 0x10);

    std::uint32_t primask;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");

    std::uint8_t st = static_cast<std::uint8_t>(Status::Ok);
    std::uint32_t n;

    for (n = kWaitLoops; (*sts & kStsBusy) && n; n--) {}
    if (n == 0) st = static_cast<std::uint8_t>(Status::Busy);

    if (st == static_cast<std::uint8_t>(Status::Ok) && (*ctrl & kCtrlLock)) {
        *unlock = kKey1;
        *unlock = kKey2;
        if (*ctrl & kCtrlLock) st = static_cast<std::uint8_t>(Status::KeyFail);
    }

    if (st == static_cast<std::uint8_t>(Status::Ok)) {
        *sts = kStsEop | kStsProgErr | kStsWrProt;          // write-1-to-clear stale flags
        *ctrl |= kCtrlProg;
        *reinterpret_cast<volatile std::uint16_t*>(addr) = value;
        for (n = kWaitLoops; (*sts & kStsBusy) && n; n--) {}
        const std::uint32_t result = *sts;
        if (n == 0) st = static_cast<std::uint8_t>(Status::Busy);
        else if (result & kStsWrProt) st = static_cast<std::uint8_t>(Status::WriteProtect);
        else if (result & kStsProgErr) st = static_cast<std::uint8_t>(Status::ProgramError);
        *sts = kStsEop | kStsProgErr | kStsWrProt;
        *ctrl &= ~kCtrlProg;
    }

    if (!(*ctrl & kCtrlLock)) *ctrl |= kCtrlLock;           // always leave the controller locked
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
    return st;
}

struct RealFlash {
    Status program(std::uint32_t addr, std::uint16_t value) {
        // Flash programming needs the internal 8 MHz oscillator running (the USB clock
        // recovery keeps it on already; enabling it again is harmless).
        auto& crm = reg::crm();
        crm.CTRL |= kCrmCtrlHick;
        std::uint32_t n = kWaitLoops;
        while (!(crm.CTRL & kCrmCtrlHickStable) && --n) {}
        if (n == 0) return Status::Busy;
        return static_cast<Status>(ram_program(addr, value));
    }
    std::uint16_t read(std::uint32_t addr) const { return *reinterpret_cast<const volatile std::uint16_t*>(addr); }
};

}  // namespace

// ---- Store region: erase and program on either bank ---------------------------------------------
//
// The AT32F403A 1 MB parts are laid out like the STM32F10x XL: a second register set for the
// upper 512 KB (UNLOCK2 0x44, STS2 0x4C, CTRL2 0x50, ADDR2 0x54, same bit positions as bank 1).
// The store region (0x080B3000..) is in that upper half. This register layout is inferred
// (not checked against a datasheet), so the first successful
// access remembers which register set worked: bank 2 is tried first for addresses >= 0x08080000
// and, if the sector does not change, once more through the bank 1 registers. g_store_bank tells
// (2, 1 or 0 = not yet known) and is logged by the store commands.
namespace {

constexpr std::uint32_t kBank2Start = 0x08080000;
constexpr std::uint32_t kCtrlSecErase = 1u << 1, kCtrlStart = 1u << 6;
constexpr std::uint32_t kEraseLoops = 20'000'000;   // a 2 KB sector erases in tens of ms

enum : std::uint32_t { kOpProgram = 0, kOpErase = 1 };

// Runs from RAM with interrupts off. `regs` is the controller base plus 0 (bank 1) or 0x40 (bank 2).
__attribute__((section(".ramfunc"), noinline, used))
std::uint8_t ram_store_op(std::uint32_t op, std::uint32_t regs, std::uint32_t addr, std::uint16_t value) {
    volatile std::uint32_t* const unlock = reinterpret_cast<volatile std::uint32_t*>(regs + 0x04);
    volatile std::uint32_t* const sts = reinterpret_cast<volatile std::uint32_t*>(regs + 0x0C);
    volatile std::uint32_t* const ctrl = reinterpret_cast<volatile std::uint32_t*>(regs + 0x10);
    volatile std::uint32_t* const adr = reinterpret_cast<volatile std::uint32_t*>(regs + 0x14);
    const std::uint32_t loops = op == kOpErase ? kEraseLoops : kWaitLoops;

    std::uint32_t primask;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");

    std::uint8_t st = static_cast<std::uint8_t>(Status::Ok);
    std::uint32_t n;

    for (n = kWaitLoops; (*sts & kStsBusy) && n; n--) {}
    if (n == 0) st = static_cast<std::uint8_t>(Status::Busy);

    if (st == static_cast<std::uint8_t>(Status::Ok) && (*ctrl & kCtrlLock)) {
        *unlock = kKey1;
        *unlock = kKey2;
        if (*ctrl & kCtrlLock) st = static_cast<std::uint8_t>(Status::KeyFail);
    }

    if (st == static_cast<std::uint8_t>(Status::Ok)) {
        *sts = kStsEop | kStsProgErr | kStsWrProt;
        if (op == kOpErase) {
            *ctrl |= kCtrlSecErase;
            *adr = addr;
            *ctrl |= kCtrlStart;
        } else {
            *ctrl |= kCtrlProg;
            *reinterpret_cast<volatile std::uint16_t*>(addr) = value;
        }
        for (n = loops; (*sts & kStsBusy) && n; n--) {}
        const std::uint32_t result = *sts;
        if (n == 0) st = static_cast<std::uint8_t>(Status::Busy);
        else if (result & kStsWrProt) st = static_cast<std::uint8_t>(Status::WriteProtect);
        else if (result & kStsProgErr) st = static_cast<std::uint8_t>(Status::ProgramError);
        *sts = kStsEop | kStsProgErr | kStsWrProt;
        *ctrl &= ~(kCtrlProg | kCtrlSecErase | kCtrlStart);
    }

    if (!(*ctrl & kCtrlLock)) *ctrl |= kCtrlLock;
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
    return st;
}

std::uint8_t g_store_bank = 0;

Status hick_on() {
    auto& crm = reg::crm();
    crm.CTRL |= kCrmCtrlHick;
    std::uint32_t n = kWaitLoops;
    while (!(crm.CTRL & kCrmCtrlHickStable) && --n) {}
    return n == 0 ? Status::Busy : Status::Ok;
}

std::uint16_t peek(std::uint32_t addr) { return *reinterpret_cast<const volatile std::uint16_t*>(addr); }

bool sector_blank(std::uint32_t addr) {
    for (std::uint32_t a = addr; a < addr + kSectorSize; a += 2)
        if (peek(a) != 0xFFFF) return false;
    return true;
}

// Register sets to try for an address, in order.
struct Tries { std::uint32_t regs[2]; int n; };
Tries tries_for(std::uint32_t addr) {
    if (addr < kBank2Start) return {{kFlashBase, 0}, 1};
    if (g_store_bank == 1) return {{kFlashBase, 0}, 1};
    if (g_store_bank == 2) return {{kFlashBase + 0x40, 0}, 1};
    return {{kFlashBase + 0x40, kFlashBase}, 2};
}

}  // namespace

std::uint8_t store_bank() { return g_store_bank; }

Status store_erase(std::uint32_t addr) {
    // A file system sector, or a legacy sector whose content proves it is one of our old stores; any other
    // sector (the native pages included) is refused before a register is touched.
    if (!erase_allowed(addr, peek)) return Status::Refused;
    if (const Status h = hick_on(); h != Status::Ok) return h;
    Status st = Status::VerifyFailed;
    const Tries t = tries_for(addr);
    for (int i = 0; i < t.n; i++) {
        st = static_cast<Status>(ram_store_op(kOpErase, t.regs[i], addr, 0));
        if (st == Status::Ok && !sector_blank(addr)) st = Status::VerifyFailed;
        if (st == Status::Ok) {
            if (addr >= kBank2Start && g_store_bank == 0) g_store_bank = t.regs[i] == kFlashBase ? 1 : 2;
            return st;
        }
    }
    return st;
}

Status store_program(std::uint32_t addr, std::span<const std::uint8_t> data) {
    const std::uint32_t len = static_cast<std::uint32_t>((data.size() + 1) & ~std::size_t{1});
    if (!store_allowed(addr, len)) return Status::Refused;
    if (const Status h = hick_on(); h != Status::Ok) return h;
    for (std::size_t i = 0; i < data.size(); i += 2) {
        const std::uint16_t v = static_cast<std::uint16_t>(data[i] | ((i + 1 < data.size() ? data[i + 1] : 0xFF) << 8));
        const std::uint32_t a = addr + static_cast<std::uint32_t>(i);
        if (v == 0xFFFF) continue;
        Status st = Status::VerifyFailed;
        const Tries t = tries_for(a);
        for (int k = 0; k < t.n; k++) {
            st = static_cast<Status>(ram_store_op(kOpProgram, t.regs[k], a, v));
            if (st == Status::Ok && peek(a) != v) st = Status::VerifyFailed;
            if (st == Status::Ok) {
                if (a >= kBank2Start && g_store_bank == 0) g_store_bank = t.regs[k] == kFlashBase ? 1 : 2;
                break;
            }
        }
        if (st != Status::Ok) return st;
    }
    return Status::Ok;
}

const std::uint8_t* store_view(std::uint32_t addr) { return reinterpret_cast<const std::uint8_t*>(addr); }

Status clear_marker() {
    RealFlash hw;
    return flash::clear_marker(hw);
}

bool marker_present() {
    RealFlash hw;
    for (std::uint32_t a : kAllowed)
        if (hw.read(a) != kClearValue) return true;
    return false;
}

}  // namespace k2::flash
